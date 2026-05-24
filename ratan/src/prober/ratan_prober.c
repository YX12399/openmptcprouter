// SPDX-License-Identifier: GPL-2.0
/*
 * ratan_prober.c -- per-WAN UDP probe daemon.
 *
 * One thread per WAN. Sends a 24-byte UDP probe every 50ms (configurable)
 * to <server>:<port>; the VPS-side ratan_probe_responder echoes it back.
 * Computes RTT / loss / jitter, emits a ratan_sample over a Unix datagram
 * socket to the predictor.
 *
 * Fast-failover: on N consecutive losses (default 3 -> ~150ms), the WAN's
 * own thread writes weight=0 into the pinned BPF map for that subflow
 * index. This is the sub-200ms KPI path -- no roundtrip through the
 * predictor. An all-zero guard prevents zeroing the LAST live subflow.
 *
 * Time discipline: CLOCK_MONOTONIC + timerfd for the send cadence
 * (low jitter); CLOCK_MONOTONIC for RTT math.
 *
 * Hardening deferred: HMAC probe auth, drop privileges to a ratan user,
 * IPv6 server. Step 3 MVP runs as root, IPv4-only, unauthenticated.
 */

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/timerfd.h>
#include <sys/types.h>
#include <sys/un.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#include <bpf/bpf.h>

#include "ratan_proto.h"

#define MAX_WANS              8
#define DEFAULT_PORT          5555
#define DEFAULT_INTERVAL_MS   50
#define DEFAULT_TIMEOUT_MS    200
#define DEFAULT_FAILOVER_N    3
#define DEFAULT_SAMPLES_SOCK  "/run/ratan/samples.sock"
#define DEFAULT_WEIGHTS_MAP   "/sys/fs/bpf/ratan/path_weights"
#define DEFAULT_BASE_WEIGHT   50      /* what each WAN starts at (predictor overrides) */
#define JITTER_ALPHA_NUM      1       /* jitter EWMA = (1 * sample + 7 * prev) / 8 */
#define JITTER_ALPHA_DEN      8

struct cfg {
	char     server[128];
	uint16_t port;
	uint32_t interval_ms;
	uint32_t timeout_ms;
	uint32_t failover_n;
	char     samples_sock[128];
	char     weights_map[128];
	uint32_t base_weight;
	int      n_wans;
	struct {
		char    iface[IFNAMSIZ];
		uint8_t subflow_idx;
	} wan[MAX_WANS];
	bool     foreground;
};

/* per-WAN runtime state */
struct wan_state {
	const struct cfg *cfg;
	int               idx;            /* index into cfg->wan[] */
	uint8_t           subflow_idx;
	char              iface[IFNAMSIZ];
	int               sock;           /* UDP socket bound to iface */
	int               timer_fd;
	int               samples_fd;     /* clone fd for emit (per-thread to avoid lock) */
	int               weights_fd;     /* shared BPF map fd */
	struct sockaddr_in server_sa;

	uint32_t          seq;
	uint32_t          loss_streak;
	uint32_t          jitter_us;
	uint32_t          rtt_baseline_us;
	bool              in_failover;    /* we wrote weight=0; predictor restores */

	pthread_t         tid;
};

/* shared */
static volatile sig_atomic_t g_stop;
static int g_weights_fd = -1;
static pthread_mutex_t g_map_lock = PTHREAD_MUTEX_INITIALIZER;
static struct wan_state *g_wans;
static int g_n_wans;

static void on_signal(int s) { (void)s; g_stop = 1; }

static uint64_t now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

static void log_msg(int prio, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vsyslog(prio, fmt, ap);
	va_end(ap);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	fputc('\n', stderr);
	va_end(ap);
}

/* All-zero guard: would setting w->idx to zero leave NO subflow > 0? */
static bool would_strand_user(int target_idx, uint32_t new_weight)
{
	/* Note: TOCTOU race vs concurrent write_weight() is accepted for the
	 * Step 3 MVP; the worst case is a brief over-/under-zeroing during a
	 * simultaneous failover on two WANs, which the predictor heals.
	 * Hardening: hold g_map_lock across the check+write in write_weight. */
	bool any_nonzero = false;

	for (int i = 0; i < g_n_wans; i++) {
		uint32_t key = g_wans[i].subflow_idx;
		uint32_t val = (i == target_idx) ? new_weight : 0;
		uint32_t cur;

		if (i != target_idx) {
			if (bpf_map_lookup_elem(g_weights_fd, &key, &cur) == 0)
				val = cur;
		}
		if (val > 0) { any_nonzero = true; break; }
	}
	return !any_nonzero;
}

static int write_weight(uint8_t subflow_idx, uint32_t weight)
{
	uint32_t key = subflow_idx;
	int rc;

	pthread_mutex_lock(&g_map_lock);
	rc = bpf_map_update_elem(g_weights_fd, &key, &weight, BPF_ANY);
	pthread_mutex_unlock(&g_map_lock);
	return rc;
}

static int set_initial_weights(const struct cfg *cfg)
{
	for (int i = 0; i < cfg->n_wans; i++) {
		uint32_t key = cfg->wan[i].subflow_idx;
		uint32_t val = cfg->base_weight;
		if (bpf_map_update_elem(g_weights_fd, &key, &val, BPF_ANY) < 0)
			return -1;
	}
	return 0;
}

static int open_samples_sock(const char *path)
{
	int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	if (fd < 0) return -1;
	return fd;
}

static void emit_sample(int fd, const char *path, const struct ratan_sample *s)
{
	struct sockaddr_un sa = { .sun_family = AF_UNIX };
	strncpy(sa.sun_path, path, sizeof(sa.sun_path) - 1);
	(void)sendto(fd, s, sizeof(*s), MSG_DONTWAIT,
		     (struct sockaddr *)&sa, sizeof(sa));
	/* drop on the floor if predictor isn't reading; we don't block probing */
}

static int open_wan_socket(const char *iface, struct sockaddr_in *sa)
{
	int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	if (fd < 0) return -1;

	if (setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, iface, strlen(iface)) < 0) {
		log_msg(LOG_ERR, "SO_BINDTODEVICE %s: %s (need CAP_NET_RAW)",
			iface, strerror(errno));
		close(fd);
		return -1;
	}

	if (connect(fd, (struct sockaddr *)sa, sizeof(*sa)) < 0) {
		log_msg(LOG_ERR, "connect via %s: %s", iface, strerror(errno));
		close(fd);
		return -1;
	}
	return fd;
}

static int wait_response(int sock, uint32_t timeout_ms, struct ratan_probe *resp)
{
	struct pollfd p = { .fd = sock, .events = POLLIN };
	int rc = poll(&p, 1, (int)timeout_ms);
	if (rc <= 0) return rc;  /* 0 = timeout, -1 = err */
	ssize_t n = recv(sock, resp, sizeof(*resp), 0);
	if (n != (ssize_t)sizeof(*resp)) return -1;
	if (resp->version != RATAN_PROTO_VERSION) return -1;
	if (!(resp->flags & RATAN_PROBE_FLAG_RESPONSE)) return -1;
	return 1;
}

static void *wan_thread(void *arg)
{
	struct wan_state *w = arg;
	const struct cfg *cfg = w->cfg;
	struct itimerspec its = {
		.it_interval = { .tv_sec = cfg->interval_ms / 1000,
				 .tv_nsec = (cfg->interval_ms % 1000) * 1000000L },
		.it_value    = { .tv_sec = 0, .tv_nsec = 1 },  /* fire immediately */
	};

	if (timerfd_settime(w->timer_fd, 0, &its, NULL) < 0) {
		log_msg(LOG_ERR, "[%s] timerfd_settime: %s", w->iface, strerror(errno));
		return NULL;
	}

	log_msg(LOG_INFO, "[%s] probing %s:%u every %ums (subflow=%u)",
		w->iface, cfg->server, cfg->port, cfg->interval_ms, w->subflow_idx);

	while (!g_stop) {
		uint64_t expirations;
		ssize_t n = read(w->timer_fd, &expirations, sizeof(expirations));
		if (n < 0) {
			if (errno == EINTR) continue;
			break;
		}

		w->seq++;
		struct ratan_probe req = {
			.version = RATAN_PROTO_VERSION,
			.flags   = 0,
			.wan_id  = w->subflow_idx,
			.seq     = w->seq,
			.tx_ns   = now_ns(),
			.rx_ns   = 0,
		};

		if (send(w->sock, &req, sizeof(req), MSG_DONTWAIT) < 0) {
			/* send error counts as loss for accounting */
			goto loss;
		}

		struct ratan_probe resp;
		int r = wait_response(w->sock, cfg->timeout_ms, &resp);
		if (r != 1) goto loss;
		if (resp.seq != req.seq) {
			/* late response from a previous probe; treat current as loss
			 * (could be smarter and credit the matching one) */
			goto loss;
		}

		/* RTT */
		uint64_t now = now_ns();
		uint64_t rtt_ns = now - req.tx_ns;
		uint32_t rtt_us = (uint32_t)(rtt_ns / 1000);

		/* jitter EWMA */
		uint32_t diff = (rtt_us > w->rtt_baseline_us)
				? rtt_us - w->rtt_baseline_us
				: w->rtt_baseline_us - rtt_us;
		w->jitter_us = (JITTER_ALPHA_NUM * diff +
				(JITTER_ALPHA_DEN - JITTER_ALPHA_NUM) * w->jitter_us)
				/ JITTER_ALPHA_DEN;
		/* slow baseline (32x slower than jitter; ~1.6s effective at 50ms cadence) */
		w->rtt_baseline_us = (rtt_us + 31 * w->rtt_baseline_us) / 32;
		if (w->rtt_baseline_us == 0) w->rtt_baseline_us = rtt_us;

		{
			uint8_t event = (w->loss_streak > 0)
					? RATAN_EVENT_RECOVERY : RATAN_EVENT_OK;
			w->loss_streak = 0;
			/* Prober never WRITES on recovery -- the predictor (Step 6)
			 * owns weight restoration so policy stays in one place. */

			struct ratan_sample s = {
				.ts_ns       = now,
				.wan_id      = w->subflow_idx,
				.event       = event,
				.seq         = w->seq,
				.rtt_us      = rtt_us,
				.loss_count  = 0,
				.jitter_us   = w->jitter_us,
			};
			emit_sample(w->samples_fd, cfg->samples_sock, &s);
		}
		continue;

loss:
		{
			w->loss_streak++;
			uint8_t event = RATAN_EVENT_LOSS;

			if (w->loss_streak >= cfg->failover_n && !w->in_failover) {
				if (would_strand_user(w->idx, 0)) {
					log_msg(LOG_WARNING,
						"[%s] %u consecutive losses but would strand "
						"user; holding weight, not failing over",
						w->iface, w->loss_streak);
				} else if (write_weight(w->subflow_idx, 0) == 0) {
					w->in_failover = true;
					event = RATAN_EVENT_FAST_FAILOVER;
					log_msg(LOG_WARNING,
						"[%s] fast failover: weight=0 after %u losses",
						w->iface, w->loss_streak);
				} else {
					log_msg(LOG_ERR, "[%s] bpf_map_update failed: %s",
						w->iface, strerror(errno));
				}
			}

			struct ratan_sample s = {
				.ts_ns       = now_ns(),
				.wan_id      = w->subflow_idx,
				.event       = event,
				.seq         = w->seq,
				.rtt_us      = 0,
				.loss_count  = w->loss_streak,
				.jitter_us   = w->jitter_us,
			};
			emit_sample(w->samples_fd, cfg->samples_sock, &s);
		}
	}

	return NULL;
}

static int resolve_server(const char *host, uint16_t port, struct sockaddr_in *sa)
{
	struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_DGRAM };
	struct addrinfo *res;
	char portbuf[8];
	snprintf(portbuf, sizeof(portbuf), "%u", port);
	if (getaddrinfo(host, portbuf, &hints, &res) != 0) return -1;
	memcpy(sa, res->ai_addr, sizeof(*sa));
	freeaddrinfo(res);
	return 0;
}

static void usage(const char *me)
{
	fprintf(stderr,
"Usage: %s --server HOST [opts] --wan IFACE:IDX [--wan IFACE:IDX ...]\n"
"\n"
"  --server HOST          VPS hostname or IPv4 (required)\n"
"  --port N               UDP port (default %u)\n"
"  --interval-ms N        probe cadence (default %u)\n"
"  --timeout-ms N         per-probe response timeout (default %u)\n"
"  --failover-n N         consecutive losses -> weight=0 (default %u)\n"
"  --samples-sock PATH    /run/ratan/samples.sock by default\n"
"  --weights-map PATH     %s by default\n"
"  --base-weight N        initial weight per WAN (default %u)\n"
"  --wan IFACE:IDX        repeatable; IDX = MPTCP subflow index 0..%u\n"
"  --foreground           don't fork; log to stderr too\n",
		me, DEFAULT_PORT, DEFAULT_INTERVAL_MS, DEFAULT_TIMEOUT_MS,
		DEFAULT_FAILOVER_N, DEFAULT_WEIGHTS_MAP, DEFAULT_BASE_WEIGHT,
		MAX_WANS - 1);
}

static int parse_wan_spec(struct cfg *cfg, const char *spec)
{
	char *colon = strchr(spec, ':');
	if (!colon || cfg->n_wans >= MAX_WANS) return -1;
	size_t ifl = colon - spec;
	if (ifl >= IFNAMSIZ) return -1;
	memcpy(cfg->wan[cfg->n_wans].iface, spec, ifl);
	cfg->wan[cfg->n_wans].iface[ifl] = '\0';
	cfg->wan[cfg->n_wans].subflow_idx = (uint8_t)atoi(colon + 1);
	cfg->n_wans++;
	return 0;
}

int main(int argc, char **argv)
{
	struct cfg cfg = {
		.port = DEFAULT_PORT,
		.interval_ms = DEFAULT_INTERVAL_MS,
		.timeout_ms = DEFAULT_TIMEOUT_MS,
		.failover_n = DEFAULT_FAILOVER_N,
		.base_weight = DEFAULT_BASE_WEIGHT,
	};
	strncpy(cfg.samples_sock, DEFAULT_SAMPLES_SOCK, sizeof(cfg.samples_sock) - 1);
	strncpy(cfg.weights_map, DEFAULT_WEIGHTS_MAP, sizeof(cfg.weights_map) - 1);

	static const struct option opts[] = {
		{ "server",        required_argument, 0, 's' },
		{ "port",          required_argument, 0, 'p' },
		{ "interval-ms",   required_argument, 0, 'i' },
		{ "timeout-ms",    required_argument, 0, 't' },
		{ "failover-n",    required_argument, 0, 'n' },
		{ "samples-sock",  required_argument, 0, 'S' },
		{ "weights-map",   required_argument, 0, 'M' },
		{ "base-weight",   required_argument, 0, 'b' },
		{ "wan",           required_argument, 0, 'w' },
		{ "foreground",    no_argument,       0, 'f' },
		{ "help",          no_argument,       0, 'h' },
		{ 0 }
	};

	int c;
	while ((c = getopt_long(argc, argv, "s:p:i:t:n:S:M:b:w:fh", opts, NULL)) != -1) {
		switch (c) {
		case 's': strncpy(cfg.server, optarg, sizeof(cfg.server) - 1); break;
		case 'p': cfg.port = (uint16_t)atoi(optarg); break;
		case 'i': cfg.interval_ms = (uint32_t)atoi(optarg); break;
		case 't': cfg.timeout_ms = (uint32_t)atoi(optarg); break;
		case 'n': cfg.failover_n = (uint32_t)atoi(optarg); break;
		case 'S': strncpy(cfg.samples_sock, optarg, sizeof(cfg.samples_sock) - 1); break;
		case 'M': strncpy(cfg.weights_map, optarg, sizeof(cfg.weights_map) - 1); break;
		case 'b': cfg.base_weight = (uint32_t)atoi(optarg); break;
		case 'w':
			if (parse_wan_spec(&cfg, optarg) < 0) {
				fprintf(stderr, "bad --wan spec: %s\n", optarg);
				return 1;
			}
			break;
		case 'f': cfg.foreground = true; break;
		case 'h': default: usage(argv[0]); return c == 'h' ? 0 : 1;
		}
	}

	if (!cfg.server[0] || cfg.n_wans == 0) { usage(argv[0]); return 1; }

	openlog("ratan-prober", LOG_PID | (cfg.foreground ? LOG_PERROR : 0), LOG_DAEMON);

	struct sockaddr_in server_sa;
	if (resolve_server(cfg.server, cfg.port, &server_sa) < 0) {
		log_msg(LOG_ERR, "could not resolve %s", cfg.server);
		return 1;
	}

	/* Ensure /run/ratan exists for the samples socket. */
	mkdir("/run/ratan", 0755);

	/* Open the pinned BPF weights map. */
	g_weights_fd = bpf_obj_get(cfg.weights_map);
	if (g_weights_fd < 0) {
		log_msg(LOG_ERR, "bpf_obj_get %s: %s (is ratan-sched loaded?)",
			cfg.weights_map, strerror(errno));
		return 1;
	}
	if (set_initial_weights(&cfg) < 0)
		log_msg(LOG_WARNING, "could not set initial weights: %s", strerror(errno));

	g_wans = calloc(cfg.n_wans, sizeof(*g_wans));
	if (!g_wans) return 1;
	g_n_wans = cfg.n_wans;

	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);
	signal(SIGPIPE, SIG_IGN);

	for (int i = 0; i < cfg.n_wans; i++) {
		struct wan_state *w = &g_wans[i];
		w->cfg = &cfg;
		w->idx = i;
		w->subflow_idx = cfg.wan[i].subflow_idx;
		strncpy(w->iface, cfg.wan[i].iface, IFNAMSIZ - 1);
		w->server_sa = server_sa;
		w->rtt_baseline_us = 0;
		w->jitter_us = 0;

		w->sock = open_wan_socket(w->iface, &w->server_sa);
		if (w->sock < 0) { log_msg(LOG_ERR, "wan %s: socket failed", w->iface); continue; }
		w->timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC);
		if (w->timer_fd < 0) { log_msg(LOG_ERR, "timerfd_create: %s", strerror(errno)); continue; }
		w->samples_fd = open_samples_sock(cfg.samples_sock);
		w->weights_fd = g_weights_fd;

		if (pthread_create(&w->tid, NULL, wan_thread, w) != 0) {
			log_msg(LOG_ERR, "pthread_create for %s: %s", w->iface, strerror(errno));
		}
	}

	for (int i = 0; i < cfg.n_wans; i++) {
		if (g_wans[i].tid) pthread_join(g_wans[i].tid, NULL);
	}

	close(g_weights_fd);
	free(g_wans);
	closelog();
	return 0;
}
