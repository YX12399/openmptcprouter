# RATAN/OpenMPTCProuter build environment
# ----------------------------------------
# A reproducible Debian-based Docker image with every dep the OpenWrt
# build system needs. Built specifically so Mac users can build the
# image without fighting macOS's case-insensitive filesystem.
#
# Usage (from repo root):
#   docker build -t ratan-build -f ratan/scripts/Dockerfile ratan/scripts/
#   docker run --rm -it -v "$(pwd)":/build -w /build ratan-build \
#       bash -c "git config --global --add safe.directory /build && \
#                ratan/scripts/build-openwrt.sh rpi4"
#
# Or: ratan/scripts/build-in-docker.sh rpi4   (does both above for you)

FROM debian:bookworm

ENV DEBIAN_FRONTEND=noninteractive
ENV LANG=C.UTF-8

# All build deps OpenWrt + OMR + ratan need. Filtered for cross-arch
# compatibility -- OpenWrt's stock dep list includes gcc-multilib
# (32-bit x86 support) which doesn't exist on arm64 Debian. For an
# arm64 host (Apple Silicon Mac, Hetzner Ampere) building an arm64
# target (RPi4 / BCM2711) we don't need multilib -- host tools build
# with the native arm64 gcc, target tools build with OpenWrt's own
# cross toolchain.
RUN apt-get update && \
    apt-get install -y --no-install-recommends \
        build-essential clang flex bison g++ gawk \
        gettext git libncurses-dev libssl-dev \
        python3 python3-setuptools python3-distutils python3-yaml \
        rsync swig unzip zlib1g-dev file wget curl perl \
        ca-certificates xxd cpio gzip bzip2 xz-utils sudo \
        openssh-client time locales && \
    rm -rf /var/lib/apt/lists/* && \
    sed -i '/en_US.UTF-8/s/^# //g' /etc/locale.gen && locale-gen

# OpenWrt refuses to build as root. Create a 'builder' user with the
# same UID as the typical macOS user so the host filesystem mount
# stays writable from inside the container.
RUN useradd -m -u 1000 -s /bin/bash builder && \
    echo "builder ALL=(ALL) NOPASSWD: ALL" >> /etc/sudoers

USER builder
WORKDIR /build
ENV PATH="/home/builder/.local/bin:$PATH"

CMD ["bash"]
