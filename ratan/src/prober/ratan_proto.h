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

/* UDP probe packet (request) and echo (response). 24 bytes. */
struct ratan_probe {
	uint8_t  version;      /* RATAN_PROTO_VERSION */
	uint8_t  flags;        /* bit 0: 1 = response, 0 = request */
	uint16_t wan_id;       /* sender's WAN index (0..MPTCP_SUBFLOWS_MAX-1) */
	uint32_t seq;          /* per-WAN monotonic sequence */
	uint64_t tx_ns;        /* sender's CLOCK_MONOTONIC at TX (req) -- echoed verbatim */
	uint64_t rx_ns;        /* responder's CLOCK_MONOTONIC at RX (resp); 0 in req */
} __attribute__((packed));

#define RATAN_PROBE_FLAG_RESPONSE  0x01

/* Sample emitted over /run/ratan/samples.sock to the predictor.
 * One per probe round-trip (or one loss event). 32 bytes. */
struct ratan_sample {
	uint64_t ts_ns;        /* CLOCK_MONOTONIC when sample generated */
	uint8_t  wan_id;
	uint8_t  event;        /* RATAN_EVENT_* */
	uint16_t _pad;
	uint32_t seq;
	uint32_t rtt_us;       /* 0 on loss */
	uint32_t loss_count;   /* running consecutive-loss count */
	uint32_t jitter_us;    /* EWMA |rtt_i - rtt_baseline| */
} __attribute__((packed));

#define RATAN_EVENT_OK              0
#define RATAN_EVENT_LOSS            1
#define RATAN_EVENT_FAST_FAILOVER   2  /* prober just wrote weight=0 to BPF map */
#define RATAN_EVENT_RECOVERY        3  /* first OK after loss streak */

#endif /* RATAN_PROTO_H */
