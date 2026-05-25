// SPDX-License-Identifier: GPL-2.0
#ifndef RATAN_MOS_H
#define RATAN_MOS_H

#include <pthread.h>
#include <signal.h>

/* Spawn the MOS computer thread. Returns 0 on success.
 *
 * Reads from the same SQLite DB the daemon writes to: every second it
 * queries for currently-active VideoCall/* flows and the last-1s
 * per-WAN sample aggregates, computes MOS per flow, and emits
 * RATAN_EVENT_MOS envelopes back to the telemetry socket (so the daemon
 * persists + broadcasts via SSE just like every other event).
 *
 *   telem_sock_path: where to send envelopes.
 *   db_path:         SQLite DB to query (read-only).
 *   category_glob:   simple substring match against the flow's category.
 *                    Default "VideoCall/" matches Teams/Zoom/Meet/etc.
 *   stop_flag:       set by main on shutdown.
 */
int mos_start(const char *telem_sock_path, const char *db_path,
	      const char *category_glob,
	      volatile sig_atomic_t *stop_flag, pthread_t *out_tid);

#endif
