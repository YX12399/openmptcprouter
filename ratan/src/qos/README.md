# RATAN QoS (Step 8a — framework)

The "Teams survives a WAN drop while iperf3 absorbs the hit" workstream.
This directory holds the shaping framework: `tc HTB` per WAN +
`nft` classification by `ct->mark`. The framework is **complete and
ships now**; what gets *applied* by `ct->mark` depends on a separately
verified assumption (see below).

## Files

| File | What it is |
|---|---|
| `ratan-qos-classes.json` | Category-pattern → integer mark mapping. Used by the (forthcoming) marker daemon and as documentation of which `ct->mark` values mean what. |
| `ratan-qos.nft.in` | `nft` ruleset template. Three classes (realtime / bulk / background) matched on `ct mark`. Loaded by the apply script. |
| `ratan-qos-apply.sh` | Reads UCI, builds per-WAN HTB tree, loads the nft ruleset. Idempotent. |
| `ratan-qos-stop.sh` | Reverses `apply`. Idempotent. |

## The HTB tree

For each WAN egress:

```
1: htb default 20
└── 1:1 (root, total_kbps ceiling)
    ├── 1:10 = realtime    rate=realtime_floor_kbps   ceil=total   prio 1
    ├── 1:20 = bulk        rate=remainder              ceil=total   prio 2
    └── 1:30 = background  rate=100kbit                ceil=total*bg_ceil_pct/100  prio 3
fq_codel under each class for in-class fair queuing.
```

Effects:
- **Realtime always gets its floor** (default 1.5 Mbps), even when bulk is saturating the link.
- **Bulk takes the rest** of the link when realtime isn't using its full ceiling.
- **Background is capped** by `bg_ceil_pct` so backups can't push realtime out of its budget.
- On a WAN drop, the OTHER WAN's HTB tree still honors realtime's floor — the call survives.

## The nft ruleset

```
ct mark 1 → tc priority 1:10  (realtime)
ct mark 2 → tc priority 1:20  (bulk)
ct mark 3 → tc priority 1:30  (background)
ct mark 0 → tc priority 1:20  (unclassified → bulk default)
```

Hooked into both `output` (locally-generated traffic) and `forward` (LAN→WAN routed traffic), at priority `-150` so we run BEFORE most other tables.

## The assumption gap (calibrate on first flash)

The framework matches on `ct->mark`. Whoever WRITES that mark is outside this directory:

| Source of `ct->mark` | Status |
|---|---|
| **OMR's nDPI-netfilter writes category id to `ct->mark`** | Common pattern, ASSUMED. Verify on flash: `conntrack -L \| grep mark=` while a known Teams call is active. If `mark=` reflects the call category, we're done. |
| Standalone `ratan-qos-marker` daemon | Built next (Step 8b) ONLY IF the above assumption fails — i.e. if OMR uses a CT extension that `nft` can't match natively. |

We don't ship a marker today because writing one against the wrong assumption is worse than writing none. **First-flash test calibrates the gap.**

## UCI config example

`/etc/config/ratan-qos`:

```
config qos 'main'
    option enabled '1'
    option realtime_min_kbps '1500'
    option bg_ceil_pct '20'

config wan 'starlink'
    option iface 'wan0'
    option total_kbps '200000'

config wan 'cellular'
    option iface 'wan1'
    option total_kbps '50000'
```

## Validation recipe

Already shipped at `ratan/src/test/recipes/qos_isolation.yaml`. Run:

```sh
ratan-test record --recipe qos_isolation --output run.json
```

while a Teams call + bulk iperf3 are active. Asserts MOS stays ≥3.5 throughout the WAN drop.
