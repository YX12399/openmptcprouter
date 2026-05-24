// SPDX-License-Identifier: GPL-2.0
/*
 * ratan_telemetryd.c -- RATAN telemetry daemon (Step 4a core).
 *
 * Responsibilities:
 *   1. Read tagged events from /run/ratan/telemetry.sock (Unix datagram).
 *      Sole reader. Currently handles RATAN_EVENT_SAMPLE/STATE/WEIGHT;
 *      DISCOVERY/FLOW/MOS/INJECT placeholders persist as opaque blobs
 *      until Step 4b's emitters land.
 *   2. Persist to SQLite WAL at /var/lib/ratan/telemetry.db.
 *   3. Serve HTTP on 127.0.0.1:9180:
 *        GET  /healthz
 *        GET  /metrics                       Prometheus exposition
 *        GET  /stream                        SSE live feed of every event
 *        GET  /sessions                      JSON list
 *        POST /sessions {name, metadata?}    start session, return {id, ...}
 *        POST /sessions/<id>/stop            mark ended, compute summary
 *        GET  /sessions/<id>                 JSON details
 *        GET  /sessions/<id>/download        full export as JSON
 *        DEL  /sessions/<id>                 remove session + linked rows
 *
 * Single-threaded poll loop. At our throughput (~160 samples/s aggregate
 * across all WANs) this is plenty; multi-thread complexity is unwarranted.
 *
 * License: GPL-2.0. Statically linked against sqlite3 on OpenWrt
 * (libsqlite3-0); dynamically on dev boxes.
 */

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#include <sqlite3.h>

#include "../prober/ratan_proto.h"
#include "discover.h"
#include "flowtrack.h"
#include "mos.h"

#define DEFAULT_DB_PATH       "/var/lib/ratan/telemetry.db"
#define DEFAULT_SOCK_PATH     "/run/ratan/telemetry.sock"
#define DEFAULT_HTTP_HOST     "127.0.0.1"
#define DEFAULT_HTTP_PORT     9180
#define DEFAULT_SCHEMA_PATH   "/usr/share/ratan/schema.sql"
#define DEFAULT_LEASE_FILE    "/tmp/dhcp.leases"
#define DEFAULT_NDPI_CATS     "/etc/ratan/ndpi-categories.conf"

#define MAX_HTTP_CLIENTS      32
#define HTTP_REQ_MAX          8192
#define SSE_OUT_MAX           32768   /* per-client buffered SSE payload */
#define POLLFD_BASE           3       /* signalfd + telemsock + httpsock */

static volatile sig_atomic_t g_stop;

/* ---------- logging ---------- */

static bool g_foreground;

/* Public (non-static) so discover.c can call it without exposing more API. */
void logf_(int prio, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vsyslog(prio, fmt, ap);
	va_end(ap);
	if (g_foreground) {
		va_start(ap, fmt);
		vfprintf(stderr, fmt, ap);
		fputc('\n', stderr);
		va_end(ap);
	}
}

#define LOGI(...) logf_(LOG_INFO,    __VA_ARGS__)
#define LOGW(...) logf_(LOG_WARNING, __VA_ARGS__)
#define LOGE(...) logf_(LOG_ERR,     __VA_ARGS__)

/* ---------- counters (for /metrics) ---------- */

static struct {
	atomic_ullong samples_total;
	atomic_ullong samples_loss;
	atomic_ullong samples_failover;
	atomic_ullong state_transitions_total;
	atomic_ullong weight_changes_total;
	atomic_ullong discovery_events_total;
	atomic_ullong flow_events_total;
	atomic_ullong mos_events_total;
	atomic_ullong http_requests_total;
	atomic_ullong sse_clients_current;
	atomic_ullong sessions_started_total;
	atomic_ullong sessions_completed_total;
	atomic_ullong bytes_ingested;
} g_metrics;

/* ---------- DB ---------- */

static sqlite3 *g_db;
static sqlite3_stmt *g_st_ins_sample;
static sqlite3_stmt *g_st_ins_state;
static sqlite3_stmt *g_st_ins_weight;
static sqlite3_stmt *g_st_ins_inject;
static sqlite3_stmt *g_st_ins_discovery;
static sqlite3_stmt *g_st_ins_flow;
static sqlite3_stmt *g_st_ins_mos;
static sqlite3_stmt *g_st_ins_session;
static sqlite3_stmt *g_st_get_session;
static sqlite3_stmt *g_st_stop_session;
static sqlite3_stmt *g_st_active_session;

static atomic_int g_active_session_id;  /* 0 means none */

static int db_exec(const char *sql)
{
	char *err = NULL;
	int rc = sqlite3_exec(g_db, sql, NULL, NULL, &err);
	if (rc != SQLITE_OK) {
		LOGE("sqlite3_exec: %s", err ? err : "?");
		sqlite3_free(err);
		return -1;
	}
	return 0;
}

static int read_file_into(const char *path, char **out)
{
	FILE *f = fopen(path, "rb");
	if (!f) return -1;
	fseek(f, 0, SEEK_END);
	long sz = ftell(f);
	fseek(f, 0, SEEK_SET);
	*out = malloc(sz + 1);
	if (!*out) { fclose(f); return -1; }
	if (fread(*out, 1, sz, f) != (size_t)sz) { free(*out); *out = NULL; fclose(f); return -1; }
	(*out)[sz] = '\0';
	fclose(f);
	return 0;
}

static int db_init(const char *path, const char *schema_path)
{
	int rc = sqlite3_open(path, &g_db);
	if (rc != SQLITE_OK) { LOGE("sqlite3_open(%s): %s", path, sqlite3_errmsg(g_db)); return -1; }

	char *schema = NULL;
	if (read_file_into(schema_path, &schema) == 0) {
		if (db_exec(schema) < 0) { free(schema); return -1; }
		free(schema);
	} else {
		LOGW("schema file %s not readable; assuming DB is pre-initialized", schema_path);
	}

	const char *prepares[] = {
		"INSERT INTO samples(ts_ns,wan_id,event,seq,rtt_us,loss_count,jitter_us,session_id) "
		"VALUES (?,?,?,?,?,?,?,?)",

		"INSERT INTO state_transitions(ts_ns,wan_id,from_state,to_state,reason,session_id) "
		"VALUES (?,?,?,?,?,?)",

		"INSERT INTO weights(ts_ns,wan_id,weight,source,session_id) "
		"VALUES (?,?,?,?,?)",

		"INSERT INTO injects(ts_ns,session_id,action,target,detail) "
		"VALUES (?,?,?,?,?)",

		"INSERT INTO discovery(ts_ns,kind,detail) VALUES (?,?,?)",

		"INSERT INTO flows(ts_ns,event,five_tuple,category,mark) "
		"VALUES (?,?,?,?,?)",

		"INSERT INTO mos(ts_ns,flow_id,mos,rtt_ms,loss_pct,jitter_ms,session_id) "
		"VALUES (?,?,?,?,?,?,?)",

		"INSERT INTO sessions(name,started_ns,metadata) VALUES (?,?,?)",
		"SELECT id,name,started_ns,ended_ns,status,metadata,summary FROM sessions WHERE id=?",
		"UPDATE sessions SET ended_ns=?, status=?, summary=? WHERE id=?",
		"SELECT id,started_ns FROM sessions WHERE status='recording' ORDER BY id DESC LIMIT 1",
	};
	sqlite3_stmt **slots[] = {
		&g_st_ins_sample, &g_st_ins_state, &g_st_ins_weight,
		&g_st_ins_inject, &g_st_ins_discovery, &g_st_ins_flow,
		&g_st_ins_mos,
		&g_st_ins_session, &g_st_get_session, &g_st_stop_session,
		&g_st_active_session,
	};
	for (size_t i = 0; i < sizeof(prepares)/sizeof(*prepares); i++) {
		if (sqlite3_prepare_v2(g_db, prepares[i], -1, slots[i], NULL) != SQLITE_OK) {
			LOGE("prepare[%zu]: %s", i, sqlite3_errmsg(g_db));
			return -1;
		}
	}

	/* Detect any leftover active session from previous run -> resume it,
	 * so an unclean shutdown doesn't lose the linkage. */
	if (sqlite3_step(g_st_active_session) == SQLITE_ROW)
		atomic_store(&g_active_session_id, sqlite3_column_int(g_st_active_session, 0));
	sqlite3_reset(g_st_active_session);

	return 0;
}

/* ---------- JSON string escaping ----------
 * Required everywhere we interpolate user-supplied text (inject detail
 * comes from YAML recipes via ratan-test and routinely contains quotes;
 * state.reason / weight.source / etc. are internal but might still grow
 * special chars in future). Truncates at outsz-1; ensures valid JSON. */
static size_t json_escape(char *out, size_t outsz, const char *in)
{
	size_t o = 0;
	for (size_t i = 0; in && in[i] && o + 7 < outsz; i++) {
		unsigned char c = (unsigned char)in[i];
		if (c == '"' || c == '\\') { out[o++] = '\\'; out[o++] = (char)c; }
		else if (c == '\n')        { out[o++] = '\\'; out[o++] = 'n'; }
		else if (c == '\r')        { out[o++] = '\\'; out[o++] = 'r'; }
		else if (c == '\t')        { out[o++] = '\\'; out[o++] = 't'; }
		else if (c < 0x20)         { o += snprintf(out + o, outsz - o, "\\u%04x", c); }
		else                       { out[o++] = (char)c; }
	}
	out[o] = '\0';
	return o;
}

/* ---------- SSE broadcast ---------- */

struct sse_client {
	int  fd;
	char buf[SSE_OUT_MAX];   /* outbound queue */
	size_t buf_len;
};

static struct sse_client g_sse[MAX_HTTP_CLIENTS];
static int g_sse_n;

static void sse_drop(int idx)
{
	if (idx < 0 || idx >= g_sse_n) return;
	close(g_sse[idx].fd);
	g_sse[idx] = g_sse[--g_sse_n];
	atomic_store(&g_metrics.sse_clients_current, g_sse_n);
}

static void sse_broadcast(const char *json, size_t json_len)
{
	for (int i = 0; i < g_sse_n; ) {
		struct sse_client *c = &g_sse[i];
		const char *prefix = "data: ";
		const char *suffix = "\n\n";
		size_t need = strlen(prefix) + json_len + strlen(suffix);

		if (c->buf_len + need > sizeof(c->buf)) {
			LOGW("SSE client backlog overflow; dropping");
			sse_drop(i);
			continue;
		}
		memcpy(c->buf + c->buf_len, prefix, strlen(prefix));
		c->buf_len += strlen(prefix);
		memcpy(c->buf + c->buf_len, json, json_len);
		c->buf_len += json_len;
		memcpy(c->buf + c->buf_len, suffix, strlen(suffix));
		c->buf_len += strlen(suffix);

		/* Try to drain now; defer leftover to next pollout. */
		ssize_t n = send(c->fd, c->buf, c->buf_len, MSG_NOSIGNAL | MSG_DONTWAIT);
		if (n > 0) {
			memmove(c->buf, c->buf + n, c->buf_len - n);
			c->buf_len -= n;
		} else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
			sse_drop(i);
			continue;
		}
		i++;
	}
}

/* ---------- ingest from telemetry.sock ---------- */

static int current_session_or_zero(void)
{
	int s = atomic_load(&g_active_session_id);
	return s;
}

static void on_sample(const struct ratan_sample *s)
{
	atomic_fetch_add(&g_metrics.samples_total, 1);
	if (s->event == RATAN_SAMPLE_LOSS) atomic_fetch_add(&g_metrics.samples_loss, 1);
	if (s->event == RATAN_SAMPLE_FAST_FAILOVER) atomic_fetch_add(&g_metrics.samples_failover, 1);

	sqlite3_reset(g_st_ins_sample);
	sqlite3_bind_int64(g_st_ins_sample, 1, (sqlite3_int64)s->ts_ns);
	sqlite3_bind_int  (g_st_ins_sample, 2, s->wan_id);
	sqlite3_bind_int  (g_st_ins_sample, 3, s->event);
	sqlite3_bind_int64(g_st_ins_sample, 4, (sqlite3_int64)s->seq);
	if (s->rtt_us) sqlite3_bind_int(g_st_ins_sample, 5, s->rtt_us);
	else           sqlite3_bind_null(g_st_ins_sample, 5);
	sqlite3_bind_int  (g_st_ins_sample, 6, s->loss_count);
	sqlite3_bind_int  (g_st_ins_sample, 7, s->jitter_us);
	int sid = current_session_or_zero();
	if (sid) sqlite3_bind_int(g_st_ins_sample, 8, sid);
	else     sqlite3_bind_null(g_st_ins_sample, 8);

	if (sqlite3_step(g_st_ins_sample) != SQLITE_DONE)
		LOGW("ins_sample: %s", sqlite3_errmsg(g_db));

	char j[256];
	int n = snprintf(j, sizeof(j),
		"{\"kind\":\"sample\",\"ts_ns\":%llu,\"wan\":%u,\"event\":%u,"
		"\"seq\":%u,\"rtt_us\":%u,\"loss\":%u,\"jitter_us\":%u}",
		(unsigned long long)s->ts_ns, s->wan_id, s->event,
		s->seq, s->rtt_us, s->loss_count, s->jitter_us);
	if (n > 0) sse_broadcast(j, (size_t)n);
}

static void on_state(const struct ratan_state_event *e)
{
	atomic_fetch_add(&g_metrics.state_transitions_total, 1);
	sqlite3_reset(g_st_ins_state);
	sqlite3_bind_int64(g_st_ins_state, 1, (sqlite3_int64)e->ts_ns);
	sqlite3_bind_int  (g_st_ins_state, 2, e->wan_id);
	sqlite3_bind_text (g_st_ins_state, 3, e->from_state, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text (g_st_ins_state, 4, e->to_state,   -1, SQLITE_TRANSIENT);
	sqlite3_bind_text (g_st_ins_state, 5, e->reason,     -1, SQLITE_TRANSIENT);
	int sid = current_session_or_zero();
	if (sid) sqlite3_bind_int(g_st_ins_state, 6, sid);
	else     sqlite3_bind_null(g_st_ins_state, 6);
	if (sqlite3_step(g_st_ins_state) != SQLITE_DONE)
		LOGW("ins_state: %s", sqlite3_errmsg(g_db));

	char j[384], fs[32], ts[32], rs[96];
	json_escape(fs, sizeof(fs), e->from_state);
	json_escape(ts, sizeof(ts), e->to_state);
	json_escape(rs, sizeof(rs), e->reason);
	int n = snprintf(j, sizeof(j),
		"{\"kind\":\"state\",\"ts_ns\":%llu,\"wan\":%u,"
		"\"from\":\"%s\",\"to\":\"%s\",\"reason\":\"%s\"}",
		(unsigned long long)e->ts_ns, e->wan_id, fs, ts, rs);
	if (n > 0) sse_broadcast(j, (size_t)n);
}

static void on_weight(const struct ratan_weight_event *e)
{
	atomic_fetch_add(&g_metrics.weight_changes_total, 1);
	sqlite3_reset(g_st_ins_weight);
	sqlite3_bind_int64(g_st_ins_weight, 1, (sqlite3_int64)e->ts_ns);
	sqlite3_bind_int  (g_st_ins_weight, 2, e->wan_id);
	sqlite3_bind_int  (g_st_ins_weight, 3, e->weight);
	sqlite3_bind_text (g_st_ins_weight, 4, e->source, -1, SQLITE_TRANSIENT);
	int sid = current_session_or_zero();
	if (sid) sqlite3_bind_int(g_st_ins_weight, 5, sid);
	else     sqlite3_bind_null(g_st_ins_weight, 5);
	if (sqlite3_step(g_st_ins_weight) != SQLITE_DONE)
		LOGW("ins_weight: %s", sqlite3_errmsg(g_db));

	char j[192], src[24];
	json_escape(src, sizeof(src), e->source);
	int n = snprintf(j, sizeof(j),
		"{\"kind\":\"weight\",\"ts_ns\":%llu,\"wan\":%u,"
		"\"weight\":%u,\"source\":\"%s\"}",
		(unsigned long long)e->ts_ns, e->wan_id, e->weight, src);
	if (n > 0) sse_broadcast(j, (size_t)n);
}

static void on_discovery(const struct ratan_discovery_event *e)
{
	atomic_fetch_add(&g_metrics.discovery_events_total, 1);
	sqlite3_reset(g_st_ins_discovery);
	sqlite3_bind_int64(g_st_ins_discovery, 1, (sqlite3_int64)e->ts_ns);
	sqlite3_bind_text (g_st_ins_discovery, 2, e->kind,   -1, SQLITE_TRANSIENT);
	sqlite3_bind_text (g_st_ins_discovery, 3, e->detail, -1, SQLITE_TRANSIENT);
	if (sqlite3_step(g_st_ins_discovery) != SQLITE_DONE)
		LOGW("ins_discovery: %s", sqlite3_errmsg(g_db));

	char j[256], esc_kind[48], esc_det[160];
	json_escape(esc_kind, sizeof(esc_kind), e->kind);
	json_escape(esc_det,  sizeof(esc_det),  e->detail);
	int n = snprintf(j, sizeof(j),
		"{\"kind\":\"discovery\",\"ts_ns\":%llu,"
		"\"sub\":\"%s\",\"detail\":\"%s\"}",
		(unsigned long long)e->ts_ns, esc_kind, esc_det);
	if (n > 0) sse_broadcast(j, (size_t)n);
}

static void on_mos(const struct ratan_mos_event *e)
{
	atomic_fetch_add(&g_metrics.mos_events_total, 1);

	double rtt_ms    = e->rtt_us / 1000.0;
	double loss_pct  = e->loss_ppm / 10000.0;
	double jitter_ms = e->jitter_us / 1000.0;

	sqlite3_reset(g_st_ins_mos);
	sqlite3_bind_int64 (g_st_ins_mos, 1, (sqlite3_int64)e->ts_ns);
	sqlite3_bind_text  (g_st_ins_mos, 2, e->flow_id, -1, SQLITE_TRANSIENT);
	sqlite3_bind_double(g_st_ins_mos, 3, e->mos);
	sqlite3_bind_double(g_st_ins_mos, 4, rtt_ms);
	sqlite3_bind_double(g_st_ins_mos, 5, loss_pct);
	sqlite3_bind_double(g_st_ins_mos, 6, jitter_ms);
	int sid = current_session_or_zero();
	if (sid) sqlite3_bind_int(g_st_ins_mos, 7, sid);
	else     sqlite3_bind_null(g_st_ins_mos, 7);
	if (sqlite3_step(g_st_ins_mos) != SQLITE_DONE)
		LOGW("ins_mos: %s", sqlite3_errmsg(g_db));

	char j[512], esc_fid[140], esc_cat[80];
	json_escape(esc_fid, sizeof(esc_fid), e->flow_id);
	json_escape(esc_cat, sizeof(esc_cat), e->category);
	int n = snprintf(j, sizeof(j),
		"{\"kind\":\"mos\",\"ts_ns\":%llu,\"flow\":\"%s\",\"category\":\"%s\","
		"\"wan\":%u,\"mos\":%.3f,\"r\":%.2f,"
		"\"rtt_ms\":%.2f,\"loss_pct\":%.4f,\"jitter_ms\":%.2f}",
		(unsigned long long)e->ts_ns, esc_fid, esc_cat, e->wan_id,
		(double)e->mos, (double)e->r_factor,
		rtt_ms, loss_pct, jitter_ms);
	if (n > 0) sse_broadcast(j, (size_t)n);
}

static void on_flow(const struct ratan_flow_event *e)
{
	atomic_fetch_add(&g_metrics.flow_events_total, 1);

	const char *event_str =
		e->event == RATAN_FLOW_START  ? "flow_start" :
		e->event == RATAN_FLOW_END    ? "flow_end"   :
		e->event == RATAN_FLOW_UPDATE ? "flow_update" : "flow_?";
	const char *proto_str =
		e->proto == 6   ? "tcp"   :
		e->proto == 17  ? "udp"   :
		e->proto == 1   ? "icmp"  :
		e->proto == 58  ? "icmp6" :
		e->proto == 132 ? "sctp"  : "ip";

	char five[160];
	snprintf(five, sizeof(five), "%s:%s->%s", proto_str, e->src, e->dst);

	sqlite3_reset(g_st_ins_flow);
	sqlite3_bind_int64(g_st_ins_flow, 1, (sqlite3_int64)e->ts_ns);
	sqlite3_bind_text (g_st_ins_flow, 2, event_str,    -1, SQLITE_TRANSIENT);
	sqlite3_bind_text (g_st_ins_flow, 3, five,         -1, SQLITE_TRANSIENT);
	sqlite3_bind_text (g_st_ins_flow, 4, e->category,  -1, SQLITE_TRANSIENT);
	sqlite3_bind_int  (g_st_ins_flow, 5, (int)e->mark);
	if (sqlite3_step(g_st_ins_flow) != SQLITE_DONE)
		LOGW("ins_flow: %s", sqlite3_errmsg(g_db));

	char j[512], esc_five[200], esc_cat[80];
	json_escape(esc_five, sizeof(esc_five), five);
	json_escape(esc_cat,  sizeof(esc_cat),  e->category);
	int n = snprintf(j, sizeof(j),
		"{\"kind\":\"flow\",\"ts_ns\":%llu,\"event\":\"%s\","
		"\"five_tuple\":\"%s\",\"mark\":%u,\"category\":\"%s\","
		"\"bytes_orig\":%llu,\"bytes_reply\":%llu,\"family\":%u}",
		(unsigned long long)e->ts_ns, event_str, esc_five,
		e->mark, esc_cat,
		(unsigned long long)e->bytes_orig,
		(unsigned long long)e->bytes_reply,
		e->family);
	if (n > 0) sse_broadcast(j, (size_t)n);
}

static void on_inject(const struct ratan_inject_event *e)
{
	/* The injects table has session_id NOT NULL with FK CASCADE -- writing
	 * with session_id <= 0 silently drops. ratan-test always supplies a
	 * valid id from the session it just created via POST /sessions. */
	if (e->session_id <= 0) return;

	sqlite3_reset(g_st_ins_inject);
	sqlite3_bind_int64(g_st_ins_inject, 1, (sqlite3_int64)e->ts_ns);
	sqlite3_bind_int  (g_st_ins_inject, 2, e->session_id);
	sqlite3_bind_text (g_st_ins_inject, 3, e->action, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text (g_st_ins_inject, 4, e->target, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text (g_st_ins_inject, 5, e->detail, -1, SQLITE_TRANSIENT);
	if (sqlite3_step(g_st_ins_inject) != SQLITE_DONE)
		LOGW("ins_inject: %s", sqlite3_errmsg(g_db));

	char j[512], act[32], tgt[32], det[160];
	json_escape(act, sizeof(act), e->action);
	json_escape(tgt, sizeof(tgt), e->target);
	json_escape(det, sizeof(det), e->detail);
	int n = snprintf(j, sizeof(j),
		"{\"kind\":\"inject\",\"ts_ns\":%llu,\"session\":%d,"
		"\"action\":\"%s\",\"target\":\"%s\",\"detail\":\"%s\"}",
		(unsigned long long)e->ts_ns, e->session_id, act, tgt, det);
	if (n > 0) sse_broadcast(j, (size_t)n);
}

static void ingest(const uint8_t *buf, size_t n)
{
	atomic_fetch_add(&g_metrics.bytes_ingested, n);
	if (n < sizeof(struct ratan_event_hdr)) return;
	const struct ratan_event_hdr *h = (const void *)buf;
	if (h->version != RATAN_PROTO_VERSION) {
		LOGW("ingest: unknown version %u", h->version);
		return;
	}
	if (sizeof(*h) + h->len > n) {
		LOGW("ingest: truncated frame (kind=%u declared_len=%u got=%zu)",
		     h->kind, h->len, n);
		return;
	}
	const void *p = buf + sizeof(*h);
	switch (h->kind) {
	case RATAN_EVENT_SAMPLE:
		if (h->len == sizeof(struct ratan_sample)) on_sample(p);
		break;
	case RATAN_EVENT_STATE:
		if (h->len == sizeof(struct ratan_state_event)) on_state(p);
		break;
	case RATAN_EVENT_WEIGHT:
		if (h->len == sizeof(struct ratan_weight_event)) on_weight(p);
		break;
	case RATAN_EVENT_INJECT:
		if (h->len == sizeof(struct ratan_inject_event)) on_inject(p);
		break;
	case RATAN_EVENT_DISCOVERY:
		if (h->len == sizeof(struct ratan_discovery_event)) on_discovery(p);
		break;
	case RATAN_EVENT_FLOW:
		if (h->len == sizeof(struct ratan_flow_event)) on_flow(p);
		break;
	case RATAN_EVENT_MOS:
		if (h->len == sizeof(struct ratan_mos_event)) on_mos(p);
		break;
	default:
		break;
	}
}

/* ---------- HTTP server (minimal) ---------- */

struct http_client {
	int    fd;
	bool   sse;                 /* keep-open SSE stream */
	int    sse_slot;            /* index into g_sse[] if sse */
	char   req[HTTP_REQ_MAX];
	size_t req_len;
	bool   req_complete;
};

static struct http_client g_http[MAX_HTTP_CLIENTS];
static int g_http_n;

static void http_drop(int idx)
{
	if (idx < 0 || idx >= g_http_n) return;
	struct http_client *c = &g_http[idx];
	if (c->sse && c->sse_slot >= 0) sse_drop(c->sse_slot);
	else close(c->fd);
	g_http[idx] = g_http[--g_http_n];
}

static void send_status(int fd, int code, const char *ctype, const char *body, size_t blen)
{
	const char *msg = "OK";
	if (code == 201) msg = "Created";
	else if (code == 204) msg = "No Content";
	else if (code == 400) msg = "Bad Request";
	else if (code == 404) msg = "Not Found";
	else if (code == 405) msg = "Method Not Allowed";
	else if (code == 500) msg = "Internal Server Error";

	char hdr[512];
	int n = snprintf(hdr, sizeof(hdr),
		"HTTP/1.1 %d %s\r\n"
		"Content-Type: %s\r\n"
		"Content-Length: %zu\r\n"
		"Connection: close\r\n"
		"Access-Control-Allow-Origin: *\r\n"
		"\r\n",
		code, msg, ctype, blen);
	if (n > 0) (void)send(fd, hdr, (size_t)n, MSG_NOSIGNAL);
	if (body && blen) (void)send(fd, body, blen, MSG_NOSIGNAL);
}

static void route_healthz(int fd)
{
	const char *body = "ok\n";
	send_status(fd, 200, "text/plain", body, strlen(body));
}

static void route_metrics(int fd)
{
	char b[2048];
	int n = snprintf(b, sizeof(b),
"# HELP ratan_samples_total Total telemetry samples ingested\n"
"# TYPE ratan_samples_total counter\n"
"ratan_samples_total %llu\n"
"# HELP ratan_samples_loss Total loss samples\n"
"# TYPE ratan_samples_loss counter\n"
"ratan_samples_loss %llu\n"
"# HELP ratan_samples_failover Total fast-failover events\n"
"# TYPE ratan_samples_failover counter\n"
"ratan_samples_failover %llu\n"
"# HELP ratan_state_transitions_total Classifier FSM transitions\n"
"# TYPE ratan_state_transitions_total counter\n"
"ratan_state_transitions_total %llu\n"
"# HELP ratan_weight_changes_total BPF weight map writes\n"
"# TYPE ratan_weight_changes_total counter\n"
"ratan_weight_changes_total %llu\n"
"# HELP ratan_discovery_events_total Discovery events ingested (netlink + inotify)\n"
"# TYPE ratan_discovery_events_total counter\n"
"ratan_discovery_events_total %llu\n"
"# HELP ratan_flow_events_total Conntrack flow events ingested (CTNETLINK)\n"
"# TYPE ratan_flow_events_total counter\n"
"ratan_flow_events_total %llu\n"
"# HELP ratan_mos_events_total Per-second MOS samples computed for active call flows\n"
"# TYPE ratan_mos_events_total counter\n"
"ratan_mos_events_total %llu\n"
"# HELP ratan_http_requests_total HTTP requests handled\n"
"# TYPE ratan_http_requests_total counter\n"
"ratan_http_requests_total %llu\n"
"# HELP ratan_sse_clients SSE subscribers currently connected\n"
"# TYPE ratan_sse_clients gauge\n"
"ratan_sse_clients %llu\n"
"# HELP ratan_sessions_started_total Sessions ever started\n"
"# TYPE ratan_sessions_started_total counter\n"
"ratan_sessions_started_total %llu\n"
"# HELP ratan_sessions_completed_total Sessions cleanly completed\n"
"# TYPE ratan_sessions_completed_total counter\n"
"ratan_sessions_completed_total %llu\n"
"# HELP ratan_active_session_id Currently-recording session id (0 = none)\n"
"# TYPE ratan_active_session_id gauge\n"
"ratan_active_session_id %d\n",
		(unsigned long long)atomic_load(&g_metrics.samples_total),
		(unsigned long long)atomic_load(&g_metrics.samples_loss),
		(unsigned long long)atomic_load(&g_metrics.samples_failover),
		(unsigned long long)atomic_load(&g_metrics.state_transitions_total),
		(unsigned long long)atomic_load(&g_metrics.weight_changes_total),
		(unsigned long long)atomic_load(&g_metrics.discovery_events_total),
		(unsigned long long)atomic_load(&g_metrics.flow_events_total),
		(unsigned long long)atomic_load(&g_metrics.mos_events_total),
		(unsigned long long)atomic_load(&g_metrics.http_requests_total),
		(unsigned long long)atomic_load(&g_metrics.sse_clients_current),
		(unsigned long long)atomic_load(&g_metrics.sessions_started_total),
		(unsigned long long)atomic_load(&g_metrics.sessions_completed_total),
		atomic_load(&g_active_session_id));
	send_status(fd, 200, "text/plain; version=0.0.4", b, (n > 0) ? (size_t)n : 0);
}

static int parse_int_path_suffix(const char *path, const char *prefix)
{
	size_t plen = strlen(prefix);
	if (strncmp(path, prefix, plen) != 0) return -1;
	const char *p = path + plen;
	char *endp;
	long v = strtol(p, &endp, 10);
	if (endp == p) return -1;
	return (int)v;
}

/* Sessions */

static void route_sessions_list(int fd)
{
	sqlite3_stmt *st;
	const char *q = "SELECT id,name,started_ns,ended_ns,status FROM sessions "
			"ORDER BY id DESC LIMIT 200";
	if (sqlite3_prepare_v2(g_db, q, -1, &st, NULL) != SQLITE_OK) {
		send_status(fd, 500, "text/plain", "", 0); return;
	}
	char *out = malloc(65536);
	if (!out) { sqlite3_finalize(st); send_status(fd, 500, "text/plain", "", 0); return; }
	size_t len = 0;
	len += snprintf(out + len, 65536 - len, "{\"sessions\":[");
	bool first = true;
	while (sqlite3_step(st) == SQLITE_ROW) {
		if (!first) len += snprintf(out + len, 65536 - len, ",");
		first = false;
		int id = sqlite3_column_int(st, 0);
		const unsigned char *name = sqlite3_column_text(st, 1);
		sqlite3_int64 started = sqlite3_column_int64(st, 2);
		sqlite3_int64 ended = sqlite3_column_type(st, 3) == SQLITE_NULL ? 0
				     : sqlite3_column_int64(st, 3);
		const unsigned char *status = sqlite3_column_text(st, 4);
		char esc_name[256], esc_status[32];
		json_escape(esc_name, sizeof(esc_name), name ? (const char *)name : "");
		json_escape(esc_status, sizeof(esc_status), status ? (const char *)status : "");
		len += snprintf(out + len, 65536 - len,
			"{\"id\":%d,\"name\":\"%s\",\"started_ns\":%lld,"
			"\"ended_ns\":%lld,\"status\":\"%s\"}",
			id, esc_name, (long long)started, (long long)ended, esc_status);
	}
	len += snprintf(out + len, 65536 - len, "]}");
	sqlite3_finalize(st);
	send_status(fd, 200, "application/json", out, len);
	free(out);
}

/* Extract a JSON string field; returns 0 on success. Tiny manual parser
 * because we don't want a json lib dep. Handles "key":"value" pairs only. */
static int json_get_str(const char *body, const char *key, char *out, size_t outsz)
{
	char pat[64];
	snprintf(pat, sizeof(pat), "\"%s\"", key);
	const char *p = strstr(body, pat);
	if (!p) return -1;
	p += strlen(pat);
	while (*p == ' ' || *p == ':' || *p == '\t') p++;
	if (*p != '"') return -1;
	p++;
	size_t i = 0;
	while (*p && *p != '"' && i + 1 < outsz) {
		if (*p == '\\' && p[1]) { out[i++] = p[1]; p += 2; }
		else                    { out[i++] = *p++; }
	}
	out[i] = '\0';
	return 0;
}

static uint64_t now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

static void route_sessions_create(int fd, const char *body)
{
	char name[128] = "unnamed";
	char meta[1024] = "";
	json_get_str(body, "name", name, sizeof(name));
	/* metadata is optional; we just store whatever JSON-ish substring
	 * the caller sent under that key. Strict parsing deferred. */
	const char *m = strstr(body, "\"metadata\"");
	if (m) {
		m = strchr(m, ':');
		if (m) {
			m++;
			while (*m == ' ' || *m == '\t') m++;
			strncpy(meta, m, sizeof(meta) - 1);
		}
	}

	sqlite3_reset(g_st_ins_session);
	sqlite3_bind_text (g_st_ins_session, 1, name, -1, SQLITE_TRANSIENT);
	sqlite3_bind_int64(g_st_ins_session, 2, (sqlite3_int64)now_ns());
	sqlite3_bind_text (g_st_ins_session, 3, meta, -1, SQLITE_TRANSIENT);

	if (sqlite3_step(g_st_ins_session) != SQLITE_DONE) {
		send_status(fd, 500, "text/plain", "", 0); return;
	}
	int id = (int)sqlite3_last_insert_rowid(g_db);
	atomic_store(&g_active_session_id, id);
	atomic_fetch_add(&g_metrics.sessions_started_total, 1);

	char resp[256];
	int n = snprintf(resp, sizeof(resp),
		"{\"id\":%d,\"name\":\"%s\",\"started_ns\":%llu,\"status\":\"recording\"}",
		id, name, (unsigned long long)now_ns());
	send_status(fd, 201, "application/json", resp, (size_t)n);
	LOGI("session %d started: %s", id, name);
}

/* Compute a small summary for a finished session.
 * Counts samples per WAN, loss counts, transitions, weight changes. */
static char *compute_summary(int sid, size_t *out_len)
{
	char *buf = malloc(8192);
	if (!buf) return NULL;
	size_t len = 0;

	len += snprintf(buf + len, 8192 - len, "{\"per_wan\":[");
	sqlite3_stmt *st;
	const char *q =
		"SELECT wan_id, COUNT(*), SUM(CASE WHEN event=1 THEN 1 ELSE 0 END), "
		"       SUM(CASE WHEN event=2 THEN 1 ELSE 0 END), "
		"       AVG(NULLIF(rtt_us,0)), MAX(NULLIF(rtt_us,0)) "
		"FROM samples WHERE session_id=? GROUP BY wan_id ORDER BY wan_id";
	if (sqlite3_prepare_v2(g_db, q, -1, &st, NULL) == SQLITE_OK) {
		sqlite3_bind_int(st, 1, sid);
		bool first = true;
		while (sqlite3_step(st) == SQLITE_ROW) {
			if (!first) len += snprintf(buf + len, 8192 - len, ",");
			first = false;
			len += snprintf(buf + len, 8192 - len,
				"{\"wan\":%d,\"samples\":%d,\"loss\":%d,\"failover\":%d,"
				"\"rtt_us_avg\":%.0f,\"rtt_us_max\":%lld}",
				sqlite3_column_int(st, 0),
				sqlite3_column_int(st, 1),
				sqlite3_column_int(st, 2),
				sqlite3_column_int(st, 3),
				sqlite3_column_double(st, 4),
				(long long)sqlite3_column_int64(st, 5));
		}
		sqlite3_finalize(st);
	}
	len += snprintf(buf + len, 8192 - len, "]");

	int transitions = 0, weight_changes = 0;
	if (sqlite3_prepare_v2(g_db, "SELECT COUNT(*) FROM state_transitions WHERE session_id=?",
			       -1, &st, NULL) == SQLITE_OK) {
		sqlite3_bind_int(st, 1, sid);
		if (sqlite3_step(st) == SQLITE_ROW) transitions = sqlite3_column_int(st, 0);
		sqlite3_finalize(st);
	}
	if (sqlite3_prepare_v2(g_db, "SELECT COUNT(*) FROM weights WHERE session_id=?",
			       -1, &st, NULL) == SQLITE_OK) {
		sqlite3_bind_int(st, 1, sid);
		if (sqlite3_step(st) == SQLITE_ROW) weight_changes = sqlite3_column_int(st, 0);
		sqlite3_finalize(st);
	}
	len += snprintf(buf + len, 8192 - len,
		",\"state_transitions\":%d,\"weight_changes\":%d}",
		transitions, weight_changes);

	*out_len = len;
	return buf;
}

static void route_sessions_stop(int fd, int id)
{
	int active = atomic_load(&g_active_session_id);
	if (active != id) {
		const char *e = "session not active or unknown id";
		send_status(fd, 400, "text/plain", e, strlen(e)); return;
	}
	size_t slen = 0;
	char *summary = compute_summary(id, &slen);
	const char *status = "completed";

	sqlite3_reset(g_st_stop_session);
	sqlite3_bind_int64(g_st_stop_session, 1, (sqlite3_int64)now_ns());
	sqlite3_bind_text (g_st_stop_session, 2, status, -1, SQLITE_TRANSIENT);
	sqlite3_bind_text (g_st_stop_session, 3, summary ? summary : "{}", -1, SQLITE_TRANSIENT);
	sqlite3_bind_int  (g_st_stop_session, 4, id);
	if (sqlite3_step(g_st_stop_session) != SQLITE_DONE) {
		free(summary); send_status(fd, 500, "text/plain", "", 0); return;
	}

	atomic_store(&g_active_session_id, 0);
	atomic_fetch_add(&g_metrics.sessions_completed_total, 1);

	char resp[8192 + 256];
	int n = snprintf(resp, sizeof(resp),
		"{\"id\":%d,\"status\":\"%s\",\"summary\":%s}",
		id, status, summary ? summary : "{}");
	send_status(fd, 200, "application/json", resp, (size_t)n);
	free(summary);
	LOGI("session %d stopped", id);
}

static void route_sessions_get(int fd, int id)
{
	sqlite3_reset(g_st_get_session);
	sqlite3_bind_int(g_st_get_session, 1, id);
	if (sqlite3_step(g_st_get_session) != SQLITE_ROW) {
		send_status(fd, 404, "text/plain", "", 0); return;
	}
	char resp[16384], esc_name[256], esc_status[32];
	const unsigned char *name = sqlite3_column_text(g_st_get_session, 1);
	sqlite3_int64 started = sqlite3_column_int64(g_st_get_session, 2);
	sqlite3_int64 ended = sqlite3_column_type(g_st_get_session, 3) == SQLITE_NULL ? 0
			     : sqlite3_column_int64(g_st_get_session, 3);
	const unsigned char *status  = sqlite3_column_text(g_st_get_session, 4);
	const unsigned char *meta    = sqlite3_column_text(g_st_get_session, 5);
	const unsigned char *summary = sqlite3_column_text(g_st_get_session, 6);
	json_escape(esc_name,   sizeof(esc_name),   name   ? (const char *)name   : "");
	json_escape(esc_status, sizeof(esc_status), status ? (const char *)status : "");

	/* metadata + summary are stored as JSON literals (or empty); pass through
	 * untouched if non-empty, else null. They originate from us, never the
	 * wire, so they're safe to inline. */
	int n = snprintf(resp, sizeof(resp),
		"{\"id\":%d,\"name\":\"%s\",\"started_ns\":%lld,\"ended_ns\":%lld,"
		"\"status\":\"%s\",\"metadata\":%s,\"summary\":%s}",
		id, esc_name, (long long)started, (long long)ended, esc_status,
		meta && *meta ? (const char *)meta : "null",
		summary && *summary ? (const char *)summary : "null");
	send_status(fd, 200, "application/json", resp, (size_t)n);
}

/* Download serves the session as a single JSON file with the full event
 * timeline. Used by the LuCI UI's "Download" button and the Vercel
 * dashboard's session-replay feature. */
static void route_sessions_download(int fd, int id)
{
	sqlite3_stmt *st;
	char *buf = malloc(1 << 20);   /* 1 MiB; truncate gracefully if exceeded */
	if (!buf) { send_status(fd, 500, "text/plain", "", 0); return; }
	size_t cap = 1 << 20, len = 0;

	/* Session header */
	sqlite3_reset(g_st_get_session);
	sqlite3_bind_int(g_st_get_session, 1, id);
	if (sqlite3_step(g_st_get_session) != SQLITE_ROW) {
		free(buf); send_status(fd, 404, "text/plain", "", 0); return;
	}
	const unsigned char *name = sqlite3_column_text(g_st_get_session, 1);
	sqlite3_int64 started = sqlite3_column_int64(g_st_get_session, 2);
	sqlite3_int64 ended   = sqlite3_column_type(g_st_get_session, 3) == SQLITE_NULL ? 0
			       : sqlite3_column_int64(g_st_get_session, 3);
	const unsigned char *status  = sqlite3_column_text(g_st_get_session, 4);
	const unsigned char *summary = sqlite3_column_text(g_st_get_session, 6);
	char esc_name[256], esc_status[32];
	json_escape(esc_name,   sizeof(esc_name),   name   ? (const char *)name   : "");
	json_escape(esc_status, sizeof(esc_status), status ? (const char *)status : "");
	len += snprintf(buf + len, cap - len,
		"{\"session\":{\"id\":%d,\"name\":\"%s\",\"started_ns\":%lld,"
		"\"ended_ns\":%lld,\"status\":\"%s\",\"summary\":%s},",
		id, esc_name, (long long)started, (long long)ended, esc_status,
		summary && *summary ? (const char *)summary : "null");

#define DUMP(tbl, fields, fmt, ...) do { \
		len += snprintf(buf + len, cap - len, "\"" tbl "\":["); \
		bool first = true; \
		const char *q = "SELECT " fields " FROM " tbl " WHERE session_id=? ORDER BY ts_ns"; \
		if (sqlite3_prepare_v2(g_db, q, -1, &st, NULL) == SQLITE_OK) { \
			sqlite3_bind_int(st, 1, id); \
			while (sqlite3_step(st) == SQLITE_ROW) { \
				if (cap - len < 1024) { len += snprintf(buf + len, cap - len, "/* truncated */"); break; } \
				if (!first) len += snprintf(buf + len, cap - len, ","); \
				first = false; \
				len += snprintf(buf + len, cap - len, fmt, __VA_ARGS__); \
			} \
			sqlite3_finalize(st); \
		} \
		len += snprintf(buf + len, cap - len, "],"); \
	} while (0)

	DUMP("samples",
	     "ts_ns,wan_id,event,seq,rtt_us,loss_count,jitter_us",
	     "{\"ts\":%lld,\"wan\":%d,\"event\":%d,\"seq\":%d,\"rtt_us\":%d,\"loss\":%d,\"jitter_us\":%d}",
	     (long long)sqlite3_column_int64(st, 0),
	     sqlite3_column_int(st, 1), sqlite3_column_int(st, 2),
	     sqlite3_column_int(st, 3), sqlite3_column_int(st, 4),
	     sqlite3_column_int(st, 5), sqlite3_column_int(st, 6));

	DUMP("state_transitions",
	     "ts_ns,wan_id,from_state,to_state,reason",
	     "{\"ts\":%lld,\"wan\":%d,\"from\":\"%s\",\"to\":\"%s\",\"reason\":\"%s\"}",
	     (long long)sqlite3_column_int64(st, 0),
	     sqlite3_column_int(st, 1),
	     (const char *)sqlite3_column_text(st, 2),
	     (const char *)sqlite3_column_text(st, 3),
	     (const char *)sqlite3_column_text(st, 4));

	DUMP("weights",
	     "ts_ns,wan_id,weight,source",
	     "{\"ts\":%lld,\"wan\":%d,\"weight\":%d,\"source\":\"%s\"}",
	     (long long)sqlite3_column_int64(st, 0),
	     sqlite3_column_int(st, 1), sqlite3_column_int(st, 2),
	     (const char *)sqlite3_column_text(st, 3));

#undef DUMP

	/* Injects need string-escape on each text column (detail may contain
	 * JSON literals from recipes), so we don't use the simple DUMP macro. */
	len += snprintf(buf + len, cap - len, "\"injects\":[");
	{
		bool first = true;
		const char *q = "SELECT ts_ns,action,target,detail FROM injects "
				"WHERE session_id=? ORDER BY ts_ns";
		if (sqlite3_prepare_v2(g_db, q, -1, &st, NULL) == SQLITE_OK) {
			sqlite3_bind_int(st, 1, id);
			while (sqlite3_step(st) == SQLITE_ROW) {
				if (cap - len < 1024) {
					len += snprintf(buf + len, cap - len, "/* truncated */");
					break;
				}
				if (!first) len += snprintf(buf + len, cap - len, ",");
				first = false;
				char act[64], tgt[64], det[256];
				json_escape(act, sizeof(act),
					(const char *)sqlite3_column_text(st, 1));
				json_escape(tgt, sizeof(tgt),
					(const char *)sqlite3_column_text(st, 2));
				json_escape(det, sizeof(det),
					(const char *)sqlite3_column_text(st, 3));
				len += snprintf(buf + len, cap - len,
					"{\"ts\":%lld,\"action\":\"%s\",\"target\":\"%s\",\"detail\":\"%s\"}",
					(long long)sqlite3_column_int64(st, 0),
					act, tgt, det);
			}
			sqlite3_finalize(st);
		}
		len += snprintf(buf + len, cap - len, "],");
	}

	/* Trim trailing comma if present */
	if (len > 0 && buf[len - 1] == ',') len--;
	len += snprintf(buf + len, cap - len, "}");

	/* Use Content-Disposition so browsers offer a download. */
	char hdr[256];
	int hn = snprintf(hdr, sizeof(hdr),
		"HTTP/1.1 200 OK\r\n"
		"Content-Type: application/json\r\n"
		"Content-Length: %zu\r\n"
		"Content-Disposition: attachment; filename=\"ratan-session-%d.json\"\r\n"
		"Access-Control-Allow-Origin: *\r\n"
		"Connection: close\r\n\r\n", len, id);
	(void)send(fd, hdr, (size_t)hn, MSG_NOSIGNAL);
	(void)send(fd, buf, len, MSG_NOSIGNAL);
	free(buf);
}

/* GET /discovery -- returns up to N most recent discovery events. The UI
 * uses this to render a "currently connected" panel on page load (before
 * subscribing to /stream for delta updates). Default limit 200. */
static void route_discovery_list(int fd, int limit)
{
	if (limit <= 0 || limit > 2000) limit = 200;
	sqlite3_stmt *st;
	const char *q = "SELECT ts_ns,kind,detail FROM discovery "
			"ORDER BY ts_ns DESC LIMIT ?";
	if (sqlite3_prepare_v2(g_db, q, -1, &st, NULL) != SQLITE_OK) {
		send_status(fd, 500, "text/plain", "", 0); return;
	}
	sqlite3_bind_int(st, 1, limit);

	char *out = malloc(65536);
	if (!out) { sqlite3_finalize(st); send_status(fd, 500, "text/plain", "", 0); return; }
	size_t len = 0;
	len += snprintf(out + len, 65536 - len, "{\"events\":[");
	bool first = true;
	while (sqlite3_step(st) == SQLITE_ROW) {
		if (65536 - len < 512) break;
		if (!first) len += snprintf(out + len, 65536 - len, ",");
		first = false;
		char esc_kind[48], esc_det[256];
		json_escape(esc_kind, sizeof(esc_kind),
			    (const char *)sqlite3_column_text(st, 1));
		json_escape(esc_det,  sizeof(esc_det),
			    (const char *)sqlite3_column_text(st, 2));
		len += snprintf(out + len, 65536 - len,
			"{\"ts_ns\":%lld,\"kind\":\"%s\",\"detail\":\"%s\"}",
			(long long)sqlite3_column_int64(st, 0), esc_kind, esc_det);
	}
	len += snprintf(out + len, 65536 - len, "]}");
	sqlite3_finalize(st);
	send_status(fd, 200, "application/json", out, len);
	free(out);
}

/* GET /mos -- N most recent MOS samples. Pair with /stream for live. */
static void route_mos_list(int fd, int limit)
{
	if (limit <= 0 || limit > 2000) limit = 200;
	sqlite3_stmt *st;
	const char *q = "SELECT ts_ns,flow_id,mos,rtt_ms,loss_pct,jitter_ms,session_id "
			"FROM mos ORDER BY ts_ns DESC LIMIT ?";
	if (sqlite3_prepare_v2(g_db, q, -1, &st, NULL) != SQLITE_OK) {
		send_status(fd, 500, "text/plain", "", 0); return;
	}
	sqlite3_bind_int(st, 1, limit);

	char *out = malloc(65536);
	if (!out) { sqlite3_finalize(st); send_status(fd, 500, "text/plain", "", 0); return; }
	size_t len = 0;
	len += snprintf(out + len, 65536 - len, "{\"samples\":[");
	bool first = true;
	while (sqlite3_step(st) == SQLITE_ROW) {
		if (65536 - len < 256) break;
		if (!first) len += snprintf(out + len, 65536 - len, ",");
		first = false;
		char esc_fid[160];
		json_escape(esc_fid, sizeof(esc_fid),
			(const char *)sqlite3_column_text(st, 1));
		int sid = sqlite3_column_type(st, 6) == SQLITE_NULL ? 0
			  : sqlite3_column_int(st, 6);
		len += snprintf(out + len, 65536 - len,
			"{\"ts_ns\":%lld,\"flow\":\"%s\",\"mos\":%.3f,"
			"\"rtt_ms\":%.2f,\"loss_pct\":%.4f,\"jitter_ms\":%.2f,"
			"\"session_id\":%d}",
			(long long)sqlite3_column_int64(st, 0), esc_fid,
			sqlite3_column_double(st, 2),
			sqlite3_column_double(st, 3),
			sqlite3_column_double(st, 4),
			sqlite3_column_double(st, 5), sid);
	}
	len += snprintf(out + len, 65536 - len, "]}");
	sqlite3_finalize(st);
	send_status(fd, 200, "application/json", out, len);
	free(out);
}

/* GET /flows -- N most recent flow events. Pair this with /stream for
 * the UI's live flows table. */
static void route_flows_list(int fd, int limit)
{
	if (limit <= 0 || limit > 2000) limit = 200;
	sqlite3_stmt *st;
	const char *q = "SELECT ts_ns,event,five_tuple,category,mark FROM flows "
			"ORDER BY ts_ns DESC LIMIT ?";
	if (sqlite3_prepare_v2(g_db, q, -1, &st, NULL) != SQLITE_OK) {
		send_status(fd, 500, "text/plain", "", 0); return;
	}
	sqlite3_bind_int(st, 1, limit);

	char *out = malloc(65536);
	if (!out) { sqlite3_finalize(st); send_status(fd, 500, "text/plain", "", 0); return; }
	size_t len = 0;
	len += snprintf(out + len, 65536 - len, "{\"flows\":[");
	bool first = true;
	while (sqlite3_step(st) == SQLITE_ROW) {
		if (65536 - len < 512) break;
		if (!first) len += snprintf(out + len, 65536 - len, ",");
		first = false;
		char esc_ev[24], esc_ft[200], esc_cat[80];
		json_escape(esc_ev,  sizeof(esc_ev),
			(const char *)sqlite3_column_text(st, 1));
		json_escape(esc_ft,  sizeof(esc_ft),
			(const char *)sqlite3_column_text(st, 2));
		json_escape(esc_cat, sizeof(esc_cat),
			(const char *)sqlite3_column_text(st, 3));
		len += snprintf(out + len, 65536 - len,
			"{\"ts_ns\":%lld,\"event\":\"%s\",\"five_tuple\":\"%s\","
			"\"category\":\"%s\",\"mark\":%d}",
			(long long)sqlite3_column_int64(st, 0),
			esc_ev, esc_ft, esc_cat, sqlite3_column_int(st, 4));
	}
	len += snprintf(out + len, 65536 - len, "]}");
	sqlite3_finalize(st);
	send_status(fd, 200, "application/json", out, len);
	free(out);
}

static void route_sessions_delete(int fd, int id)
{
	char sql[128];
	snprintf(sql, sizeof(sql), "DELETE FROM sessions WHERE id=%d", id);
	if (db_exec(sql) < 0) { send_status(fd, 500, "text/plain", "", 0); return; }
	send_status(fd, 204, "text/plain", "", 0);
}

/* SSE handshake: send headers, register the client in g_sse for broadcasts. */
static void route_stream(int idx)
{
	struct http_client *c = &g_http[idx];
	const char *hdr =
		"HTTP/1.1 200 OK\r\n"
		"Content-Type: text/event-stream\r\n"
		"Cache-Control: no-cache\r\n"
		"Connection: keep-alive\r\n"
		"Access-Control-Allow-Origin: *\r\n"
		"\r\n"
		"retry: 2000\n\n";
	if (send(c->fd, hdr, strlen(hdr), MSG_NOSIGNAL) < 0) {
		http_drop(idx); return;
	}

	if (g_sse_n >= MAX_HTTP_CLIENTS) {
		const char *e = "data: {\"err\":\"too many sse clients\"}\n\n";
		(void)send(c->fd, e, strlen(e), MSG_NOSIGNAL);
		http_drop(idx); return;
	}
	g_sse[g_sse_n].fd = c->fd;
	g_sse[g_sse_n].buf_len = 0;
	c->sse = true;
	c->sse_slot = g_sse_n;
	g_sse_n++;
	atomic_store(&g_metrics.sse_clients_current, g_sse_n);
}

/* CORS preflight for browser clients. */
static void route_cors_preflight(int fd)
{
	const char *hdr =
		"HTTP/1.1 204 No Content\r\n"
		"Access-Control-Allow-Origin: *\r\n"
		"Access-Control-Allow-Methods: GET, POST, DELETE, OPTIONS\r\n"
		"Access-Control-Allow-Headers: Content-Type\r\n"
		"Access-Control-Max-Age: 86400\r\n"
		"Content-Length: 0\r\n"
		"Connection: close\r\n\r\n";
	(void)send(fd, hdr, strlen(hdr), MSG_NOSIGNAL);
}

static void http_handle(int idx)
{
	struct http_client *c = &g_http[idx];
	atomic_fetch_add(&g_metrics.http_requests_total, 1);

	/* parse method + path */
	char method[8] = "", path[256] = "";
	if (sscanf(c->req, "%7s %255s", method, path) != 2) {
		send_status(c->fd, 400, "text/plain", "", 0);
		http_drop(idx); return;
	}

	/* body: anything after "\r\n\r\n" (POST only) */
	const char *body = strstr(c->req, "\r\n\r\n");
	if (body) body += 4; else body = "";

	if (!strcmp(method, "OPTIONS")) {
		route_cors_preflight(c->fd); http_drop(idx); return;
	}

	if (!strcmp(method, "GET") && !strcmp(path, "/healthz")) {
		route_healthz(c->fd); http_drop(idx); return;
	}
	if (!strcmp(method, "GET") && !strcmp(path, "/metrics")) {
		route_metrics(c->fd); http_drop(idx); return;
	}
	if (!strcmp(method, "GET") && !strcmp(path, "/stream")) {
		route_stream(idx); return;  /* keeps connection open */
	}
	if (!strcmp(method, "GET") && !strcmp(path, "/sessions")) {
		route_sessions_list(c->fd); http_drop(idx); return;
	}
	if (!strcmp(method, "GET") &&
	    (!strcmp(path, "/discovery") || !strncmp(path, "/discovery?", 11))) {
		int limit = 200;
		const char *q = strchr(path, '?');
		if (q) { const char *l = strstr(q, "limit="); if (l) limit = atoi(l + 6); }
		route_discovery_list(c->fd, limit);
		http_drop(idx); return;
	}
	if (!strcmp(method, "GET") &&
	    (!strcmp(path, "/flows") || !strncmp(path, "/flows?", 7))) {
		int limit = 200;
		const char *q = strchr(path, '?');
		if (q) { const char *l = strstr(q, "limit="); if (l) limit = atoi(l + 6); }
		route_flows_list(c->fd, limit);
		http_drop(idx); return;
	}
	if (!strcmp(method, "GET") &&
	    (!strcmp(path, "/mos") || !strncmp(path, "/mos?", 5))) {
		int limit = 200;
		const char *q = strchr(path, '?');
		if (q) { const char *l = strstr(q, "limit="); if (l) limit = atoi(l + 6); }
		route_mos_list(c->fd, limit);
		http_drop(idx); return;
	}
	if (!strcmp(method, "POST") && !strcmp(path, "/sessions")) {
		route_sessions_create(c->fd, body); http_drop(idx); return;
	}
	int id;
	id = parse_int_path_suffix(path, "/sessions/");
	if (id >= 0) {
		const char *tail = path + strlen("/sessions/");
		while (*tail >= '0' && *tail <= '9') tail++;

		if (!strcmp(method, "POST") && !strcmp(tail, "/stop")) {
			route_sessions_stop(c->fd, id); http_drop(idx); return;
		}
		if (!strcmp(method, "GET") && !strcmp(tail, "/download")) {
			route_sessions_download(c->fd, id); http_drop(idx); return;
		}
		if (!strcmp(method, "GET") && *tail == '\0') {
			route_sessions_get(c->fd, id); http_drop(idx); return;
		}
		if (!strcmp(method, "DELETE") && *tail == '\0') {
			route_sessions_delete(c->fd, id); http_drop(idx); return;
		}
	}

	send_status(c->fd, 404, "text/plain", "", 0);
	http_drop(idx);
}

static void http_advance(int idx)
{
	struct http_client *c = &g_http[idx];
	ssize_t n = recv(c->fd, c->req + c->req_len,
			 sizeof(c->req) - 1 - c->req_len, MSG_DONTWAIT);
	if (n <= 0) {
		if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK))
			http_drop(idx);
		return;
	}
	c->req_len += n;
	c->req[c->req_len] = '\0';
	if (strstr(c->req, "\r\n\r\n")) {
		c->req_complete = true;
		http_handle(idx);
	} else if (c->req_len >= sizeof(c->req) - 1) {
		send_status(c->fd, 400, "text/plain", "request too large", 18);
		http_drop(idx);
	}
}

/* ---------- main loop ---------- */

static int make_tel_sock(const char *path)
{
	int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	if (fd < 0) return -1;
	struct sockaddr_un sa = { .sun_family = AF_UNIX };
	strncpy(sa.sun_path, path, sizeof(sa.sun_path) - 1);
	unlink(path);
	if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
		close(fd); return -1;
	}
	chmod(path, 0660);
	int rcvbuf = 1 << 20;
	setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
	return fd;
}

static int make_http_sock(const char *host, uint16_t port)
{
	int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	if (fd < 0) return -1;
	int one = 1;
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	struct sockaddr_in sa = { .sin_family = AF_INET, .sin_port = htons(port) };
	if (inet_pton(AF_INET, host, &sa.sin_addr) != 1) { close(fd); return -1; }
	if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) { close(fd); return -1; }
	if (listen(fd, 16) < 0) { close(fd); return -1; }
	return fd;
}

static void usage(const char *me)
{
	fprintf(stderr,
"Usage: %s [opts]\n"
"  --db PATH          %s by default\n"
"  --schema PATH      %s by default\n"
"  --sock PATH        %s by default\n"
"  --http-host IP     %s by default\n"
"  --http-port N      %d by default\n"
"  --lease-file PATH  %s by default (dnsmasq leases for discover thread)\n"
"  --no-discover      don't spawn the netlink+inotify discovery thread\n"
"  --ndpi-cats PATH   %s by default (nDPI mark->category map)\n"
"  --no-flowtrack     don't spawn the CTNETLINK flow tracker thread\n"
"  --no-mos           don't spawn the MOS computer thread\n"
"  --mos-category G   substring match for which flows get MOS scored\n"
"                     (default \"VideoCall/\" -- matches Teams/Zoom/Meet)\n"
"  --foreground\n",
		me, DEFAULT_DB_PATH, DEFAULT_SCHEMA_PATH, DEFAULT_SOCK_PATH,
		DEFAULT_HTTP_HOST, DEFAULT_HTTP_PORT,
		DEFAULT_LEASE_FILE, DEFAULT_NDPI_CATS);
}

int main(int argc, char **argv)
{
	const char *db_path     = DEFAULT_DB_PATH;
	const char *sock_path   = DEFAULT_SOCK_PATH;
	const char *schema_path = DEFAULT_SCHEMA_PATH;
	const char *http_host   = DEFAULT_HTTP_HOST;
	const char *lease_file  = DEFAULT_LEASE_FILE;
	const char *ndpi_cats   = DEFAULT_NDPI_CATS;
	uint16_t    http_port   = DEFAULT_HTTP_PORT;
	bool        run_discover  = true;
	bool        run_flowtrack = true;
	bool        run_mos       = true;
	const char *mos_category  = "VideoCall/";

	enum { OPT_LEASE = 256, OPT_NODISC, OPT_NDPI, OPT_NOFLOW,
	       OPT_NOMOS, OPT_MOSCAT };
	static const struct option opts[] = {
		{ "db",            required_argument, 0, 'd' },
		{ "schema",        required_argument, 0, 'C' },
		{ "sock",          required_argument, 0, 's' },
		{ "http-host",     required_argument, 0, 'H' },
		{ "http-port",     required_argument, 0, 'P' },
		{ "lease-file",    required_argument, 0, OPT_LEASE },
		{ "no-discover",   no_argument,       0, OPT_NODISC },
		{ "ndpi-cats",     required_argument, 0, OPT_NDPI },
		{ "no-flowtrack",  no_argument,       0, OPT_NOFLOW },
		{ "no-mos",        no_argument,       0, OPT_NOMOS },
		{ "mos-category",  required_argument, 0, OPT_MOSCAT },
		{ "foreground",    no_argument,       0, 'f' },
		{ "help",          no_argument,       0, 'h' },
		{ 0 }
	};
	int c;
	while ((c = getopt_long(argc, argv, "d:C:s:H:P:fh", opts, NULL)) != -1) {
		switch (c) {
		case 'd': db_path = optarg; break;
		case 'C': schema_path = optarg; break;
		case 's': sock_path = optarg; break;
		case 'H': http_host = optarg; break;
		case 'P': http_port = (uint16_t)atoi(optarg); break;
		case OPT_LEASE:  lease_file = optarg; break;
		case OPT_NODISC: run_discover = false; break;
		case OPT_NDPI:   ndpi_cats = optarg; break;
		case OPT_NOFLOW: run_flowtrack = false; break;
		case OPT_NOMOS:  run_mos = false; break;
		case OPT_MOSCAT: mos_category = optarg; break;
		case 'f': g_foreground = true; break;
		case 'h': default: usage(argv[0]); return c == 'h' ? 0 : 1;
		}
	}

	openlog("ratan-telemetryd", LOG_PID | (g_foreground ? LOG_PERROR : 0), LOG_DAEMON);

	mkdir("/run/ratan", 0755);
	mkdir("/var/lib/ratan", 0755);

	if (db_init(db_path, schema_path) < 0) { LOGE("db init failed"); return 1; }

	int tel_fd = make_tel_sock(sock_path);
	if (tel_fd < 0) { LOGE("tel sock %s: %s", sock_path, strerror(errno)); return 1; }
	int http_fd = make_http_sock(http_host, http_port);
	if (http_fd < 0) { LOGE("http %s:%u: %s", http_host, http_port, strerror(errno)); return 1; }

	sigset_t mask;
	sigemptyset(&mask);
	sigaddset(&mask, SIGINT);
	sigaddset(&mask, SIGTERM);
	sigprocmask(SIG_BLOCK, &mask, NULL);
	int sig_fd = signalfd(-1, &mask, SFD_CLOEXEC);
	if (sig_fd < 0) { LOGE("signalfd: %s", strerror(errno)); return 1; }
	signal(SIGPIPE, SIG_IGN);

	LOGI("listening: telsock=%s http=%s:%u db=%s",
	     sock_path, http_host, http_port, db_path);

	pthread_t discover_tid = 0;
	if (run_discover) {
		if (discover_start(sock_path, lease_file, &g_stop, &discover_tid) < 0)
			LOGW("discover thread failed to start (continuing without it)");
	}
	pthread_t flowtrack_tid = 0;
	if (run_flowtrack) {
		if (flowtrack_start(sock_path, ndpi_cats, &g_stop, &flowtrack_tid) < 0)
			LOGW("flowtrack thread failed to start (CAP_NET_ADMIN + nf_conntrack required); continuing without it");
	}
	pthread_t mos_tid = 0;
	if (run_mos) {
		if (mos_start(sock_path, db_path, mos_category, &g_stop, &mos_tid) < 0)
			LOGW("mos thread failed to start; continuing without it");
	}

	while (!g_stop) {
		struct pollfd pfds[POLLFD_BASE + MAX_HTTP_CLIENTS];
		pfds[0].fd = sig_fd;   pfds[0].events = POLLIN;
		pfds[1].fd = tel_fd;   pfds[1].events = POLLIN;
		pfds[2].fd = http_fd;  pfds[2].events = POLLIN;
		for (int i = 0; i < g_http_n; i++) {
			pfds[POLLFD_BASE + i].fd = g_http[i].fd;
			pfds[POLLFD_BASE + i].events = POLLIN;
			if (g_http[i].sse && g_http[i].sse_slot >= 0 &&
			    g_sse[g_http[i].sse_slot].buf_len > 0)
				pfds[POLLFD_BASE + i].events |= POLLOUT;
		}

		int rc = poll(pfds, POLLFD_BASE + g_http_n, 1000);
		if (rc < 0) { if (errno == EINTR) continue; break; }

		if (pfds[0].revents & POLLIN) break;

		if (pfds[1].revents & POLLIN) {
			uint8_t buf[4096];
			ssize_t n;
			while ((n = recv(tel_fd, buf, sizeof(buf), MSG_DONTWAIT)) > 0)
				ingest(buf, (size_t)n);
		}

		if (pfds[2].revents & POLLIN) {
			while (g_http_n < MAX_HTTP_CLIENTS) {
				int cfd = accept4(http_fd, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
				if (cfd < 0) break;
				g_http[g_http_n].fd = cfd;
				g_http[g_http_n].req_len = 0;
				g_http[g_http_n].req_complete = false;
				g_http[g_http_n].sse = false;
				g_http[g_http_n].sse_slot = -1;
				g_http_n++;
			}
		}

		for (int i = 0; i < g_http_n; ) {
			int prev_n = g_http_n;
			if (pfds[POLLFD_BASE + i].revents & POLLIN) http_advance(i);
			if (pfds[POLLFD_BASE + i].revents & POLLOUT && g_http[i].sse &&
			    g_http[i].sse_slot >= 0) {
				/* drain pending SSE bytes */
				struct sse_client *s = &g_sse[g_http[i].sse_slot];
				ssize_t n = send(s->fd, s->buf, s->buf_len, MSG_NOSIGNAL | MSG_DONTWAIT);
				if (n > 0) { memmove(s->buf, s->buf + n, s->buf_len - n); s->buf_len -= n; }
				else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) http_drop(i);
			}
			if (g_http_n == prev_n) i++;
		}
	}

	LOGI("shutting down");
	g_stop = 1;
	if (discover_tid)  pthread_join(discover_tid, NULL);
	if (flowtrack_tid) pthread_join(flowtrack_tid, NULL);
	if (mos_tid)       pthread_join(mos_tid, NULL);
	close(sig_fd); close(http_fd); close(tel_fd);
	for (int i = 0; i < g_http_n; i++) close(g_http[i].fd);
	for (int i = 0; i < g_sse_n; i++) close(g_sse[i].fd);
	sqlite3_finalize(g_st_ins_sample);
	sqlite3_finalize(g_st_ins_state);
	sqlite3_finalize(g_st_ins_weight);
	sqlite3_finalize(g_st_ins_inject);
	sqlite3_finalize(g_st_ins_discovery);
	sqlite3_finalize(g_st_ins_flow);
	sqlite3_finalize(g_st_ins_mos);
	sqlite3_finalize(g_st_ins_session);
	sqlite3_finalize(g_st_get_session);
	sqlite3_finalize(g_st_stop_session);
	sqlite3_finalize(g_st_active_session);
	sqlite3_close(g_db);
	closelog();
	return 0;
}
