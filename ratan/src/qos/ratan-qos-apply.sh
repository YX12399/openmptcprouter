#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
#
# ratan-qos-apply.sh -- bring up the RATAN QoS shaping framework.
#
# What this does:
#   1. Reads /etc/config/ratan-qos (UCI) for per-WAN ifaces and capacities.
#   2. Builds an HTB tree on each WAN egress:
#        1:1 (root, total_kbps ceiling)
#          1:10 = realtime (rate=realtime_floor_kbps, ceil=total)
#          1:20 = bulk     (rate=remaining,            ceil=total)
#          1:30 = background (rate=1Mbps, ceil=total*bg_ceil_pct/100)
#      fq_codel leaf under each class.
#   3. Loads the nftables ruleset that maps ct->mark to tc priority.
#
# Idempotent: re-running tears down the previous state first.
#
# Hardware-verification gap: see ../../docs/qos.md for the assumption
# that ct->mark is populated by OMR's nDPI integration (or, later,
# by ratan-qos-marker).

set -e

NFT_FILE="${RATAN_QOS_NFT:-/etc/ratan/qos/ratan-qos.nft}"
LOG_TAG="ratan-qos"

log() { logger -t "$LOG_TAG" -- "$*"; echo "$*" >&2; }

# ---------- read UCI ----------
. /usr/share/libubox/jshn.sh 2>/dev/null || true

# Fallback: simple uci -q wrappers, even when jshn isn't available.
uci_q() { uci -q get "$1" 2>/dev/null; }

REALTIME_MIN_KBPS=$(uci_q ratan-qos.main.realtime_min_kbps); REALTIME_MIN_KBPS=${REALTIME_MIN_KBPS:-1500}
BG_CEIL_PCT=$(uci_q ratan-qos.main.bg_ceil_pct);             BG_CEIL_PCT=${BG_CEIL_PCT:-20}

# Enumerate `config wan ...` sections of /etc/config/ratan-qos.
WAN_SECTIONS=$(uci -q show ratan-qos 2>/dev/null | awk -F'[.=]' '/^ratan-qos\.[^.]+=wan$/ {print $2}')

if [ -z "$WAN_SECTIONS" ]; then
    log "no 'wan' sections in /etc/config/ratan-qos; nothing to apply"
    exit 0
fi

# ---------- helpers ----------

setup_iface_htb() {
    local iface="$1" total_kbps="$2"

    # tear down any existing root qdisc on this iface (ignore errors)
    tc qdisc del dev "$iface" root 2>/dev/null || true

    local rt_rate="${REALTIME_MIN_KBPS}kbit"
    local total_rate="${total_kbps}kbit"
    local bulk_rate=$(( total_kbps - REALTIME_MIN_KBPS ))
    [ "$bulk_rate" -lt 100 ] && bulk_rate=100
    local bg_ceil=$(( total_kbps * BG_CEIL_PCT / 100 ))
    [ "$bg_ceil" -lt 100 ] && bg_ceil=100

    log "iface=$iface total=${total_kbps}kbit realtime_floor=${REALTIME_MIN_KBPS}kbit bg_ceil=${bg_ceil}kbit"

    tc qdisc add dev "$iface" root handle 1: htb default 20
    tc class add dev "$iface" parent 1:  classid 1:1  htb \
        rate "$total_rate" ceil "$total_rate"
    # realtime: guaranteed floor, can use up to ceiling
    tc class add dev "$iface" parent 1:1 classid 1:10 htb \
        rate "${REALTIME_MIN_KBPS}kbit" ceil "$total_rate" prio 1
    # bulk: gets whatever's left, can also use up to ceiling
    tc class add dev "$iface" parent 1:1 classid 1:20 htb \
        rate "${bulk_rate}kbit" ceil "$total_rate" prio 2
    # background: small floor, capped ceiling so backups can't starve realtime
    tc class add dev "$iface" parent 1:1 classid 1:30 htb \
        rate "100kbit" ceil "${bg_ceil}kbit" prio 3
    # fq_codel leaves for fair queuing within each class
    tc qdisc add dev "$iface" parent 1:10 handle 10: fq_codel
    tc qdisc add dev "$iface" parent 1:20 handle 20: fq_codel
    tc qdisc add dev "$iface" parent 1:30 handle 30: fq_codel
}

# ---------- apply ----------

# 1. tc HTB on every configured WAN
for sect in $WAN_SECTIONS; do
    IFACE=$(uci_q ratan-qos.${sect}.iface)
    TOTAL_KBPS=$(uci_q ratan-qos.${sect}.total_kbps)
    [ -z "$IFACE" ] && { log "skip [$sect]: no iface"; continue; }
    [ -z "$TOTAL_KBPS" ] && { log "skip [$sect] $IFACE: no total_kbps"; continue; }
    if [ ! -d "/sys/class/net/$IFACE" ]; then
        log "skip [$sect] $IFACE: iface not present"
        continue
    fi
    setup_iface_htb "$IFACE" "$TOTAL_KBPS"
done

# 2. nft ruleset (classification by ct->mark)
if [ -r "$NFT_FILE" ]; then
    # Drop the old table if present, then load.
    nft -e 'delete table inet ratan_qos' 2>/dev/null || true
    if nft -f "$NFT_FILE"; then
        log "nft ruleset loaded from $NFT_FILE"
    else
        log "WARNING: nft -f $NFT_FILE failed"
    fi
else
    log "WARNING: $NFT_FILE missing; tc tree alive but no classification yet"
fi

log "applied"
exit 0
