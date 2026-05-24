# RATAN

MPTCP/Starlink path-aware aggregation engine. Drives a custom **BPF struct_ops MPTCP scheduler** from a userspace path-quality predictor + fast prober, on top of an OpenMPTCProuter (OpenWrt) router image or a stock Ubuntu host.

Context and design rationale live in `RATANTCPLayerHyperPathResearch.md` (one level up, alongside the OMR fork README) and in `.claude/plans/look-through-and-through-joyful-ullman.md`.

## Layout (target end-state)

```
ratan/
├── src/                            source of truth (cross-distro)
│   ├── sched/                      BPF struct_ops MPTCP scheduler + libbpf loader  [STEP 1: present]
│   ├── predictor/                  userspace scoring daemon (Python)               [STEP 3]
│   ├── prober/                     50ms UDP prober + fast-failover writer (C)      [STEP 4]
│   ├── handover/                   Starlink handover learner (Python)              [STEP 5]
│   ├── qos/                        nftables + tc HTB + ndpi-hook kmod              [STEP 6]
│   └── vps/                        ratan_probe_responder (VPS-side echo)           [STEP 7]
├── packaging/
│   ├── openwrt/                    OpenWrt feed (CUSTOM_FEED_URL points here)      [STEP 2]
│   └── debian/                     Ubuntu .deb packaging                           [STEP 8]
├── procd/                          OpenWrt init scripts                            [STEP 2]
├── systemd/                        Ubuntu systemd units                            [STEP 8]
├── vps/ansible/                    VPS terminator setup                            [STEP 9]
└── scripts/                        build wrappers                                  [STEP 10]
```

## Current state — Steps 1–2 of 17

Shipped (Step 1, scheduler source):
- `src/sched/ratan_sched.bpf.c` — BPF struct_ops MPTCP scheduler. Default behavior = highest weight in a pinned `ratan_path_weights` map wins; falls back to first-active subflow if weights are all zero.
- `src/sched/ratan_loader.c` — libbpf one-shot loader. Pins the link and map under `/sys/fs/bpf/ratan/`, exits.
- `src/sched/Makefile` — standalone dev build (no OpenWrt toolchain required).

Shipped (Step 2, OpenWrt packaging):
- `packaging/openwrt/ratan-sched/Makefile` — OpenWrt package recipe. Pulls source from `src/sched/`, generates `vmlinux.h` from kernel BTF at build time, compiles the BPF object with the OpenWrt clang/llvm-bpf toolchain (enabled automatically for kernel ≥6.6 via `CONFIG_BPF_TOOLCHAIN_HOST=y` in OMR's `build.sh`), compiles the loader against libbpf.
- `packaging/openwrt/ratan-sched/files/ratan-sched.init` — procd init script (START=70). Mounts bpffs, runs the loader, sets `/proc/sys/net/mptcp/scheduler=ratan`.
- `packaging/openwrt/ratan-sched/files/ratan-sched.defaults` — uci-defaults (first-boot enabler).
- `packaging/openwrt/ratan-sched/files/ratan.config` — initial `/etc/config/ratan`.
- `packaging/openwrt/ratan-full/Makefile` — metapackage. Selected automatically when `OMR_DIST=ratan OMR_PACKAGES=full`; depends on `openmptcprouter-full + ratan-sched`.
- `scripts/build-openwrt.sh` — build wrapper. Sets `CUSTOM_FEED` to the in-repo feed subdir (bypassing the URL-clone path in `build.sh`), `OMR_DIST=ratan` (so the feed is `src-link`'d as `ratan`, not colliding with the upstream `openmptcprouter` feed).

Not yet shipped: prober, telemetry, classifier, predictor, QoS, handover learner, CLI, LuCI UI, relay client, debian packaging, VPS bits.

## Building an actual OMR firmware image (Step 2)

On a Linux build host (Ubuntu 22.04+ recommended) with enough disk and time:

```sh
# 1. Clone this fork on the branch that has ratan/
git clone https://github.com/YX12399/openmptcprouter.git
cd openmptcprouter
git checkout claude/explore-openmptcprouter-z8iF1

# 2. One-shot build (RPi4, kernel 6.6, full RATAN image)
ratan/scripts/build-openwrt.sh rpi4

# Or x86_64 (for the Beelink, once you have it)
ratan/scripts/build-openwrt.sh x86_64

# Or override the kernel
OMR_KERNEL=6.12 ratan/scripts/build-openwrt.sh rpi4
```

What the wrapper does (so you can drive it manually if needed):

```sh
cd openmptcprouter
OMR_TARGET=rpi4 \
OMR_KERNEL=6.6 \
OMR_DIST=ratan \
OMR_PACKAGES=full \
CUSTOM_FEED=$(realpath ratan/packaging/openwrt) \
./build.sh
```

Two important `build.sh` interactions:
- `CUSTOM_FEED` (not `CUSTOM_FEED_URL`) is set directly, which skips the URL-clone path in `build.sh` and uses the local subdir as a feed.
- `OMR_DIST=ratan` makes `build.sh` `src-link` our feed as `ratan` and auto-emit `CONFIG_PACKAGE_ratan-full=y` (which depends on `openmptcprouter-full + ratan-sched`).

First build will take hours (downloads OpenWrt sources, builds toolchain, kernel, all packages). Output lands in `source/bin/targets/<target>/<subtarget>/`.

## Quick test on a dev box (Ubuntu 24.04, kernel ≥6.5) — without building a firmware image

```sh
sudo apt install -y clang llvm libbpf-dev linux-tools-common \
                    linux-tools-$(uname -r) bpftool

cd src/sched
make                          # produces ratan_sched.bpf.o + ratan_loader
sudo make install             # /usr/local/lib/bpf/ + /usr/local/sbin/

sudo /usr/local/sbin/ratan_loader   # loads + pins, exits 0

# verify
sudo bpftool struct_ops list                       # should list 'ratan'
sudo bpftool map dump pinned /sys/fs/bpf/ratan/path_weights  # zeros

# write some weights (subflow 0 = 70, subflow 1 = 30)
sudo bpftool map update pinned /sys/fs/bpf/ratan/path_weights \
        key 0 0 0 0 value 70 0 0 0
sudo bpftool map update pinned /sys/fs/bpf/ratan/path_weights \
        key 1 0 0 0 value 30 0 0 0

# activate (only affects MPTCP sockets opened after this)
echo ratan | sudo tee /proc/sys/net/mptcp/scheduler

# cleanup
sudo /usr/local/sbin/ratan_loader --unload
```

## License

GPL-2.0 (matches OMR / OpenWrt / Linux kernel — required for the BPF program to use GPL-exported kfuncs).
