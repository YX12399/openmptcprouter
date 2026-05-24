#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# build-openwrt.sh -- build an OMR firmware image with the RATAN feed enabled.
#
# Usage:
#   ratan/scripts/build-openwrt.sh                  # defaults: rpi4, kernel 6.6
#   ratan/scripts/build-openwrt.sh rpi4
#   ratan/scripts/build-openwrt.sh x86_64
#   OMR_KERNEL=6.12 ratan/scripts/build-openwrt.sh rpi4
#
# What this does:
#   1. Locates the OMR build.sh (the parent dir of ratan/).
#   2. Sets CUSTOM_FEED to the in-repo ratan/packaging/openwrt subdir.
#      This bypasses build.sh's URL-clone path -- needed because our
#      feed lives in a subdirectory of the OMR repo itself, not in a
#      separate repo. (build.sh line ~146: `if [ -n "$CUSTOM_FEED_URL" ]
#      && [ -z "$CUSTOM_FEED" ]; then` -- setting CUSTOM_FEED short-circuits
#      the URL clone.)
#   3. Sets OMR_DIST=ratan so the feed is src-link'd as 'ratan' (not
#      colliding with the upstream 'openmptcprouter' feed) and so
#      CONFIG_PACKAGE_ratan-full=y gets auto-emitted (build.sh line ~341).
#   4. Exec's build.sh with everything wired up.
#
# Output: image in source/bin/targets/* once the build completes (hours).

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
RATAN_DIR="$(realpath "$SCRIPT_DIR/..")"
OMR_DIR="$(realpath "$RATAN_DIR/..")"
FEED_DIR="$RATAN_DIR/packaging/openwrt"

OMR_TARGET="${1:-${OMR_TARGET:-rpi4}}"
OMR_KERNEL="${OMR_KERNEL:-6.6}"
OMR_DIST="${OMR_DIST:-ratan}"
OMR_PACKAGES="${OMR_PACKAGES:-full}"

[ -d "$FEED_DIR" ]          || { echo "feed dir missing: $FEED_DIR"           >&2; exit 1; }
[ -f "$OMR_DIR/build.sh" ]  || { echo "not an OMR checkout: $OMR_DIR"         >&2; exit 1; }
[ -d "$FEED_DIR/ratan-full" ] || { echo "ratan-full pkg missing in $FEED_DIR" >&2; exit 1; }
[ -d "$FEED_DIR/ratan-sched" ] || { echo "ratan-sched pkg missing in $FEED_DIR" >&2; exit 1; }

cat <<EOF
==> RATAN OpenWrt build
    OMR_DIR:      $OMR_DIR
    OMR_TARGET:   $OMR_TARGET
    OMR_KERNEL:   $OMR_KERNEL
    OMR_DIST:     $OMR_DIST
    OMR_PACKAGES: $OMR_PACKAGES
    CUSTOM_FEED:  $FEED_DIR
==> Invoking OMR build.sh ...
EOF

cd "$OMR_DIR"
exec env \
	OMR_TARGET="$OMR_TARGET" \
	OMR_KERNEL="$OMR_KERNEL" \
	OMR_DIST="$OMR_DIST" \
	OMR_PACKAGES="$OMR_PACKAGES" \
	CUSTOM_FEED="$FEED_DIR" \
	./build.sh
