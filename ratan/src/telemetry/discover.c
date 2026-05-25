// SPDX-License-Identifier: GPL-2.0
/*
 * discover.c -- in-process discovery subscriber for ratan_telemetryd.
 *
 * Runs as a pthread launched by main(). Subscribes to:
 *   - RTNETLINK groups RTMGRP_LINK | IPV4_IFADDR | IPV6_IFADDR | NEIGH.
 *     Emits iface_up/down, addr_add/del, neighbor_add/del.
 *   - Initial snapshot via RTM_GETLINK / GETADDR / GETNEIGH dumps so the
 *     UI sees current state immediately on startup.
 *   - inotify on /tmp/dhcp.leases (OpenWrt dnsmasq default; overridable
 *     via the LEASE_FILE env knob the daemon passes in). MAC-keyed diff
 *     to emit dhcp_lease_add/del on each modification.
 *
 * Output: writes RATAN_EVENT_DISCOVERY envelopes to the same Unix
 * datagram telemetry socket the prober uses. The daemon's main loop
 * ingests them via the existing dispatch table.
 *
 * Deferred (TODO, document in plan):
 *   - hostapd UBUS subscriber for WiFi assoc/disassoc (needs libubus;
 *     OMR-only).
 *   - MPTCP genetlink subscriber for MPTCP_PM_CMD_* path notifications.
 *
 * No libnl dep; raw netlink keeps the dep tree clean.
 */

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <net/if.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#include "../prober/ratan_proto.h"
#include "discover.h"

#define MAX_LEASES 256
#define INOTIFY_BUF (16 * (sizeof(struct inotify_event) + NAME_MAX + 1))

/* DHCP lease row, file scope (C doesn't permit struct-name :: scoping). */
struct lease_row {
	char mac[18];   /* "aa:bb:cc:dd:ee:ff\0" */
	char ip[64];    /* big enough for IPv6 */
	char host[64];
	long expiry;
};

/* Per-thread state. Allocated once by discover_start() and passed in. */
struct discover_state {
	pthread_t       tid;
	int             telem_sock;          /* AF_UNIX SOCK_DGRAM client */
	struct sockaddr_un telem_sa;
	int             rtnl_fd;             /* RTNETLINK subscription */
	int             inotify_fd;
	int             inotify_wd;          /* watch descriptor for lease file */
	char            lease_file[128];

	struct lease_row leases[MAX_LEASES];
	int             n_leases;
	volatile sig_atomic_t *stop;
};

/* Forward decl for ratan_telemetryd's logger; resolved at link time. */
extern void logf_(int prio, const char *fmt, ...);
#define LOGI(...) logf_(LOG_INFO,    __VA_ARGS__)
#define LOGW(...) logf_(LOG_WARNING, __VA_ARGS__)
#define LOGE(...) logf_(LOG_ERR,     __VA_ARGS__)

static uint64_t now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

/* ---------- emit ---------- */

static void emit(struct discover_state *st, const char *kind, const char *detail_fmt, ...)
{
	struct {
		struct ratan_event_hdr        hdr;
		struct ratan_discovery_event  payload;
	} __attribute__((packed)) frame = {
		.hdr = {
			.kind = RATAN_EVENT_DISCOVERY,
			.version = RATAN_PROTO_VERSION,
			.len = sizeof(struct ratan_discovery_event),
		},
		.payload = { .ts_ns = now_ns() },
	};
	strncpy(frame.payload.kind, kind, sizeof(frame.payload.kind) - 1);

	va_list ap;
	va_start(ap, detail_fmt);
	vsnprintf(frame.payload.detail, sizeof(frame.payload.detail), detail_fmt, ap);
	va_end(ap);

	if (sendto(st->telem_sock, &frame, sizeof(frame), MSG_DONTWAIT,
		   (struct sockaddr *)&st->telem_sa, sizeof(st->telem_sa)) < 0) {
		/* daemon ingest loop is in the same process; if this fails the
		 * daemon is in trouble already. Log once and continue. */
		if (errno != EAGAIN && errno != EWOULDBLOCK)
			LOGW("discover: sendto telem sock: %s", strerror(errno));
	}
}

/* ---------- netlink helpers ---------- */

static int rtnl_open(void)
{
	int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
	if (fd < 0) return -1;

	int rcvbuf = 1 << 20;
	setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

	struct sockaddr_nl sa = {
		.nl_family = AF_NETLINK,
		.nl_groups = RTMGRP_LINK | RTMGRP_IPV4_IFADDR |
			     RTMGRP_IPV6_IFADDR | RTMGRP_NEIGH,
	};
	if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
		close(fd); return -1;
	}
	return fd;
}

/* Send an RTM_GET* DUMP request to seed initial state. */
static int rtnl_dump(int fd, int type, int family)
{
	struct {
		struct nlmsghdr  nh;
		struct rtgenmsg  g;
	} req = {
		.nh = {
			.nlmsg_len = NLMSG_LENGTH(sizeof(req.g)),
			.nlmsg_type = type,
			.nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP,
		},
		.g = { .rtgen_family = (unsigned char)family },
	};
	struct sockaddr_nl kernel = { .nl_family = AF_NETLINK };
	if (sendto(fd, &req, req.nh.nlmsg_len, 0,
		   (struct sockaddr *)&kernel, sizeof(kernel)) < 0)
		return -1;
	return 0;
}

/* Walk RTA chain looking for a specific attr; returns its data or NULL. */
static const void *rta_find(const struct rtattr *rta, int rta_len, int type, int *out_len)
{
	for (; RTA_OK(rta, rta_len); rta = RTA_NEXT(rta, rta_len)) {
		if (rta->rta_type == type) {
			if (out_len) *out_len = (int)RTA_PAYLOAD(rta);
			return RTA_DATA(rta);
		}
	}
	return NULL;
}

static void fmt_flags(unsigned int flags, char *out, size_t outsz)
{
	out[0] = '\0';
	size_t l = 0;
#define A(f, s) if ((flags) & (f)) { l += snprintf(out + l, outsz - l, "%s%s", l ? "," : "", s); }
	/* Only POSIX flags from <net/if.h>; LOWER_UP is a Linux extension
	 * in <linux/if.h> which conflicts with the POSIX header. We can
	 * still get the carrier state via the IFLA_OPERSTATE attr if we
	 * decide we need it later. */
	A(IFF_UP,       "UP");
	A(IFF_RUNNING,  "RUNNING");
	A(IFF_LOOPBACK, "LOOPBACK");
	A(IFF_BROADCAST,"BROADCAST");
	A(IFF_NOARP,    "NOARP");
	A(IFF_POINTOPOINT,"POINTOPOINT");
#undef A
}

static void fmt_mac(const unsigned char *m, int n, char *out, size_t outsz)
{
	if (n != 6) { snprintf(out, outsz, "?"); return; }
	snprintf(out, outsz, "%02x:%02x:%02x:%02x:%02x:%02x",
		 m[0], m[1], m[2], m[3], m[4], m[5]);
}

/* ---------- RTM handlers ---------- */

static void handle_link(struct discover_state *st, const struct nlmsghdr *nh, bool snapshot)
{
	const struct ifinfomsg *ifi = NLMSG_DATA(nh);
	int attr_len = (int)IFLA_PAYLOAD(nh);
	const struct rtattr *rta = IFLA_RTA(ifi);
	int name_len = 0;
	const char *name = rta_find(rta, attr_len, IFLA_IFNAME, &name_len);
	int mtu_len = 0;
	const unsigned int *mtu = rta_find(rta, attr_len, IFLA_MTU, &mtu_len);

	char iface[32] = "?";
	if (name) snprintf(iface, sizeof(iface), "%.*s", name_len, name);

	char flags[64];
	fmt_flags(ifi->ifi_flags, flags, sizeof(flags));

	const char *kind;
	if (snapshot)                          kind = "snapshot_iface";
	else if (nh->nlmsg_type == RTM_DELLINK) kind = "iface_down";
	else if (ifi->ifi_flags & IFF_UP)       kind = "iface_up";
	else                                    kind = "iface_down";

	emit(st, kind,
	     "{\"iface\":\"%s\",\"index\":%d,\"mtu\":%u,\"flags\":\"%s\"}",
	     iface, ifi->ifi_index,
	     mtu ? *mtu : 0, flags);
}

static void handle_addr(struct discover_state *st, const struct nlmsghdr *nh, bool snapshot)
{
	const struct ifaddrmsg *ifa = NLMSG_DATA(nh);
	int attr_len = (int)IFA_PAYLOAD(nh);
	const struct rtattr *rta = IFA_RTA(ifa);

	int alen = 0;
	const void *addr = rta_find(rta, attr_len, IFA_LOCAL, &alen);
	if (!addr) addr = rta_find(rta, attr_len, IFA_ADDRESS, &alen);
	if (!addr) return;

	char addr_s[INET6_ADDRSTRLEN] = "?";
	inet_ntop(ifa->ifa_family, addr, addr_s, sizeof(addr_s));

	char iface[IFNAMSIZ] = "?";
	if_indextoname(ifa->ifa_index, iface);

	const char *kind = snapshot ? "snapshot_addr"
			   : (nh->nlmsg_type == RTM_DELADDR ? "addr_del" : "addr_add");

	emit(st, kind,
	     "{\"iface\":\"%s\",\"addr\":\"%s/%u\",\"family\":%u}",
	     iface, addr_s, ifa->ifa_prefixlen, ifa->ifa_family);
}

static void handle_neigh(struct discover_state *st, const struct nlmsghdr *nh, bool snapshot)
{
	const struct ndmsg *nd = NLMSG_DATA(nh);
	/* Skip transient states that aren't actually a neighbor */
	if (nd->ndm_state & (NUD_FAILED | NUD_NOARP | NUD_INCOMPLETE | NUD_NONE))
		return;

	int attr_len = (int)(nh->nlmsg_len - NLMSG_LENGTH(sizeof(*nd)));
	const struct rtattr *rta = (const struct rtattr *)((const char *)nd + NLMSG_ALIGN(sizeof(*nd)));

	int dlen = 0, llen = 0;
	const void *dst = rta_find(rta, attr_len, NDA_DST, &dlen);
	const void *ll  = rta_find(rta, attr_len, NDA_LLADDR, &llen);
	if (!dst) return;

	char ip[INET6_ADDRSTRLEN] = "?";
	inet_ntop(nd->ndm_family, dst, ip, sizeof(ip));
	char mac[18] = "?";
	if (ll) fmt_mac(ll, llen, mac, sizeof(mac));

	char iface[IFNAMSIZ] = "?";
	if_indextoname(nd->ndm_ifindex, iface);

	const char *kind = snapshot ? "snapshot_neighbor"
			   : (nh->nlmsg_type == RTM_DELNEIGH ? "neighbor_del" : "neighbor_add");

	emit(st, kind,
	     "{\"iface\":\"%s\",\"ip\":\"%s\",\"mac\":\"%s\"}",
	     iface, ip, mac);
}

/* Drain a netlink fd; `snapshot` distinguishes dump-reply messages from
 * subsequent live events. Returns 1 if a NLMSG_DONE was seen (end of dump),
 * 0 otherwise, -1 on hard error. */
static int rtnl_drain(struct discover_state *st, int fd, bool snapshot)
{
	uint8_t buf[8192];
	int saw_done = 0;
	for (;;) {
		ssize_t n = recv(fd, buf, sizeof(buf), MSG_DONTWAIT);
		if (n < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK) break;
			if (errno == EINTR) continue;
			return -1;
		}
		if (n == 0) break;

		struct nlmsghdr *nh = (struct nlmsghdr *)buf;
		while (NLMSG_OK(nh, n)) {
			if (nh->nlmsg_type == NLMSG_DONE) { saw_done = 1; break; }
			if (nh->nlmsg_type == NLMSG_ERROR) {
				const struct nlmsgerr *e = NLMSG_DATA(nh);
				if (e->error) LOGW("discover: nlmsgerr: %s", strerror(-e->error));
			} else if (nh->nlmsg_type == RTM_NEWLINK || nh->nlmsg_type == RTM_DELLINK) {
				handle_link(st, nh, snapshot);
			} else if (nh->nlmsg_type == RTM_NEWADDR || nh->nlmsg_type == RTM_DELADDR) {
				handle_addr(st, nh, snapshot);
			} else if (nh->nlmsg_type == RTM_NEWNEIGH || nh->nlmsg_type == RTM_DELNEIGH) {
				handle_neigh(st, nh, snapshot);
			}
			nh = NLMSG_NEXT(nh, n);
		}
		if (!(MSG_DONTWAIT)) break;
	}
	return saw_done;
}

/* Sync dump: send GET, drain replies until NLMSG_DONE. Used for the
 * startup snapshot only -- after that we stay in subscription mode. */
static void rtnl_initial_snapshot(struct discover_state *st)
{
	int dump_fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
	if (dump_fd < 0) return;
	struct sockaddr_nl sa = { .nl_family = AF_NETLINK };
	bind(dump_fd, (struct sockaddr *)&sa, sizeof(sa));

	const struct { int type; int family; } dumps[] = {
		{ RTM_GETLINK,  AF_UNSPEC },
		{ RTM_GETADDR,  AF_UNSPEC },
		{ RTM_GETNEIGH, AF_UNSPEC },
	};
	for (size_t i = 0; i < sizeof(dumps)/sizeof(*dumps); i++) {
		if (rtnl_dump(dump_fd, dumps[i].type, dumps[i].family) < 0) continue;
		/* Drain with a small timeout so we don't block forever if the
		 * kernel decides not to reply. */
		for (int loops = 0; loops < 100; loops++) {
			struct pollfd p = { .fd = dump_fd, .events = POLLIN };
			int rc = poll(&p, 1, 200);
			if (rc <= 0) break;
			if (rtnl_drain(st, dump_fd, true) == 1) break;
		}
	}
	close(dump_fd);
}

/* ---------- DHCP lease file watch ---------- */

static int lease_open(struct discover_state *st)
{
	int fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
	if (fd < 0) return -1;
	/* Watch the directory + the file. Watching just the file is fragile
	 * because dnsmasq replaces it atomically (rename), which removes the
	 * watch. Watch the dir for CREATE/MOVED_TO of the basename and
	 * (re)attach to the file on each event. */
	char dir[128];
	strncpy(dir, st->lease_file, sizeof(dir) - 1); dir[sizeof(dir) - 1] = '\0';
	char *slash = strrchr(dir, '/');
	if (!slash) { close(fd); return -1; }
	*slash = '\0';
	if (inotify_add_watch(fd, dir, IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE) < 0) {
		LOGW("discover: inotify dir %s: %s", dir, strerror(errno));
	}
	if (access(st->lease_file, R_OK) == 0) {
		st->inotify_wd = inotify_add_watch(fd, st->lease_file,
						   IN_MODIFY | IN_CLOSE_WRITE);
		if (st->inotify_wd < 0)
			LOGW("discover: inotify lease %s: %s", st->lease_file, strerror(errno));
	} else {
		st->inotify_wd = -1;
		LOGI("discover: lease file %s not present yet; watching dir for creation",
		     st->lease_file);
	}
	return fd;
}

/* Parse one dnsmasq leases line into a row. Returns 1 if valid, else 0.
 * Format (whitespace-separated, 5 fields):
 *   expiry_ts  mac  ip  hostname  clientid
 * hostname may be "*" (no hostname known). */
static int lease_parse_line(const char *line, struct lease_row *out)
{
	long exp; char mac[32], ip[64], host[64];
	int n = sscanf(line, "%ld %31s %63s %63s", &exp, mac, ip, host);
	if (n < 3) return 0;
	out->expiry = exp;
	strncpy(out->mac, mac, sizeof(out->mac) - 1); out->mac[sizeof(out->mac) - 1] = '\0';
	strncpy(out->ip, ip, sizeof(out->ip) - 1);    out->ip[sizeof(out->ip) - 1] = '\0';
	if (n >= 4) {
		strncpy(out->host, host, sizeof(out->host) - 1);
		out->host[sizeof(out->host) - 1] = '\0';
	} else {
		out->host[0] = '\0';
	}
	return 1;
}

static int lease_read_snapshot(struct discover_state *st,
			       struct lease_row *rows, int max)
{
	FILE *f = fopen(st->lease_file, "r");
	if (!f) return 0;
	int n = 0;
	char line[512];
	while (n < max && fgets(line, sizeof(line), f)) {
		if (lease_parse_line(line, &rows[n])) n++;
	}
	fclose(f);
	return n;
}

static int row_find(const struct lease_row *rows, int n, const char *mac)
{
	for (int i = 0; i < n; i++)
		if (strcmp(rows[i].mac, mac) == 0) return i;
	return -1;
}

static void lease_diff_and_emit(struct discover_state *st)
{
	struct lease_row cur[MAX_LEASES];
	int nc = lease_read_snapshot(st, cur, MAX_LEASES);

	/* Adds: in cur, not in st->leases */
	for (int i = 0; i < nc; i++) {
		if (row_find(st->leases, st->n_leases, cur[i].mac) < 0) {
			emit(st, "dhcp_lease_add",
			     "{\"mac\":\"%s\",\"ip\":\"%s\",\"host\":\"%s\",\"expiry\":%ld}",
			     cur[i].mac, cur[i].ip, cur[i].host, cur[i].expiry);
		}
	}
	/* Removes: in st->leases, not in cur */
	for (int i = 0; i < st->n_leases; i++) {
		if (row_find(cur, nc, st->leases[i].mac) < 0) {
			emit(st, "dhcp_lease_del",
			     "{\"mac\":\"%s\",\"ip\":\"%s\",\"host\":\"%s\"}",
			     st->leases[i].mac, st->leases[i].ip, st->leases[i].host);
		}
	}
	memcpy(st->leases, cur, sizeof(cur[0]) * nc);
	st->n_leases = nc;
}

static void lease_initial_snapshot(struct discover_state *st)
{
	st->n_leases = lease_read_snapshot(st, st->leases, MAX_LEASES);
	for (int i = 0; i < st->n_leases; i++) {
		emit(st, "snapshot_lease",
		     "{\"mac\":\"%s\",\"ip\":\"%s\",\"host\":\"%s\",\"expiry\":%ld}",
		     st->leases[i].mac, st->leases[i].ip,
		     st->leases[i].host, st->leases[i].expiry);
	}
}

/* ---------- thread entry ---------- */

static void *discover_main(void *arg)
{
	struct discover_state *st = arg;

	LOGI("discover: starting; lease_file=%s", st->lease_file);

	rtnl_initial_snapshot(st);
	lease_initial_snapshot(st);

	st->rtnl_fd = rtnl_open();
	if (st->rtnl_fd < 0) {
		LOGE("discover: rtnl_open: %s -- live netlink events disabled",
		     strerror(errno));
	}
	st->inotify_fd = lease_open(st);
	if (st->inotify_fd < 0) {
		LOGE("discover: inotify_init: %s -- lease watch disabled",
		     strerror(errno));
	}

	while (!*st->stop) {
		struct pollfd p[2];
		int np = 0;
		if (st->rtnl_fd >= 0) {
			p[np].fd = st->rtnl_fd; p[np].events = POLLIN; np++;
		}
		if (st->inotify_fd >= 0) {
			p[np].fd = st->inotify_fd; p[np].events = POLLIN; np++;
		}
		if (np == 0) { sleep(5); continue; }

		int rc = poll(p, np, 1000);
		if (rc < 0) { if (errno == EINTR) continue; break; }
		if (rc == 0) continue;

		for (int i = 0; i < np; i++) {
			if (!(p[i].revents & POLLIN)) continue;
			if (st->rtnl_fd >= 0 && p[i].fd == st->rtnl_fd) {
				rtnl_drain(st, st->rtnl_fd, false);
			} else if (st->inotify_fd >= 0 && p[i].fd == st->inotify_fd) {
				char buf[INOTIFY_BUF];
				ssize_t n = read(st->inotify_fd, buf, sizeof(buf));
				if (n <= 0) continue;
				bool refresh = false;
				for (char *q = buf; q < buf + n; ) {
					struct inotify_event *e = (void *)q;
					if (e->len > 0) {
						const char *base = strrchr(st->lease_file, '/');
						base = base ? base + 1 : st->lease_file;
						if (strcmp(e->name, base) == 0 &&
						    (e->mask & (IN_CREATE | IN_MOVED_TO))) {
							/* dnsmasq atomic-replaced the file; re-watch */
							if (st->inotify_wd >= 0)
								inotify_rm_watch(st->inotify_fd, st->inotify_wd);
							st->inotify_wd = inotify_add_watch(
								st->inotify_fd, st->lease_file,
								IN_MODIFY | IN_CLOSE_WRITE);
							refresh = true;
						}
					} else {
						/* event for the watched file itself */
						refresh = true;
					}
					q += sizeof(*e) + e->len;
				}
				if (refresh) lease_diff_and_emit(st);
			}
		}
	}

	LOGI("discover: stopping");
	if (st->rtnl_fd >= 0) close(st->rtnl_fd);
	if (st->inotify_fd >= 0) close(st->inotify_fd);
	close(st->telem_sock);
	free(st);
	return NULL;
}

int discover_start(const char *telem_sock_path, const char *lease_file,
		   volatile sig_atomic_t *stop_flag, pthread_t *out_tid)
{
	struct discover_state *st = calloc(1, sizeof(*st));
	if (!st) return -1;

	st->telem_sock = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	if (st->telem_sock < 0) { free(st); return -1; }
	st->telem_sa.sun_family = AF_UNIX;
	strncpy(st->telem_sa.sun_path, telem_sock_path,
		sizeof(st->telem_sa.sun_path) - 1);
	strncpy(st->lease_file, lease_file, sizeof(st->lease_file) - 1);
	st->stop = stop_flag;

	if (pthread_create(&st->tid, NULL, discover_main, st) != 0) {
		close(st->telem_sock); free(st); return -1;
	}
	if (out_tid) *out_tid = st->tid;
	return 0;
}
