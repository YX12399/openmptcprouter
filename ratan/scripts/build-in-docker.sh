#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# build-in-docker.sh -- Mac/Windows friendly wrapper for the OMR
# OpenWrt build. Runs the build inside a Debian container so you don't
# have to set up the Linux build host yourself.
#
# Usage:
#   ratan/scripts/build-in-docker.sh [target]
#   ratan/scripts/build-in-docker.sh rpi4
#   ratan/scripts/build-in-docker.sh --named-volume rpi4
#
# Modes:
#   default (bind-mount): the repo is mounted from the host; outputs
#     land in <repo>/source/bin/targets/... on the host. REQUIRES a
#     case-sensitive host filesystem (OpenWrt refuses otherwise).
#     On macOS that means an APFS (Case-sensitive) volume.
#   --named-volume:       repo is cloned INSIDE a Docker-managed Linux
#     volume (always case-sensitive). Use this on macOS when you don't
#     want to create a separate APFS volume. Pull the final image out
#     with: docker run --rm -v ratan-build-vol:/build -v "$PWD":/out \
#                  alpine cp /build/openmptcprouter/source/bin/...img.gz /out/
#
# Resource requirements (Docker Desktop -> Settings -> Resources):
#   - RAM:  >= 8 GB recommended (6 GB minimum)
#   - Disk: >= 40 GB for the container's working set
#   - CPUs: more = faster build
# First build: 2-4 hours. Subsequent: minutes (incremental).

set -euo pipefail

MODE="bindmount"
TARGET="rpi4"
while [ $# -gt 0 ]; do
    case "$1" in
        --named-volume) MODE="namedvol"; shift ;;
        --bindmount)    MODE="bindmount"; shift ;;
        -h|--help)
            sed -n '2,30p' "$0"
            exit 0 ;;
        *) TARGET="$1"; shift ;;
    esac
done

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
IMG_TAG="ratan-build:latest"
VOL_NAME="ratan-build-vol"
REPO_URL="${RATAN_REPO_URL:-https://github.com/YX12399/openmptcprouter.git}"
REPO_BRANCH="${RATAN_REPO_BRANCH:-claude/explore-openmptcprouter-z8iF1}"

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

# --- bindmount preflight: detect case-insensitive FS and fail fast -----------
if [ "$MODE" = "bindmount" ]; then
    probe="$REPO_DIR/.ratan-case-probe"
    mkdir -p "$probe"
    : > "$probe/CaseSensitive"
    if [ -e "$probe/casesensitive" ]; then
        rm -rf "$probe"
        cat >&2 <<EOF
ERROR: $REPO_DIR is on a case-INSENSITIVE filesystem.

OpenWrt's prereq check will refuse to build here. macOS APFS is
case-insensitive by default. Two ways forward:

  1) Easiest: create a case-sensitive APFS volume in Disk Utility
     (File -> Add APFS Volume, Format: APFS (Case-sensitive)),
     clone the repo into /Volumes/<name>/ and re-run this script
     from there.

  2) Alternative: use a Docker-managed Linux volume (always
     case-sensitive). Re-run this script with --named-volume:

         $0 --named-volume $TARGET

     The repo will be cloned inside the volume; pull the image out
     at the end with the docker cp command this script prints.
EOF
        exit 1
    fi
    rm -rf "$probe"
fi

cat <<EOF

=================================================================
RATAN OpenMPTCProuter build (inside Docker)

  Mode:     $MODE
  Target:   $TARGET
  Image:    $IMG_TAG
EOF
if [ "$MODE" = "bindmount" ]; then
    echo "  Repo:     $REPO_DIR  (host bind-mount)"
else
    echo "  Volume:   $VOL_NAME  (Docker-managed; case-sensitive)"
    echo "  Source:   $REPO_URL @ $REPO_BRANCH"
fi
cat <<EOF

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

if [ "$MODE" = "bindmount" ]; then
    docker run --rm -it \
        -v "$REPO_DIR":/build \
        -w /build \
        "$IMG_TAG" \
        bash -c "git config --global --add safe.directory /build && \
                 ratan/scripts/build-openwrt.sh '$TARGET'"

    echo ""
    echo "==> Build finished. Looking for images..."
    IMAGES=$(find "$REPO_DIR/source/bin/targets/" -name '*.img.gz' 2>/dev/null | head)
else
    # Create the volume if missing, clone the repo on first use, then build.
    docker volume inspect "$VOL_NAME" >/dev/null 2>&1 || docker volume create "$VOL_NAME"
    docker run --rm -it \
        -v "$VOL_NAME":/build \
        -w /build \
        "$IMG_TAG" \
        bash -c "
            set -e
            if [ ! -d openmptcprouter/.git ]; then
                echo '==> Cloning repo into the Docker volume (first run only)...'
                git clone --branch '$REPO_BRANCH' '$REPO_URL' openmptcprouter
            else
                echo '==> Updating repo in the Docker volume...'
                cd openmptcprouter && git fetch origin '$REPO_BRANCH' && git checkout '$REPO_BRANCH' && git pull --ff-only && cd ..
            fi
            cd openmptcprouter
            ratan/scripts/build-openwrt.sh '$TARGET'
        "

    echo ""
    echo "==> Build finished. Looking for images inside the volume..."
    IMAGES=$(docker run --rm -v "$VOL_NAME":/build "$IMG_TAG" \
        bash -c "find /build/openmptcprouter/source/bin/targets/ -name '*.img.gz' 2>/dev/null | head" || true)
fi

if [ -z "$IMAGES" ]; then
    echo "    (no .img.gz files found yet; check the build log above for errors)"
else
    echo "Images produced:"
    if [ "$MODE" = "bindmount" ]; then
        echo "$IMAGES" | while read -r f; do
            sz=$(du -h "$f" | cut -f1)
            echo "  $sz  $f"
        done
    else
        echo "$IMAGES"
        echo ""
        echo "To extract the image to your Mac:"
        first=$(echo "$IMAGES" | head -1)
        echo "  docker run --rm -v $VOL_NAME:/build -v \"\$PWD\":/out \\"
        echo "      $IMG_TAG cp '$first' /out/"
    fi
fi

cat <<EOF

Next step: flash the image to your SD card.
  Mac: open Raspberry Pi Imager (https://www.raspberrypi.com/software/)
       -> Choose OS -> "Use custom" -> pick the .img.gz above
       -> Choose Storage -> select your SD card -> Write.
EOF
