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

## Current state — Step 1 of 10

Shipped:
- `src/sched/ratan_sched.bpf.c` — BPF struct_ops MPTCP scheduler. Default behavior = highest weight in a pinned `ratan_path_weights` map wins; falls back to first-active subflow if weights are all zero.
- `src/sched/ratan_loader.c` — libbpf one-shot loader. Pins the link and map under `/sys/fs/bpf/ratan/`, exits.
- `src/sched/Makefile` — standalone dev build (no OpenWrt toolchain required).

Not yet shipped: OpenWrt packaging, predictor, prober, QoS, handover learner, debian packaging, VPS bits.

## Quick test on a dev box (Ubuntu 24.04, kernel ≥6.5)

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
