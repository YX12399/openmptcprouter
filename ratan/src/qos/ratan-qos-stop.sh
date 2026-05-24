#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
#
# ratan-qos-stop.sh -- reverse ratan-qos-apply.sh
#
# Idempotent: succeeds whether or not the framework was previously applied.

set -e

LOG_TAG="ratan-qos"
log() { logger -t "$LOG_TAG" -- "$*"; echo "$*" >&2; }

uci_q() { uci -q get "$1" 2>/dev/null; }

# Tear down nft ruleset (errors ignored)
nft -e 'delete table inet ratan_qos' 2>/dev/null || true

# Tear down tc root qdisc on each configured WAN (errors ignored)
WAN_SECTIONS=$(uci -q show ratan-qos 2>/dev/null | awk -F'[.=]' '/^ratan-qos\.[^.]+=wan$/ {print $2}')
for sect in $WAN_SECTIONS; do
    IFACE=$(uci_q ratan-qos.${sect}.iface)
    [ -z "$IFACE" ] && continue
    [ -d "/sys/class/net/$IFACE" ] || continue
    tc qdisc del dev "$IFACE" root 2>/dev/null || true
done

log "stopped"
exit 0
