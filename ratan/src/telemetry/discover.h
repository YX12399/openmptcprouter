// SPDX-License-Identifier: GPL-2.0
#ifndef RATAN_DISCOVER_H
#define RATAN_DISCOVER_H

#include <pthread.h>
#include <signal.h>

/* Spawn the discovery subscriber thread. Returns 0 on success.
 *   telem_sock_path: where to write RATAN_EVENT_DISCOVERY envelopes
 *                    (typically /run/ratan/telemetry.sock -- the same
 *                    socket the daemon itself listens on).
 *   lease_file:      dnsmasq lease file to inotify-watch (usually
 *                    /tmp/dhcp.leases on OpenWrt;
 *                    /var/lib/misc/dnsmasq.leases on Debian).
 *   stop_flag:       atomic stop signal; thread exits when set.
 *   out_tid:         optional out param for the pthread_t. */
int discover_start(const char *telem_sock_path, const char *lease_file,
		   volatile sig_atomic_t *stop_flag, pthread_t *out_tid);

#endif
