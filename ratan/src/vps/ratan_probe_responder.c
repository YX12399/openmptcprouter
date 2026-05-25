// SPDX-License-Identifier: GPL-2.0
/*
 * ratan_probe_responder.c -- VPS-side echo server for ratan_prober.
 *
 * Single UDP socket on the configured port. For every well-formed probe
 * request, fill in rx_ns and the response flag, swap nothing else (we
 * connectionlessly sendto the source), and reply.
 *
 * Stateless, no auth (Step 3 MVP). Add HMAC + rate limiting in the
 * hardening pass before prod.
 */

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <getopt.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#include "ratan_proto.h"

#define DEFAULT_PORT 5555

static volatile sig_atomic_t g_stop;
static void on_signal(int s) { (void)s; g_stop = 1; }

static uint64_t now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

int main(int argc, char **argv)
{
	uint16_t port = DEFAULT_PORT;
	const char *bind_addr = "0.0.0.0";
	bool ipv6 = false;
	int c;

	static const struct option opts[] = {
		{ "port", required_argument, 0, 'p' },
		{ "bind", required_argument, 0, 'b' },
		{ "ipv6", no_argument,       0, '6' },
		{ "help", no_argument,       0, 'h' },
		{ 0 }
	};
	while ((c = getopt_long(argc, argv, "p:b:6h", opts, NULL)) != -1) {
		switch (c) {
		case 'p': port = (uint16_t)atoi(optarg); break;
		case 'b': bind_addr = optarg; break;
		case '6': ipv6 = true; break;
		case 'h': default:
			fprintf(stderr,
				"Usage: %s [--port %u] [--bind 0.0.0.0] [--ipv6]\n",
				argv[0], DEFAULT_PORT);
			return c == 'h' ? 0 : 1;
		}
	}

	openlog("ratan-probe-responder", LOG_PID | LOG_PERROR, LOG_DAEMON);
	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);

	int fd = socket(ipv6 ? AF_INET6 : AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	if (fd < 0) { syslog(LOG_ERR, "socket: %s", strerror(errno)); return 1; }

	int one = 1;
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

	if (ipv6) {
		struct sockaddr_in6 sa = {
			.sin6_family = AF_INET6,
			.sin6_port = htons(port),
			.sin6_addr = in6addr_any,
		};
		if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
			syslog(LOG_ERR, "bind: %s", strerror(errno)); return 1;
		}
	} else {
		struct sockaddr_in sa = {
			.sin_family = AF_INET,
			.sin_port = htons(port),
		};
		if (inet_pton(AF_INET, bind_addr, &sa.sin_addr) != 1) {
			syslog(LOG_ERR, "bad --bind: %s", bind_addr); return 1;
		}
		if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
			syslog(LOG_ERR, "bind: %s", strerror(errno)); return 1;
		}
	}

	syslog(LOG_INFO, "listening on %s:%u (%s)", bind_addr, port, ipv6 ? "ipv6" : "ipv4");

	struct sockaddr_storage peer;
	socklen_t peerlen;
	struct ratan_probe pkt;
	ssize_t n;

	while (!g_stop) {
		peerlen = sizeof(peer);
		n = recvfrom(fd, &pkt, sizeof(pkt), 0,
			     (struct sockaddr *)&peer, &peerlen);
		if (n < 0) {
			if (errno == EINTR) continue;
			syslog(LOG_WARNING, "recvfrom: %s", strerror(errno));
			continue;
		}
		if (n != (ssize_t)sizeof(pkt)) continue;       /* drop malformed */
		if (pkt.version != RATAN_PROTO_VERSION) continue;
		if (pkt.flags & RATAN_PROBE_FLAG_RESPONSE) continue;  /* not for us */

		pkt.flags |= RATAN_PROBE_FLAG_RESPONSE;
		pkt.rx_ns = now_ns();
		(void)sendto(fd, &pkt, sizeof(pkt), MSG_DONTWAIT,
			     (struct sockaddr *)&peer, peerlen);
	}

	close(fd);
	closelog();
	return 0;
}
