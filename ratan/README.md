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

## Current state — Steps 1–5, 8a/8c, 9, 11 of 17

> **RATAN coexists with LuCI.** The standard LuCI admin (wizards, WAN/LAN
> config, firewall, system status, syslog, backup, OpenMPTCProuter status,
> realtime bandwidth & connections) ships unchanged and is the canonical
> place for system administration. RATAN adds a *new* dashboard
> specifically for our MPTCP-bonding telemetry and Starlink-handover
> intelligence. The RATAN nav has a "LuCI ▾" dropdown in the top-right
> that deep-links to the common LuCI destinations so users always have
> one click back to the full admin surface.



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

Shipped (Step 4b, flowtrack slice — per-flow visibility):
- `src/prober/ratan_proto.h` — added `struct ratan_flow_event` (168 bytes).
- `src/telemetry/flowtrack.{c,h}` — in-process pthread subscribing to CTNETLINK NEW/DESTROY groups. Parses nested netlink attribute trees (`CTA_TUPLE_ORIG > CTA_TUPLE_IP > CTA_IP_V4_SRC` etc., with full bounds checking on every nested attr length). Extracts 5-tuple + conntrack mark + counters. **nDPI category mapping** loaded from `/etc/ratan/ndpi-categories.conf` (one `<int>  <name>` per line; `#` comments) — the mark→category-name lookup is decoupled from kernel-side ndpi-netfilter encoding so it works whatever convention OMR's nDPI integration uses. Bounded 4096-entry flow table with LRU eviction. **Containers can't run flowtrack** (needs `CAP_NET_ADMIN` + `nf_conntrack` loaded); on failure the daemon logs a warning and continues without it.
- `src/telemetry/ratan_telemetryd.c` — handles `RATAN_EVENT_FLOW`: persists to `flows` table, broadcasts via SSE. `GET /flows?limit=N` endpoint. New metric `ratan_flow_events_total`. New CLI flags `--ndpi-cats`, `--no-flowtrack`.

Shipped (Step 4b, MOS slice — the "did the call survive?" quantification):
- `src/prober/ratan_proto.h` — added `struct ratan_mos_event` (144 bytes).
- `src/telemetry/mos.{c,h}` — in-process pthread, 1Hz tick. Queries SQLite for active VideoCall/* flows (configurable via `--mos-category`), aggregates last-1s prober samples per WAN, picks the WAN with lowest avg RTT as the call's likely path (documented heuristic), applies the E-model R-factor → MOS formula (`Id=0.024·lat + 0.11·max(lat-177.3,0)`, `Ie=loss%·30`, `R=93.2-Id-Ie`, MOS clamped to [1.0, 4.5]). Emits `RATAN_EVENT_MOS` envelopes back through the telemetry socket — symmetric with every other event. Opens SQLite **read-only** since the daemon owns writes.
- `src/telemetry/ratan_telemetryd.c` — handles `RATAN_EVENT_MOS`: persists to `mos` table with rtt_ms/loss_pct/jitter_ms (converted from prober's microsecond/ppm units), broadcasts via SSE with the full R-factor + per-component breakdown. `GET /mos?limit=N` endpoint. New metric `ratan_mos_events_total`. New CLI flags `--no-mos`, `--mos-category`.

**Verified end-to-end locally**: fed 120 prober samples on two WANs (25ms vs 60ms) + one `VideoCall/Teams` flow_start; MOS thread correctly selected the 25ms WAN, computed MOS = **4.397** (matches offline reference calculation), persisted to `mos` table, returned via `/mos` endpoint. Boundary tests confirmed MOS clamps at 4.5/1.0 across the realistic input range.

Shipped (Step 5, the smart classifier — the **heart of "smart"**):
- `src/classifier/ewma.py` — tiny EWMA filter; standalone-unit-testable.
- `src/classifier/fsm.py` — pure-Python per-WAN finite state machine. **No I/O.** Inputs: samples. Output: optional `Transition`. States: HEALTHY → TRANSIENT → DEGRADING → DOWN with the two-channel signal decomposition (slow baseline EWMA vs fast transient EWMA) that makes Starlink's 15s handovers look like blips, not degradation.
- `src/classifier/tests/{test_fsm,test_ewma}.py` — **14 unit tests, all passing.** The headline regression test (`test_repeated_blips_never_decay_weight`) feeds ten back-to-back 250ms Starlink-handover blips and asserts the weight never drops from baseline. Other tests cover: short blip → TRANSIENT (not DEGRADING), short blip recovery to HEALTHY, sustained intermittent loss → DEGRADING with smooth weight decay, 1.5s blackhole → DOWN, DOWN recovery, preempter forces TRANSIENT, steady-state silence.
- `src/classifier/ratan-classifier` — thin orchestrator (~270 lines). Long-lived SSE client to `http://127.0.0.1:9180/stream`, per-WAN `WanFsm` instances, emits `RATAN_EVENT_STATE` + `RATAN_EVENT_WEIGHT` envelopes back to the telemetry socket (symmetric with everything else), writes new weights to the pinned BPF map via `bpftool`. Auto-reconnects SSE on disconnect with exponential backoff. Has `--no-bpf` for dev boxes without the map.
- `packaging/openwrt/ratan-classifier/` — OpenWrt package (`+python3-light +python3-urllib +bpftool +ratan-telemetry +ratan-sched`) + procd init (waits for telemetry HTTP `/healthz` before launching) + default `/etc/ratan/classifier.json` config.
- `ratan-full` metapackage now also depends on `+ratan-classifier`.

**Verified end-to-end locally**: telemetry daemon + classifier started; fed 1.25s of baseline OK samples on two WANs (25ms / 60ms), then 5 consecutive losses on WAN 0 (the Starlink-handover signature); classifier transitioned `HEALTHY → TRANSIENT (consec_loss_100ms)` then `TRANSIENT → HEALTHY (blip_ended_200ms)` 350ms later; **weight stayed at 70 the whole time**; transitions correctly persisted in the SQLite `state_transitions` table.

Shipped (Step 9, handover predictor — variable 15-60s cadence):
- `src/classifier/handover.py` — `HandoverPredictor` class implementing the FSM's `preempter` interface. Per-WAN sliding-window inter-event tracking (last 20 handovers), median-based prediction gated by coefficient of variation (tight/loose/chaotic confidence tiers), automatic miss-streak unlock after 3 consecutive failed predictions, configurable pre-emption window `[predicted - 30ms, predicted + 500ms]`. Pure Python with injected clock for deterministic tests.
- `src/classifier/tests/test_handover.py` — **10 new unit tests, all passing**. Covers cold start, tight lock at consistent 15s, loose/chaotic detection, pre-emption window timing, miss-streak unlock, signature-envelope exclusion (microblips and long-degradation streaks correctly excluded), per-WAN isolation.
- `src/classifier/ratan-classifier` — orchestrator wires the predictor as `preempter` on every `WanFsm`, feeds samples to it BEFORE the FSM (so `is_about_to_handover()` reflects the current sample). Writes `/run/ratan/handover-status.json` (1s throttle + force-write on every state transition) so UI/curl/Grafana can see what the predictor thinks live.
- Default `classifier.json` now includes the `handover` block.

**Verified end-to-end locally**: 8 handover blips at consistent 15.3s spacing produced `events_total=7 intervals_tracked=6 confidence=tight cov=0.0 median_interval_s=15.3`. Classifier emitted 8 correct `HEALTHY → TRANSIENT → HEALTHY` cycles. The status file updated to reflect the lock.

**Step 5 + 9 hardware-test notes (carry forward):**
- `down_sustain_ms=1000` is a safe upper bound given Starlink handovers max ~300ms. Field data may show distinct bimodal distribution; if so, can be lowered to e.g. 500ms for faster DOWN entry. Tune only with telemetry from the real dish.
- Starlink LEO satellite handovers happen every **15-60 seconds** (variable per session, not fixed 15s). The FSM is cadence-agnostic; the prediction layer (Step 9) tracks inter-handover intervals over a sliding window and pre-empts only when the coefficient of variation indicates consistent cadence.
- Compound case (minor obstruction + handover) verified by unit test `test_minor_obstruction_plus_handovers_stays_usable` and by the new `starlink_obstruction_plus_handover.yaml` recipe. FSM correctly stays HEALTHY at 2% baseline (the call-routing decision belongs to QoS layer Step 8, not the classifier).

Shipped (Step 8a, QoS framework — the "Teams survives a WAN drop" moat):
- `src/qos/ratan-qos-classes.json` — category-pattern → integer mark map.
  Three classes: realtime (VideoCall/*, VoIP/*, Audio/*, RealTimeChat/*),
  bulk (Web/*, Streaming/*, DNS, HTTP, TLS, QUIC), background (OS/*,
  FileTransfer/*, BitTorrent/*, Backup/*).
- `src/qos/ratan-qos.nft.in` — nftables ruleset matching `ct mark` and
  setting tc priority (1:10 realtime, 1:20 bulk, 1:30 background, 1:20
  default for unmarked). Hooked into both `output` and `forward`
  at priority -150.
- `src/qos/ratan-qos-apply.sh` — reads `/etc/config/ratan-qos`, builds
  per-WAN HTB tree (1.5 Mbps realtime floor, configurable; remainder
  bulk; background capped at 20% of total), loads the nft ruleset.
  Idempotent.
- `src/qos/ratan-qos-stop.sh` — reverses apply.
- `src/qos/README.md` — design notes including the **calibration gap**
  (real flash needed to verify OMR's nDPI integration actually
  populates ct->mark; if not, Step 8b adds a marker daemon).
- `packaging/openwrt/ratan-qos/` — OpenWrt package + procd init
  (START=92, after network) + UCI defaults. Deps:
  `+kmod-sched +kmod-sched-core +tc +nftables +kmod-nft-core
  +kmod-nf-conntrack-netlink`.
- `ratan-full` metapackage now also depends on `+ratan-qos`.

**Validated in container**: shell scripts pass `sh -n`; nft ruleset
passes `nft -c -f`; JSON valid; HTB algebra works out at the default
config (1.5 Mbps floor + 200 Mbps Starlink + 50 Mbps cellular).

**Pending first-flash calibration**:
- Verify OMR's nDPI-netfilter writes the category id to `ct->mark`
  (the assumption the framework rests on). One-command check:
  `conntrack -L | grep mark=` while a known Teams call is active.
- If the assumption holds → framework just works.
- If not → Step 8b adds `ratan-qos-marker` (userspace daemon that
  subscribes to telemetry flow events and writes `ct->mark` itself,
  using libnetfilter_conntrack or `conntrack -U` shell-out).

**Validation recipe already shipped** — `ratan-test record --recipe qos_isolation`
asserts MOS stays ≥3.5 throughout an induced WAN drop while bulk
iperf3 + Teams call are active.

Shipped (Step 8c, QoS dashboard page):
- New `/qos-status` endpoint in the telemetry daemon — shells out to
  `tc -j -s class show dev <iface>` per configured WAN (the same
  iface set we already pass via `--wan` for `/wan-stats`), wraps each
  iface's tc JSON inside `{wan_id, iface, label, classes}`. Iface
  names validated against a strict allowlist before shell-out.
- New `qos.html` dashboard page (under **Network → RATAN → QoS**)
  with per-WAN cards: each class (realtime / bulk / background)
  gets a horizontal bar showing current usage vs the configured
  ceiling, a notch marking the floor, plus sent-bytes / drops /
  overlimits counters. Drops are colored red when > 0.
- Per-class current rate computed in-browser as delta of
  `stats.bytes` between 2s polls (no extra backend math needed).
- `qos` added to PAGES in `ratan.js` and to the LuCI menu.

Verified in container: `/qos-status` returns valid JSON envelope
with `classes: []` (no tc in container); on the real router with
`/etc/init.d/ratan-qos start` first, the classes populate with
1:10 / 1:20 / 1:30 + their tc stats.

Shipped (Step 11, LuCI dashboard — design integration):

The first cut shipped my hand-rolled HTML/CSS/JS. This commit **replaces it
wholesale with the polished design** delivered by the design-prompt pass
(dark theme, Geist Mono numbers, color-coded state tiers, inline SVG
sparklines, six page templates: Overview / Live / Sessions / Handover /
Flows / Devices). The design's mock-data simulator was rewritten as a
real-data adapter so the same pages work against live backends with no
template changes.

- `packaging/openwrt/luci-app-ratan/root/www/ratan/`:
  - `ratan.css` (~20 KB) — full design system: layered surfaces, semantic
    state colors, monospace numerics, status pills, sparkline styling,
    LuCI dropdown.
  - `ratan.js` (~21 KB) — **real-data adapter**, same `window.RATAN`
    public API the design's mock used (`bus`, `STATE`, `fmt`,
    `highlightJSON`, `sparkline`, `mountNav`) plus a new `api`
    namespace (`startSession`, `stopSession`, `deleteSession`,
    `downloadSessionUrl`, `getSession`). Drives STATE from polling
    seven backend endpoints at staggered cadences (`/metrics` 1s,
    `/wans` 1s, `/handover-status.json` 1s, `/events?since=` 1s,
    `/flows` 3s, `/discovery` 5s, `/sessions` 5s). SSE remains
    deferred (shell CGI buffers); polling drives the bus with the
    same event shapes the design expects.
  - 6 HTML pages from the design (overview/live/sessions/handover/flows/devices)
    + `index.html` redirect.
  - `cgi-bin/ratan-api` — unchanged proxy from the earlier commit.
- Two new backend endpoints in the telemetry daemon:
  - **`GET /wans`** — per-WAN snapshot derived from the most recent
    samples (60s avg RTT / jitter / loss %), last state transition,
    last weight write. Single SQL roundtrip per WAN.
  - **`GET /events?since=<ts_ns>&kind=<k>&limit=<N>`** — recent events
    of mixed kinds (sample/state/weight/flow/discovery/mos) for the
    Live page's catch-up polling. Sorted by ts_ns. Used until we wire
    real SSE.
- LuCI menu entries added for Flows + Devices (under Network → RATAN).
- **`LuCI ▾` dropdown in the RATAN top nav** with deep-links to: LuCI home,
  OMR status, Setup wizard, WAN/LAN config, Firewall, Realtime bandwidth,
  Realtime connections, System log, Backup/flash. Makes it explicit that
  RATAN is a dashboard, not a replacement.
- The earlier "Step 11b — LuCI-parity pages" plan is **cancelled**.
  The whole LuCI app set ships unchanged; users go there for anything
  outside RATAN's specific telemetry scope.

**End-to-end verified locally**: 6 HTML pages + CSS + JS serve 200; `/wans`
returns coherent per-WAN snapshot; `/events?since=0` returns mixed-kind
recent timeline; `/handover-status.json` reaches the static path; session
create/stop/delete round-trip through `api.*`.

**Known display gaps** — *two of the three closed in the Step 11 polish round*:
- ~~**Per-WAN throughput chart** on the Overview page renders empty~~ ✓
  **Closed by `/wan-stats` endpoint** that reads
  `/sys/class/net/<iface>/statistics/{rx,tx}_bytes` and computes bps
  from delta-bytes / delta-time vs the previous snapshot. The
  procd init mirrors prober's UCI to pass `--wan idx:iface:label`
  flags to the daemon. Adapter polls `/wan-stats` at 1s and fills
  `STATE.wans[i].{bps, rx_bps, tx_bps, bpsHist}`.
- ~~**Per-flow byte counters** are 0~~ ✓
  **Closed by schema migration**: `flows` table gained `bytes_orig`
  and `bytes_reply` columns (idempotent `ALTER TABLE` for old DBs);
  `on_flow()` now persists them; `/flows` returns the per-tuple
  `MAX(bytes_*)` so the UI shows the freshest non-zero counters.
- **Real SSE** still deferred — getting true streaming through shell
  CGI requires modifying `/etc/config/uhttpd` (a url-rewrite to
  `127.0.0.1:9180`), which would touch upstream OMR config. 1s
  polling against `/events?since=` is invisible to users and keeps
  us at zero OMR modifications. We'll add an opt-in `uhttpd` config
  fragment alongside the package later if anyone needs sub-100ms
  latency on the Live page.

### Step 11 complete. What we can do today:

```sh
# build everything in src/telemetry/
sudo apt install -y build-essential libsqlite3-dev python3-yaml
cd ratan/src/telemetry && make

# run the full pipeline locally (no kernel changes needed)
mkdir -p /tmp/ratan
./ratan_telemetryd --db /tmp/ratan/t.db --schema ./schema.sql \
    --sock /tmp/ratan/t.sock --http-port 9180 \
    --no-discover --no-flowtrack \
    --foreground &

# run the classifier against it (dev mode: skip bpftool writes)
cd ../classifier
python3 -m unittest classifier.tests.test_fsm classifier.tests.test_ewma -v
./ratan-classifier --config /etc/ratan/classifier.json --no-bpf --foreground &

# record a test session with synchronized markers (Step 4b/harness)
ratan/src/test/ratan-test --port 9180 --sock /tmp/ratan/t.sock list-recipes
ratan/src/test/ratan-test --port 9180 --sock /tmp/ratan/t.sock record \
    --name "first_test" --duration 3 --inject "at:1s mark:hello"

# UI-side endpoints ready for any client (LuCI, Vercel, curl):
curl http://127.0.0.1:9180/sessions       # list
curl http://127.0.0.1:9180/discovery      # WANs/IPs/LAN neighbors
curl http://127.0.0.1:9180/flows          # active TCP/UDP flows w/ nDPI category
curl http://127.0.0.1:9180/mos            # per-second MOS samples
curl -N http://127.0.0.1:9180/stream      # live SSE
curl http://127.0.0.1:9180/metrics        # Prometheus
curl -O -J http://127.0.0.1:9180/sessions/1/download   # downloadable JSON
```

Deferred (later commits, smaller priority): hostapd UBUS subscriber for WiFi assoc events (libubus, OMR-only), MPTCP genetlink subscriber for subflow lifecycle events. Both useful but not blocking for the demo.

Next: classifier (Step 5), predictor (Step 6), QoS (Step 8), handover learner (Step 9), CLI (Step 10), LuCI UI (Step 11), relay (Step 12), Vercel dashboard (Step 13).

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
