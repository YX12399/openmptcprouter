/* SPDX-License-Identifier: GPL-2.0 */
/*
 * ratan_proto.h -- on-the-wire formats shared between prober, responder,
 * and the predictor / classifier. Plain C structs, no padding tricks,
 * little-endian wire format (matches every Starlink/cell deployment we
 * care about). Same header used by the C prober, the VPS responder,
 * and -- via cffi -- the Python predictor.
 */

#ifndef RATAN_PROTO_H
#define RATAN_PROTO_H

#include <stdint.h>

#define RATAN_PROTO_VERSION  1

/*
 * Tagged-union envelope used on /run/ratan/telemetry.sock.
 *
 * Wire layout per datagram:
 *     [struct ratan_event_hdr][payload of hdr.len bytes]
 *
 * Prober writes RATAN_EVENT_SAMPLE; predictor writes _STATE / _WEIGHT;
 * Step 4b additions write _DISCOVERY / _FLOW / _MOS / _INJECT.
 *
 * telemetryd is the sole reader. It dispatches by kind, persists to
 * SQLite, broadcasts to SSE subscribers, exposes Prometheus counters.
 */
struct ratan_event_hdr {
	uint8_t  kind;       /* RATAN_EVENT_* */
	uint8_t  version;    /* RATAN_PROTO_VERSION */
	uint16_t len;        /* payload length */
} __attribute__((packed));

#define RATAN_EVENT_SAMPLE     1
#define RATAN_EVENT_STATE      2   /* classifier FSM transition */
#define RATAN_EVENT_WEIGHT     3   /* weight change */
#define RATAN_EVENT_DISCOVERY  4   /* Step 4b: WAN/LAN/MPTCP discovery */
#define RATAN_EVENT_FLOW       5   /* Step 4b: per-flow event */
#define RATAN_EVENT_MOS        6   /* Step 4b: per-flow MOS sample */
#define RATAN_EVENT_INJECT     7   /* Step 4b: test inject marker */

/* UDP probe packet (request) and echo (response). 24 bytes.
 * Same on wire to the VPS responder; not part of the telemetry envelope. */
struct ratan_probe {
	uint8_t  version;      /* RATAN_PROTO_VERSION */
	uint8_t  flags;        /* bit 0: 1 = response, 0 = request */
	uint16_t wan_id;       /* sender's WAN index (0..MPTCP_SUBFLOWS_MAX-1) */
	uint32_t seq;          /* per-WAN monotonic sequence */
	uint64_t tx_ns;        /* sender's CLOCK_MONOTONIC at TX (req) -- echoed */
	uint64_t rx_ns;        /* responder's CLOCK_MONOTONIC at RX (resp); 0 in req */
} __attribute__((packed));

#define RATAN_PROBE_FLAG_RESPONSE  0x01

/* Sample payload (RATAN_EVENT_SAMPLE). 28 bytes. */
struct ratan_sample {
	uint64_t ts_ns;        /* CLOCK_MONOTONIC when sample generated */
	uint8_t  wan_id;
	uint8_t  event;        /* RATAN_SAMPLE_* */
	uint16_t _pad;
	uint32_t seq;
	uint32_t rtt_us;       /* 0 on loss */
	uint32_t loss_count;   /* running consecutive-loss count */
	uint32_t jitter_us;    /* EWMA |rtt_i - rtt_baseline| */
} __attribute__((packed));

#define RATAN_SAMPLE_OK              0
#define RATAN_SAMPLE_LOSS            1
#define RATAN_SAMPLE_FAST_FAILOVER   2  /* prober just wrote weight=0 to BPF map */
#define RATAN_SAMPLE_RECOVERY        3  /* first OK after loss streak */

/* State-transition payload (RATAN_EVENT_STATE). Up to 80 bytes. */
struct ratan_state_event {
	uint64_t ts_ns;
	uint8_t  wan_id;
	uint8_t  _pad[3];
	char     from_state[16];   /* e.g. "HEALTHY" */
	char     to_state[16];
	char     reason[32];       /* short tag for the transition */
} __attribute__((packed));

/* Weight-change payload (RATAN_EVENT_WEIGHT). 24 bytes. */
struct ratan_weight_event {
	uint64_t ts_ns;
	uint8_t  wan_id;
	uint8_t  _pad[3];
	uint32_t weight;       /* 0..100 */
	char     source[8];    /* "predict", "prober", "manual" */
} __attribute__((packed));

/* Inject-marker payload (RATAN_EVENT_INJECT). Test harness writes one of
 * these every time it triggers / restores a tc netem condition, so the
 * timeline in any UI graph has synchronized vertical markers. 112 bytes. */
struct ratan_inject_event {
	uint64_t ts_ns;
	int32_t  session_id;   /* must reference an active session */
	char     action[16];   /* "netem_drop", "netem_restore", "netem_blip",
	                          "netem_delay", "mark", "shell" */
	char     target[16];   /* iface name or "" */
	char     detail[64];   /* free-form (loss%, duration, etc.) */
} __attribute__((packed));

/* Discovery-event payload (RATAN_EVENT_DISCOVERY). Written by the in-process
 * discover thread on netlink + inotify events. 136 bytes.
 *   kind:   one of "iface_up", "iface_down", "addr_add", "addr_del",
 *           "neighbor_add", "neighbor_del", "dhcp_lease_add",
 *           "dhcp_lease_del", "snapshot_iface", "snapshot_addr",
 *           "snapshot_neighbor", "snapshot_lease"
 *   detail: compact JSON, e.g.
 *           {"iface":"wan0","mtu":1500,"flags":"UP,RUNNING"}
 *           {"iface":"wan0","addr":"192.168.1.5/24","family":4}
 *           {"iface":"br-lan","ip":"192.168.1.20","mac":"aa:bb:cc:dd:ee:ff"}
 *           {"mac":"aa:..","ip":"...","host":"laptop","expiry":1700000000}
 */
struct ratan_discovery_event {
	uint64_t ts_ns;
	char     kind[24];
	char     detail[104];
} __attribute__((packed));

#endif /* RATAN_PROTO_H */
