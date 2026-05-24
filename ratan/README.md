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

## Current state — Steps 1–4b (harness + discovery slices) of 17

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

Shipped (Step 3, fast UDP prober — the sub-200ms KPI workstream):
- `src/prober/ratan_proto.h` — shared wire format (`struct ratan_probe`, `struct ratan_sample`).
- `src/prober/ratan_prober.c` — per-WAN UDP probe daemon. One pthread per WAN bound via `SO_BINDTODEVICE`, `timerfd` cadence (CLOCK_MONOTONIC) for low-jitter 50ms sends. Emits samples to `/run/ratan/samples.sock`. **Fast-failover**: on 3 consecutive losses writes `weight=0` directly to the pinned BPF map for that subflow — bypassing the predictor — with an **all-zero guard** that refuses to zero the last live subflow (stranding the user).
- `src/vps/ratan_probe_responder.c` — stateless echo server for the VPS side. IPv4 + IPv6 (`--ipv6`).
- `src/prober/Makefile` — standalone dev build.
- `packaging/openwrt/ratan-prober/` — OpenWrt package + procd init (waits for `ratan-sched`'s BPF map to exist before starting) + UCI config (`/etc/config/ratan-prober`).
- `ratan-full` metapackage now depends on `+ratan-prober`.

Shipped (Step 4a, telemetry core — the data plane behind every UI/dashboard later):
- `src/prober/ratan_proto.h` — extended with a tagged-union event envelope (`ratan_event_hdr`) and payload structs for state-transitions + weight-changes. The prober was updated to wrap its samples in the envelope (one extra 4-byte header per datagram).
- `src/telemetry/schema.sql` — SQLite schema covering samples, state transitions, weights, sessions, plus tables reserved for discovery/flows/MOS/injects (Step 4b).
- `src/telemetry/ratan_telemetryd.c` — sole reader of `/run/ratan/telemetry.sock`. SQLite WAL writer. Single-threaded poll loop. Exposes HTTP on `127.0.0.1:9180`:
  - `GET  /healthz` — liveness
  - `GET  /metrics` — Prometheus exposition (samples, losses, failovers, transitions, weight changes, HTTP/SSE counters, active session id)
  - `GET  /stream` — Server-Sent Events live feed of every event (`{"kind":"sample",...}`)
  - `GET  /sessions` — JSON list of past recordings
  - `POST /sessions {name, metadata?}` — start a session; subsequent events get linked to it
  - `POST /sessions/<id>/stop` — stop and compute a summary (per-WAN samples / loss / failover counts / RTT avg + max, transition + weight change totals)
  - `GET  /sessions/<id>` — JSON details + summary
  - `GET  /sessions/<id>/download` — full timeline as a single JSON file (`Content-Disposition: attachment` — the UI's Download button)
  - `DELETE /sessions/<id>` — remove a session and its linked rows
  - CORS-enabled for browser access from the Vercel dashboard.
- `packaging/openwrt/ratan-telemetry/` — OpenWrt package + procd init (`START=65`, before prober's `80`) + UCI config (`/etc/config/ratan-telemetry`).
- `ratan-full` metapackage now also depends on `+ratan-telemetry`.

**End-to-end verified locally** (Ubuntu 24.04): the daemon ingests prober-shaped frames, persists them, the session summary is correct, the download endpoint returns the full timeline, and the SSE stream broadcasts events to subscribers within milliseconds.

Shipped (Step 4b, harness slice — record/list/download tests from the CLI today):
- `src/prober/ratan_proto.h` — added `struct ratan_inject_event` (108 bytes) so the test harness can write synchronized markers into the telemetry stream.
- `src/telemetry/ratan_telemetryd.c` — handles `RATAN_EVENT_INJECT`: persists to the `injects` table, broadcasts via SSE so live graphs show vertical lines at every inject. Adds a generic `json_escape()` helper used everywhere user-supplied strings are interpolated into JSON output (caught a real bug during smoke testing — recipe-supplied `detail` containing JSON broke the outer JSON).
- `src/test/ratan-test` — Python CLI. Subcommands: `record`, `list`, `show ID`, `export ID`, `delete ID`, `list-recipes`, `show-recipe NAME`. Schedules inject actions on thread timers (so the main loop keeps recording), writes markers to the telemetry socket on each fire.
- `src/test/recipes/` — four shipped recipes:
  - `kpi_failover.yaml` (60s, blackhole wan0 at T+15s)
  - `video_call_continuity.yaml` (180s, three handover-shaped blips + a 5s obstruction — the headline JLR demo recipe)
  - `starlink_baseline.yaml` (600s, passive — for Step 9 handover-cadence learning)
  - `qos_isolation.yaml` (120s, force a WAN drop while bulk + call are running)
- `packaging/openwrt/ratan-test/` — OpenWrt package (depends on `+python3-light +python3-urllib +python3-yaml +ratan-telemetry`).
- `ratan-full` metapackage now also depends on `+ratan-test`.

**Smoke-tested locally end-to-end**: recipe loads → session opens → 3 `mark` injects fire on schedule → each emits a `RATAN_EVENT_INJECT` envelope → daemon persists + broadcasts on SSE within ms → `--output` produces a self-contained, JSON-valid timeline export with all injects (including quote-escaping verified against a `detail` containing `\"` and `\\`).

Shipped (Step 4b, discovery slice — the "what's connected right now" feed):
- `src/prober/ratan_proto.h` — added `struct ratan_discovery_event` (136 bytes).
- `src/telemetry/discover.{c,h}` — in-process pthread launched by the daemon. Subscribes to RTNETLINK (`RTMGRP_LINK | IPV4_IFADDR | IPV6_IFADDR | NEIGH`) for iface up/down + IP add/del + LAN-neighbor changes. Also inotify on the dnsmasq lease file (`/tmp/dhcp.leases` by default, configurable) with MAC-keyed diff to emit `dhcp_lease_add` / `dhcp_lease_del`. **Initial snapshot** on startup via `RTM_GETLINK` / `GETADDR` / `GETNEIGH` dumps + a one-shot read of the lease file — so the UI sees current state instantly without waiting for changes. Raw netlink (no libnl dep).
- `src/telemetry/ratan_telemetryd.c` — handles `RATAN_EVENT_DISCOVERY`: persists to the `discovery` table, broadcasts via SSE. Adds a `GET /discovery?limit=N` endpoint that returns the most recent N events as JSON (the UI's page-load query before subscribing to `/stream` for deltas). New CLI flag `--lease-file`; new metric `ratan_discovery_events_total`.
- `packaging/openwrt/ratan-telemetry/` — Makefile updated to compile `discover.c` with `-pthread`; UCI gains a `lease_file` option (default `/tmp/dhcp.leases`); init script passes it through.

**Verified locally**: snapshot fires on startup (5 events on a minimal container: 2 ifaces + 2 addrs + 1 neighbor with correct iface names / MTU / flags / IP family / MAC formatting), inotify catches lease changes and correctly emits MAC-keyed diffs (no false adds when an unrelated line changes), SSE broadcast in real time, `/discovery` endpoint returns the snapshot for UI consumption.

Deferred to subsequent Step 4b commits: hostapd UBUS subscriber (WiFi assoc/disassoc, OMR-only, needs libubus), MPTCP genetlink subscriber (`MPTCP_PM_CMD_*` path notifications), per-flow tracker (CTNETLINK + nDPI), MOS computer. Then: classifier (Step 5), predictor (Step 6), QoS (Step 8), handover learner (Step 9), CLI (Step 10), LuCI UI (Step 11), relay (Step 12), Vercel dashboard (Step 13).

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

## Record a test session right now (Ubuntu 24.04, no kernel changes)

The test harness exercises the full Step 4a/4b API even on a dev box.

```sh
# Once: install deps and build the daemon
sudo apt install -y build-essential libsqlite3-dev python3-yaml
cd ratan/src/telemetry && make

# Term 1: run the daemon
mkdir -p /tmp/ratan
./ratan_telemetryd \
    --db /tmp/ratan/t.db --schema ./schema.sql \
    --sock /tmp/ratan/t.sock --http-host 127.0.0.1 --http-port 9180 \
    --foreground

# Term 2: list shipped recipes, run a 3-second mark-only test, download the export
cd ratan/src/test
./ratan-test --sock /tmp/ratan/t.sock list-recipes
./ratan-test --sock /tmp/ratan/t.sock record \
    --name first_run --duration 3 \
    --inject "at:0.5s mark:hello" \
    --inject "at:1.5s mark:middle" \
    --inject "at:2.5s mark:done" \
    --output /tmp/first_run.json

# inspect what landed
./ratan-test list
./ratan-test show 1
python3 -m json.tool /tmp/first_run.json
```

The `--inject` form supports `netem_drop:wan0`, `netem_restore:wan0`,
`netem_blip:wan0/200ms/100`, `netem_delay:wan0/50ms/10ms`, `mark:label`,
and `shell:<cmd>`. The first three need root (sudo) for `tc`.

## Quick telemetry daemon test (Ubuntu 24.04, no kernel changes)

```sh
sudo apt install -y build-essential libsqlite3-dev
cd ratan/src/telemetry && make

mkdir -p /tmp/ratan_smoke
./ratan_telemetryd \
    --db /tmp/ratan_smoke/test.db \
    --schema ./schema.sql \
    --sock /tmp/ratan_smoke/telemetry.sock \
    --http-host 127.0.0.1 --http-port 19180 \
    --foreground &

# In another terminal:
curl http://127.0.0.1:19180/healthz                      # ok
curl -X POST http://127.0.0.1:19180/sessions \
     -d '{"name":"smoke"}'                               # {"id":1,...}
curl http://127.0.0.1:19180/metrics                      # Prometheus format
curl -N http://127.0.0.1:19180/stream                    # live SSE
curl -X POST http://127.0.0.1:19180/sessions/1/stop      # summary
curl -O -J http://127.0.0.1:19180/sessions/1/download    # downloads ratan-session-1.json
```

## Quick prober loopback test (any Linux box, no kernel changes)

```sh
sudo apt install -y build-essential libbpf-dev

cd ratan/src/prober && make
cd ../../ && sudo install src/prober/ratan_probe_responder /usr/local/sbin/

# Terminal 1: run the responder
sudo /usr/local/sbin/ratan_probe_responder --port 5555

# Terminal 2: tail samples (any UDP listener works; here netcat)
sudo mkdir -p /run/ratan
nc -uU /run/ratan/samples.sock -l | xxd

# Terminal 3: run the prober against localhost over the loopback iface
#   (NOTE: real usage uses real WAN ifaces. The BPF map must exist --
#   on a dev box without ratan-sched loaded, omit --weights-map or expect
#   bpf_obj_get to fail.)
sudo ratan_prober --server 127.0.0.1 --wan lo:0 --foreground
```

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
