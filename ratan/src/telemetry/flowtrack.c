// SPDX-License-Identifier: GPL-2.0
/*
 * flowtrack.c -- in-process CTNETLINK + nDPI flow tracker for telemetryd.
 *
 * Subscribes to the conntrack netlink multicast groups NEW + DESTROY,
 * parses the nested attribute tree to extract the 5-tuple + conntrack
 * mark, and emits a RATAN_EVENT_FLOW envelope per flow lifecycle event.
 *
 * The conntrack mark is assumed to carry the nDPI category id (the
 * convention OMR's ndpi-netfilter integration uses; verify on a real
 * OMR build by running `conntrack -L | grep mark=`). A mapping file at
 * /etc/ratan/ndpi-categories.conf translates mark -> human-readable
 * category name -- one "<int>  <name>" per line, '#' for comments.
 *
 * Flow table: bounded at FLOW_TABLE_CAP entries with LRU eviction.
 * Used only to debounce duplicate NEW events for the same 5-tuple and
 * to detect DESTROY for a flow we already know about. We do NOT emit
 * periodic FLOW_UPDATE in this slice -- start/end are enough; MOS
 * subscribes to the prober's sample stream for live per-WAN RTT.
 *
 * Container constraint: this code can't run under restricted privileges
 * (CAP_NET_ADMIN required to subscribe to CTNETLINK NEW/DESTROY groups).
 * On dev hosts without privilege, flowtrack_start() returns -1 and the
 * daemon logs a warning + continues. Production OpenWrt always has it.
 *
 * Deferred to a later hardening pass:
 *   - Periodic flow-update emission (every N seconds for active flows)
 *     so the UI can show per-flow byte counters live.
 *   - SIGHUP-driven reload of ndpi-categories.conf.
 *   - IPv6 over IPv4 tuple disambiguation (currently each is handled
 *     separately and the 'family' field distinguishes).
 */

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <linux/netfilter.h>
#include <linux/netfilter/nfnetlink.h>
#include <linux/netfilter/nfnetlink_conntrack.h>
#include <linux/netlink.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#include "../prober/ratan_proto.h"
#include "flowtrack.h"

#define FLOW_TABLE_CAP  4096
#define MAX_CATEGORIES   256

/* nfnetlink_conntrack.h is sparse on some headers; pull in the bits we
 * need that aren't always exposed. Defined in mainline kernels >=2.6. */
#ifndef NF_NETLINK_CONNTRACK_NEW
#define NF_NETLINK_CONNTRACK_NEW     0x00000001
#endif
#ifndef NF_NETLINK_CONNTRACK_UPDATE
#define NF_NETLINK_CONNTRACK_UPDATE  0x00000002
#endif
#ifndef NF_NETLINK_CONNTRACK_DESTROY
#define NF_NETLINK_CONNTRACK_DESTROY 0x00000004
#endif

extern void logf_(int prio, const char *fmt, ...);
#define LOGI(...) logf_(LOG_INFO,    __VA_ARGS__)
#define LOGW(...) logf_(LOG_WARNING, __VA_ARGS__)
#define LOGE(...) logf_(LOG_ERR,     __VA_ARGS__)

struct cat_entry {
	uint32_t mark;
	char     name[40];
};

struct flow_row {
	uint64_t ts_seen;    /* CLOCK_MONOTONIC ns; 0 = empty slot */
	uint8_t  proto;
	uint8_t  family;
	uint32_t mark;
	char     key[96];    /* "tcp:src->dst" or "udp:src->dst" */
	uint64_t bytes_orig;
	uint64_t bytes_reply;
};

struct flow_state {
	int                 ct_fd;          /* CTNETLINK subscription */
	int                 telem_sock;
	struct sockaddr_un  telem_sa;

	struct flow_row     table[FLOW_TABLE_CAP];

	struct cat_entry    cats[MAX_CATEGORIES];
	int                 n_cats;

	volatile sig_atomic_t *stop;
};

static uint64_t now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

/* ---------- ndpi categories.conf ---------- */

static void cat_load(struct flow_state *s, const char *path)
{
	FILE *f = fopen(path, "r");
	if (!f) {
		LOGW("flowtrack: %s not present; raw mark only, no category names", path);
		return;
	}
	char line[160];
	while (s->n_cats < MAX_CATEGORIES && fgets(line, sizeof(line), f)) {
		char *p = line;
		while (*p == ' ' || *p == '\t') p++;
		if (*p == '#' || *p == '\n' || *p == '\0') continue;
		uint32_t id;
		char name[64];
		if (sscanf(p, "%u %63s", &id, name) == 2) {
			s->cats[s->n_cats].mark = id;
			strncpy(s->cats[s->n_cats].name, name,
				sizeof(s->cats[s->n_cats].name) - 1);
			s->n_cats++;
		}
	}
	fclose(f);
	LOGI("flowtrack: loaded %d nDPI category mappings from %s",
	     s->n_cats, path);
}

static const char *cat_lookup(const struct flow_state *s, uint32_t mark)
{
	if (mark == 0) return "";
	for (int i = 0; i < s->n_cats; i++)
		if (s->cats[i].mark == mark) return s->cats[i].name;
	return "";
}

/* ---------- flow table (open addressing, LRU evict-when-full) ---------- */

static uint64_t hash_key(const char *key)
{
	/* FNV-1a 64-bit */
	uint64_t h = 0xcbf29ce484222325ULL;
	for (const unsigned char *p = (const void *)key; *p; p++) {
		h ^= *p;
		h *= 0x100000001b3ULL;
	}
	return h;
}

static int table_find(const struct flow_state *s, const char *key)
{
	uint64_t h = hash_key(key);
	for (int i = 0; i < FLOW_TABLE_CAP; i++) {
		int idx = (int)((h + i) % FLOW_TABLE_CAP);
		if (s->table[idx].ts_seen == 0) return -1;
		if (strcmp(s->table[idx].key, key) == 0) return idx;
	}
	return -1;
}

/* Insert (or update) at most-recently-touched slot. If full, evict the
 * oldest. Returns the slot index. */
static int table_upsert(struct flow_state *s, const char *key,
			uint8_t proto, uint8_t family, uint32_t mark,
			uint64_t bytes_orig, uint64_t bytes_reply)
{
	uint64_t h = hash_key(key);
	int free_slot = -1;
	for (int i = 0; i < FLOW_TABLE_CAP; i++) {
		int idx = (int)((h + i) % FLOW_TABLE_CAP);
		struct flow_row *r = &s->table[idx];
		if (r->ts_seen == 0) {
			if (free_slot < 0) free_slot = idx;
			break;
		}
		if (strcmp(r->key, key) == 0) {
			r->ts_seen     = now_ns();
			r->mark        = mark ? mark : r->mark;
			r->bytes_orig  = bytes_orig;
			r->bytes_reply = bytes_reply;
			return idx;
		}
	}

	if (free_slot < 0) {
		/* table is full; evict oldest by linear scan (4096 slots = us) */
		uint64_t min_ts = UINT64_MAX;
		int      victim = 0;
		for (int i = 0; i < FLOW_TABLE_CAP; i++) {
			if (s->table[i].ts_seen < min_ts) {
				min_ts = s->table[i].ts_seen;
				victim = i;
			}
		}
		free_slot = victim;
	}

	struct flow_row *r = &s->table[free_slot];
	strncpy(r->key, key, sizeof(r->key) - 1); r->key[sizeof(r->key) - 1] = '\0';
	r->ts_seen     = now_ns();
	r->proto       = proto;
	r->family      = family;
	r->mark        = mark;
	r->bytes_orig  = bytes_orig;
	r->bytes_reply = bytes_reply;
	return free_slot;
}

static void table_remove(struct flow_state *s, const char *key)
{
	int idx = table_find(s, key);
	if (idx < 0) return;
	memset(&s->table[idx], 0, sizeof(s->table[idx]));
}

/* ---------- emit ---------- */

static void emit(struct flow_state *s, const struct ratan_flow_event *fe)
{
	struct {
		struct ratan_event_hdr   hdr;
		struct ratan_flow_event  payload;
	} __attribute__((packed)) frame = {
		.hdr = {
			.kind    = RATAN_EVENT_FLOW,
			.version = RATAN_PROTO_VERSION,
			.len     = sizeof(struct ratan_flow_event),
		},
		.payload = *fe,
	};
	if (sendto(s->telem_sock, &frame, sizeof(frame), MSG_DONTWAIT,
		   (struct sockaddr *)&s->telem_sa, sizeof(s->telem_sa)) < 0) {
		if (errno != EAGAIN && errno != EWOULDBLOCK)
			LOGW("flowtrack: sendto: %s", strerror(errno));
	}
}

/* ---------- CTNETLINK parsing ---------- */

/* Walk a flat NLA chain. data points just past the nfgenmsg header. */
static const struct nlattr *nla_find(const void *data, int total_len, int type)
{
	const struct nlattr *na = data;
	int remaining = total_len;
	while (remaining >= (int)sizeof(*na)) {
		int alen = na->nla_len;
		if (alen < (int)sizeof(*na) || alen > remaining) break;
		if ((na->nla_type & NLA_TYPE_MASK) == type) return na;
		alen = NLA_ALIGN(alen);
		na = (const struct nlattr *)((const char *)na + alen);
		remaining -= alen;
	}
	return NULL;
}

static const void *nla_data(const struct nlattr *na) { return (const char *)na + NLA_HDRLEN; }
static int         nla_len_(const struct nlattr *na) { return na->nla_len - NLA_HDRLEN; }

/* CTA_TUPLE_ORIG is nested: contains CTA_TUPLE_IP (further nested) and
 * CTA_TUPLE_PROTO (further nested). Walks both to extract the 5-tuple.
 *
 * Returns 0 on success and fills out_proto / out_family / out_src_str /
 * out_dst_str. */
static int parse_tuple(const struct nlattr *tuple_attr, uint8_t *out_proto,
		       uint8_t *out_family,
		       char *out_src_str, size_t out_src_len,
		       char *out_dst_str, size_t out_dst_len)
{
	if (!tuple_attr) return -1;
	const void *body = nla_data(tuple_attr);
	int blen = nla_len_(tuple_attr);

	const struct nlattr *ip_a = nla_find(body, blen, CTA_TUPLE_IP);
	const struct nlattr *pr_a = nla_find(body, blen, CTA_TUPLE_PROTO);
	if (!ip_a || !pr_a) return -1;

	const void *ipb = nla_data(ip_a);
	int ipl = nla_len_(ip_a);
	const struct nlattr *src4 = nla_find(ipb, ipl, CTA_IP_V4_SRC);
	const struct nlattr *dst4 = nla_find(ipb, ipl, CTA_IP_V4_DST);
	const struct nlattr *src6 = nla_find(ipb, ipl, CTA_IP_V6_SRC);
	const struct nlattr *dst6 = nla_find(ipb, ipl, CTA_IP_V6_DST);

	const void *prb = nla_data(pr_a);
	int prl = nla_len_(pr_a);
	const struct nlattr *pn = nla_find(prb, prl, CTA_PROTO_NUM);
	const struct nlattr *sp = nla_find(prb, prl, CTA_PROTO_SRC_PORT);
	const struct nlattr *dp = nla_find(prb, prl, CTA_PROTO_DST_PORT);
	if (!pn) return -1;
	uint8_t proto = *(const uint8_t *)nla_data(pn);
	uint16_t sport = sp ? ntohs(*(const uint16_t *)nla_data(sp)) : 0;
	uint16_t dport = dp ? ntohs(*(const uint16_t *)nla_data(dp)) : 0;

	char saddr[INET6_ADDRSTRLEN] = "?", daddr[INET6_ADDRSTRLEN] = "?";

	if (src4 && dst4) {
		*out_family = 4;
		inet_ntop(AF_INET, nla_data(src4), saddr, sizeof(saddr));
		inet_ntop(AF_INET, nla_data(dst4), daddr, sizeof(daddr));
		snprintf(out_src_str, out_src_len, "%s:%u", saddr, sport);
		snprintf(out_dst_str, out_dst_len, "%s:%u", daddr, dport);
	} else if (src6 && dst6) {
		*out_family = 6;
		inet_ntop(AF_INET6, nla_data(src6), saddr, sizeof(saddr));
		inet_ntop(AF_INET6, nla_data(dst6), daddr, sizeof(daddr));
		snprintf(out_src_str, out_src_len, "[%s]:%u", saddr, sport);
		snprintf(out_dst_str, out_dst_len, "[%s]:%u", daddr, dport);
	} else {
		return -1;
	}
	*out_proto = proto;
	return 0;
}

static uint64_t parse_counters(const struct nlattr *attr)
{
	if (!attr) return 0;
	const void *body = nla_data(attr);
	int blen = nla_len_(attr);
	const struct nlattr *bytes = nla_find(body, blen, CTA_COUNTERS_BYTES);
	if (!bytes || nla_len_(bytes) < 8) return 0;
	uint64_t v;
	memcpy(&v, nla_data(bytes), 8);
	return be64toh(v);
}

static const char *proto_name(uint8_t p)
{
	switch (p) {
	case 6:   return "tcp";
	case 17:  return "udp";
	case 1:   return "icmp";
	case 58:  return "icmp6";
	case 132: return "sctp";
	default:  return "ip";
	}
}

static void handle_ct(struct flow_state *s, const struct nlmsghdr *nh, bool is_destroy)
{
	if (nh->nlmsg_len < NLMSG_LENGTH(sizeof(struct nfgenmsg))) return;
	const void *payload = (const char *)nh + NLMSG_HDRLEN + NLMSG_ALIGN(sizeof(struct nfgenmsg));
	int payload_len = (int)(nh->nlmsg_len - NLMSG_HDRLEN - NLMSG_ALIGN(sizeof(struct nfgenmsg)));
	if (payload_len < 0) return;

	const struct nlattr *t_orig  = nla_find(payload, payload_len, CTA_TUPLE_ORIG);
	const struct nlattr *mk_a    = nla_find(payload, payload_len, CTA_MARK);
	const struct nlattr *co_a    = nla_find(payload, payload_len, CTA_COUNTERS_ORIG);
	const struct nlattr *cr_a    = nla_find(payload, payload_len, CTA_COUNTERS_REPLY);

	if (!t_orig) return;
	uint8_t proto = 0, family = 0;
	char src[48] = "", dst[48] = "";
	if (parse_tuple(t_orig, &proto, &family,
			src, sizeof(src), dst, sizeof(dst)) < 0)
		return;

	uint32_t mark = 0;
	if (mk_a && nla_len_(mk_a) >= 4) {
		uint32_t v;
		memcpy(&v, nla_data(mk_a), 4);
		mark = ntohl(v);
	}
	uint64_t b_orig  = parse_counters(co_a);
	uint64_t b_reply = parse_counters(cr_a);

	char key[120];
	snprintf(key, sizeof(key), "%s:%s->%s", proto_name(proto), src, dst);

	if (is_destroy) {
		/* If we never saw the NEW (subscription started mid-flight or
		 * the table evicted it), still emit a synthetic END so the UI
		 * timeline closes. */
		table_remove(s, key);
	} else {
		int existing = table_find(s, key);
		table_upsert(s, key, proto, family, mark, b_orig, b_reply);
		if (existing >= 0) return;  /* dedupe duplicate NEW */
	}

	struct ratan_flow_event fe = {
		.ts_ns       = now_ns(),
		.event       = is_destroy ? RATAN_FLOW_END : RATAN_FLOW_START,
		.proto       = proto,
		.family      = family,
		.mark        = mark,
		.bytes_orig  = b_orig,
		.bytes_reply = b_reply,
	};
	strncpy(fe.src, src, sizeof(fe.src) - 1);
	strncpy(fe.dst, dst, sizeof(fe.dst) - 1);
	const char *cat = cat_lookup(s, mark);
	strncpy(fe.category, cat, sizeof(fe.category) - 1);

	emit(s, &fe);
}

/* ---------- thread main ---------- */

static int ct_open(void)
{
	int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_NETFILTER);
	if (fd < 0) return -1;
	int rcvbuf = 1 << 22;   /* 4 MiB for busy routers */
	setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
	struct sockaddr_nl sa = {
		.nl_family = AF_NETLINK,
		.nl_groups = NF_NETLINK_CONNTRACK_NEW | NF_NETLINK_CONNTRACK_DESTROY,
	};
	if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
		close(fd); return -1;
	}
	return fd;
}

static void *flowtrack_main(void *arg)
{
	struct flow_state *s = arg;
	LOGI("flowtrack: starting");

	uint8_t buf[8192];
	while (!*s->stop) {
		struct pollfd p = { .fd = s->ct_fd, .events = POLLIN };
		int rc = poll(&p, 1, 1000);
		if (rc < 0) { if (errno == EINTR) continue; break; }
		if (rc == 0) continue;
		if (!(p.revents & POLLIN)) continue;

		ssize_t n = recv(s->ct_fd, buf, sizeof(buf), MSG_DONTWAIT);
		if (n <= 0) {
			if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
				LOGW("flowtrack: recv: %s", strerror(errno));
			continue;
		}

		struct nlmsghdr *nh = (struct nlmsghdr *)buf;
		while (NLMSG_OK(nh, n)) {
			uint8_t subsys = (nh->nlmsg_type >> 8) & 0xff;
			uint8_t op     = nh->nlmsg_type & 0xff;
			if (subsys == NFNL_SUBSYS_CTNETLINK) {
				if (op == IPCTNL_MSG_CT_NEW)
					handle_ct(s, nh, false);
				else if (op == IPCTNL_MSG_CT_DELETE)
					handle_ct(s, nh, true);
			}
			nh = NLMSG_NEXT(nh, n);
		}
	}

	LOGI("flowtrack: stopping");
	close(s->ct_fd);
	close(s->telem_sock);
	free(s);
	return NULL;
}

int flowtrack_start(const char *telem_sock_path, const char *ndpi_categories,
		    volatile sig_atomic_t *stop_flag, pthread_t *out_tid)
{
	int ct_fd = ct_open();
	if (ct_fd < 0) return -1;

	struct flow_state *s = calloc(1, sizeof(*s));
	if (!s) { close(ct_fd); return -1; }
	s->ct_fd = ct_fd;
	s->stop = stop_flag;

	s->telem_sock = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	if (s->telem_sock < 0) { close(ct_fd); free(s); return -1; }
	s->telem_sa.sun_family = AF_UNIX;
	strncpy(s->telem_sa.sun_path, telem_sock_path,
		sizeof(s->telem_sa.sun_path) - 1);

	if (ndpi_categories && *ndpi_categories) cat_load(s, ndpi_categories);

	pthread_t tid;
	if (pthread_create(&tid, NULL, flowtrack_main, s) != 0) {
		close(s->ct_fd); close(s->telem_sock); free(s); return -1;
	}
	if (out_tid) *out_tid = tid;
	return 0;
}
