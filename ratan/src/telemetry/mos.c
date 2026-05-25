// SPDX-License-Identifier: GPL-2.0
/*
 * mos.c -- MOS proxy computer (Step 4b/MOS).
 *
 * Per-second per-active-call-flow MOS score so we can quantify
 * "did the video call survive?". Inputs from the SQLite DB the
 * daemon already maintains; output goes back through the telemetry
 * socket as RATAN_EVENT_MOS envelopes so persistence + SSE are
 * symmetric with every other event.
 *
 * MOS approximation (E-model, ITU-T G.107 simplified for Opus):
 *   Id  = 0.024 * lat_ms + 0.11 * max(lat_ms - 177.3, 0)
 *   Ie  = loss_pct * 30
 *   R   = 93.2 - Id - Ie
 *   MOS = clamp(1 + 0.035*R + 7e-6*R*(R-60)*(100-R), 1.0, 4.5)
 *
 * WAN attribution caveat: UDP video doesn't expose which subflow
 * carried each packet. We pick the WAN with lowest avg RTT in the
 * last 1s as a proxy for "the path the user is feeling". Document
 * this so reviewers don't read more precision into it than is there.
 *
 * Filter: a flow is considered an active call if its category contains
 * the configured glob substring (default "VideoCall/" matches Teams,
 * Zoom, Meet, Webex, etc.). A flow is "active" while it has a
 * flow_start without a matching flow_end.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#include <sqlite3.h>

#include "../prober/ratan_proto.h"
#include "mos.h"

#define MOS_TICK_MS         1000   /* 1Hz */
#define ACTIVE_FLOW_MAX     32     /* concurrent calls we track per tick */
#define SAMPLE_WINDOW_NS    1000000000ULL  /* 1s back */

extern void logf_(int prio, const char *fmt, ...);
#define LOGI(...) logf_(LOG_INFO,    __VA_ARGS__)
#define LOGW(...) logf_(LOG_WARNING, __VA_ARGS__)
#define LOGE(...) logf_(LOG_ERR,     __VA_ARGS__)

struct mos_state {
	int                 telem_sock;
	struct sockaddr_un  telem_sa;
	sqlite3            *db;
	char                cat_glob[40];
	volatile sig_atomic_t *stop;
};

static uint64_t now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

/* ---------- math ---------- */

static float compute_mos(double rtt_ms, double loss_pct, double *out_r)
{
	if (rtt_ms < 0) rtt_ms = 0;
	if (loss_pct < 0) loss_pct = 0;
	if (loss_pct > 100) loss_pct = 100;

	double Id = 0.024 * rtt_ms + 0.11 * fmax(rtt_ms - 177.3, 0.0);
	double Ie = loss_pct * 30.0;
	double R  = 93.2 - Id - Ie;
	if (out_r) *out_r = R;

	double mos;
	if (R < 0)   mos = 1.0;
	else if (R > 100) mos = 4.5;
	else {
		mos = 1.0 + 0.035 * R + 7e-6 * R * (R - 60) * (100 - R);
		if (mos < 1.0) mos = 1.0;
		if (mos > 4.5) mos = 4.5;
	}
	return (float)mos;
}

/* ---------- queries ---------- */

/* Pull currently-active VideoCall/* flows: those with a flow_start
 * in the last hour and no flow_end after their latest start. */
static int query_active_calls(struct mos_state *s,
			      struct {
				      char id[64];
				      char cat[40];
			      } *out, int max)
{
	const char *q =
		"SELECT five_tuple, category FROM flows "
		"WHERE event='flow_start' AND category LIKE ? "
		"  AND ts_ns > ? "
		"  AND five_tuple NOT IN ( "
		"      SELECT five_tuple FROM flows f2 "
		"      WHERE f2.event='flow_end' "
		"        AND f2.five_tuple = flows.five_tuple "
		"        AND f2.ts_ns >= flows.ts_ns "
		"  ) "
		"GROUP BY five_tuple "
		"ORDER BY MAX(ts_ns) DESC "
		"LIMIT ?";
	sqlite3_stmt *st;
	if (sqlite3_prepare_v2(s->db, q, -1, &st, NULL) != SQLITE_OK) return 0;

	char pat[64];
	snprintf(pat, sizeof(pat), "%%%s%%", s->cat_glob);
	uint64_t cutoff = now_ns() > 3600ULL * 1000000000ULL
			  ? now_ns() - 3600ULL * 1000000000ULL : 0;

	sqlite3_bind_text (st, 1, pat, -1, SQLITE_TRANSIENT);
	sqlite3_bind_int64(st, 2, (sqlite3_int64)cutoff);
	sqlite3_bind_int  (st, 3, max);

	int n = 0;
	while (n < max && sqlite3_step(st) == SQLITE_ROW) {
		const unsigned char *ft  = sqlite3_column_text(st, 0);
		const unsigned char *cat = sqlite3_column_text(st, 1);
		if (ft)  { strncpy(out[n].id,  (const char *)ft,  sizeof(out[n].id)  - 1);
			   out[n].id[sizeof(out[n].id) - 1] = '\0'; }
		if (cat) { strncpy(out[n].cat, (const char *)cat, sizeof(out[n].cat) - 1);
			   out[n].cat[sizeof(out[n].cat) - 1] = '\0'; }
		n++;
	}
	sqlite3_finalize(st);
	return n;
}

/* Per-WAN aggregates over the last 1s. Returns best (lowest-RTT)
 * WAN's id, RTT, loss%, jitter into out_*. Returns 1 if any data,
 * 0 if no samples in the window. */
static int query_best_wan_window(struct mos_state *s,
				 int *out_wan_id,
				 double *out_rtt_us,
				 double *out_loss_pct,
				 double *out_jitter_us)
{
	const char *q =
		"SELECT wan_id, "
		"       AVG(NULLIF(rtt_us,0)) AS rtt_us, "
		"       100.0 * SUM(CASE WHEN event IN (1,2) THEN 1 ELSE 0 END) "
		"             / COUNT(*) AS loss_pct, "
		"       AVG(jitter_us) AS jitter_us "
		"FROM samples WHERE ts_ns > ? "
		"GROUP BY wan_id "
		"HAVING rtt_us IS NOT NULL "
		"ORDER BY rtt_us ASC LIMIT 1";
	sqlite3_stmt *st;
	if (sqlite3_prepare_v2(s->db, q, -1, &st, NULL) != SQLITE_OK) return 0;
	uint64_t cutoff = now_ns() > SAMPLE_WINDOW_NS
			  ? now_ns() - SAMPLE_WINDOW_NS : 0;
	sqlite3_bind_int64(st, 1, (sqlite3_int64)cutoff);

	int found = 0;
	if (sqlite3_step(st) == SQLITE_ROW) {
		*out_wan_id    = sqlite3_column_int(st, 0);
		*out_rtt_us    = sqlite3_column_double(st, 1);
		*out_loss_pct  = sqlite3_column_double(st, 2);
		*out_jitter_us = sqlite3_column_double(st, 3);
		found = 1;
	}
	sqlite3_finalize(st);
	return found;
}

/* ---------- emit ---------- */

static void emit_mos(struct mos_state *s, const struct ratan_mos_event *m)
{
	struct {
		struct ratan_event_hdr  hdr;
		struct ratan_mos_event  payload;
	} __attribute__((packed)) frame = {
		.hdr = {
			.kind    = RATAN_EVENT_MOS,
			.version = RATAN_PROTO_VERSION,
			.len     = sizeof(struct ratan_mos_event),
		},
		.payload = *m,
	};
	if (sendto(s->telem_sock, &frame, sizeof(frame), MSG_DONTWAIT,
		   (struct sockaddr *)&s->telem_sa, sizeof(s->telem_sa)) < 0) {
		if (errno != EAGAIN && errno != EWOULDBLOCK)
			LOGW("mos: sendto: %s", strerror(errno));
	}
}

/* ---------- main loop ---------- */

static void *mos_main(void *arg)
{
	struct mos_state *s = arg;
	LOGI("mos: starting; category_glob='%s'", s->cat_glob);

	struct {
		char id[64];
		char cat[40];
	} active[ACTIVE_FLOW_MAX];

	while (!*s->stop) {
		struct timespec sleep_ts = {
			.tv_sec  = MOS_TICK_MS / 1000,
			.tv_nsec = (MOS_TICK_MS % 1000) * 1000000L,
		};
		nanosleep(&sleep_ts, NULL);
		if (*s->stop) break;

		int n = query_active_calls(s, active, ACTIVE_FLOW_MAX);
		if (n == 0) continue;

		int wan_id = 0;
		double rtt_us = 0, loss_pct = 0, jitter_us = 0;
		if (!query_best_wan_window(s, &wan_id, &rtt_us, &loss_pct, &jitter_us))
			continue;  /* no prober data yet; nothing to score against */

		double rtt_ms = rtt_us / 1000.0;
		double r;
		float mos = compute_mos(rtt_ms, loss_pct, &r);

		uint64_t ts = now_ns();
		for (int i = 0; i < n; i++) {
			struct ratan_mos_event m = {
				.ts_ns      = ts,
				.wan_id     = (uint8_t)wan_id,
				.rtt_us     = (uint32_t)rtt_us,
				.loss_ppm   = (uint32_t)(loss_pct * 10000.0),
				.jitter_us  = (uint32_t)jitter_us,
				.r_factor   = (float)r,
				.mos        = mos,
			};
			strncpy(m.flow_id,  active[i].id,  sizeof(m.flow_id)  - 1);
			strncpy(m.category, active[i].cat, sizeof(m.category) - 1);
			emit_mos(s, &m);
		}
	}

	LOGI("mos: stopping");
	sqlite3_close(s->db);
	close(s->telem_sock);
	free(s);
	return NULL;
}

int mos_start(const char *telem_sock_path, const char *db_path,
	      const char *category_glob,
	      volatile sig_atomic_t *stop_flag, pthread_t *out_tid)
{
	struct mos_state *s = calloc(1, sizeof(*s));
	if (!s) return -1;

	/* Open the DB READ-ONLY for safety (the daemon is the writer). */
	if (sqlite3_open_v2(db_path, &s->db,
			    SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX,
			    NULL) != SQLITE_OK) {
		LOGE("mos: sqlite3_open(%s): %s", db_path,
		     s->db ? sqlite3_errmsg(s->db) : "no handle");
		if (s->db) sqlite3_close(s->db);
		free(s); return -1;
	}

	s->telem_sock = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	if (s->telem_sock < 0) { sqlite3_close(s->db); free(s); return -1; }
	s->telem_sa.sun_family = AF_UNIX;
	strncpy(s->telem_sa.sun_path, telem_sock_path,
		sizeof(s->telem_sa.sun_path) - 1);
	strncpy(s->cat_glob,
		category_glob && *category_glob ? category_glob : "VideoCall/",
		sizeof(s->cat_glob) - 1);
	s->stop = stop_flag;

	pthread_t tid;
	if (pthread_create(&tid, NULL, mos_main, s) != 0) {
		sqlite3_close(s->db); close(s->telem_sock); free(s); return -1;
	}
	if (out_tid) *out_tid = tid;
	return 0;
}
