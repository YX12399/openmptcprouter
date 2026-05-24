#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# build-in-docker.sh -- Mac/Windows friendly wrapper for the OMR
# OpenWrt build. Runs the build inside a Debian container so you don't
# have to set up the Linux build host yourself.
#
# Usage (from anywhere):
#   ratan/scripts/build-in-docker.sh                 # default: rpi4
#   ratan/scripts/build-in-docker.sh rpi4
#   ratan/scripts/build-in-docker.sh x86_64
#
# What this does:
#   1. Builds (and caches) the Debian build-environment image.
#   2. Runs the OMR build inside a container.
#   3. Output lands in <repo>/source/bin/targets/... on the host.
#
# Resource requirements (Docker Desktop -> Settings -> Resources):
#   - RAM:  >= 6 GB (default 4 GB usually OK; bump if OOM)
#   - Disk: >= 40 GB for the container's working set
#   - CPUs: more = faster build; default 4 is fine
# First build: 2-4 hours. Subsequent: minutes (incremental).

set -euo pipefail

TARGET="${1:-rpi4}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
IMG_TAG="ratan-build:latest"

# Sanity: docker present?
if ! command -v docker >/dev/null 2>&1; then
    cat >&2 <<EOF
ERROR: docker not found in PATH.
  Mac:     install Docker Desktop -> https://www.docker.com/products/docker-desktop/
  Linux:   sudo apt install docker.io   (or your distro's equivalent)
  Windows: install Docker Desktop with WSL2 backend
EOF
    exit 1
fi
if ! docker info >/dev/null 2>&1; then
    cat >&2 <<EOF
ERROR: docker daemon not running.
  Mac: open Docker Desktop application; wait for the whale icon in
       the menu bar to stop animating, then re-run this script.
EOF
    exit 1
fi

cat <<EOF

=================================================================
RATAN OpenMPTCProuter build (inside Docker)

  Target:   $TARGET
  Repo:     $REPO_DIR
  Image:    $IMG_TAG

  This will take a few hours on first run. The container builds
  the entire OpenWrt + OMR + ratan stack. You can leave it running
  in the background; pressing Ctrl+C cleanly aborts (the build is
  resumable on next invocation).
=================================================================

EOF

echo "==> [1/2] Building Docker build-environment image (cached after first run)..."
docker build -t "$IMG_TAG" -f "$SCRIPT_DIR/Dockerfile" "$SCRIPT_DIR/"

echo ""
echo "==> [2/2] Running OMR build for target=$TARGET..."
echo ""
docker run --rm -it \
    -v "$REPO_DIR":/build \
    -w /build \
    "$IMG_TAG" \
    bash -c "git config --global --add safe.directory /build && \
             ratan/scripts/build-openwrt.sh '$TARGET'"

echo ""
echo "==> Build finished. Looking for images..."
IMAGES=$(find "$REPO_DIR/source/bin/targets/" -name '*.img.gz' 2>/dev/null | head)
if [ -z "$IMAGES" ]; then
    echo "    (no .img.gz files found yet; check the build log above for errors)"
else
    echo "Images produced:"
    echo "$IMAGES" | while read -r f; do
        sz=$(du -h "$f" | cut -f1)
        echo "  $sz  $f"
    done
fi

cat <<EOF

Next step: flash the image to your SD card.
  Mac: open Raspberry Pi Imager (https://www.raspberrypi.com/software/)
       -> Choose OS -> "Use custom" -> pick the .img.gz above
       -> Choose Storage -> select your SD card -> Write.
EOF
