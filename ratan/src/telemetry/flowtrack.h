// SPDX-License-Identifier: GPL-2.0
#ifndef RATAN_FLOWTRACK_H
#define RATAN_FLOWTRACK_H

#include <pthread.h>
#include <signal.h>

/* Spawn the conntrack/nDPI flow tracker thread. Returns 0 on success,
 * -1 if the CTNETLINK socket couldn't be opened (e.g. running without
 * CAP_NET_ADMIN or nf_conntrack module not loaded). On failure, the
 * daemon continues without flow tracking.
 *
 *   telem_sock_path: where to write RATAN_EVENT_FLOW envelopes.
 *   ndpi_categories: path to "<mark_id>  <category_name>" mapping file
 *                    (default /etc/ratan/ndpi-categories.conf). Reloaded
 *                    if you SIGHUP the daemon (TODO).
 *   stop_flag:       set by main on shutdown.
 *   out_tid:         optional out param.
 */
int flowtrack_start(const char *telem_sock_path, const char *ndpi_categories,
		    volatile sig_atomic_t *stop_flag, pthread_t *out_tid);

#endif
