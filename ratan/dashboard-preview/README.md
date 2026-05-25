# RATAN dashboard — public preview (mock data)

This directory is the **shareable static-only preview** of the RATAN
dashboard. Every number you see — WAN cards, handover phase clock,
flow table, MOS scores, sessions — is driven by an in-browser
simulator (`ratan.js` here uses the *original* mock-data version
from the design pass; the **live** dashboard at
`packaging/openwrt/luci-app-ratan/root/www/ratan/` uses a real-data
adapter that talks to the router's telemetry daemon).

## Purpose

A public URL stakeholders can visit to see *what the dashboard looks
and feels like* without needing access to a flashed router. Useful
for the JLR demo storytelling: "here's the visual; the real one runs
on the vehicle, this one runs in the cloud and behaves identically
because it shares the same JS/CSS."

## Deploy to Vercel — fastest path

### Option A — one-shot from your laptop (no GitHub integration)

```sh
npm i -g vercel               # one-time
cd ratan/dashboard-preview
vercel login                  # browser OAuth, picks your team
vercel deploy --prod          # outputs https://<project>.vercel.app
```

The directory has a `vercel.json` already; Vercel auto-detects it as
a static site and serves it. First deploy creates the project; later
`vercel deploy --prod` re-deploys to the same URL.

### Option B — Vercel + GitHub integration (auto-deploys on push)

1. https://vercel.com/new → "Import Git Repository" → pick this fork.
2. **Root Directory**: `ratan/dashboard-preview`
3. **Framework Preset**: Other (it's pure static HTML)
4. Click Deploy. Every push to the configured branch auto-deploys.

After the first deploy, Vercel gives you a permanent URL like
`https://ratan-dashboard-preview.vercel.app` (or your custom domain).

## What the preview shows

| Page | What's shown |
|---|---|
| Overview | Per-WAN cards (Starlink + Cellular), KPI strip, weight meters, handover predictor cards, 60s RTT sparklines |
| Live | Auto-scrolling event log, filter chips per event kind, KPI strip, rate visualization |
| Sessions | Sortable list of test recordings, create / download / details modal |
| Handover | Phase clock countdown, last-20 handover timeline, per-WAN predictor cards with armed banner |
| Flows | Active flows table with nDPI category, traffic direction bars, filter by category/WAN |
| Devices | LAN client list with last-seen, vendor inference, RSSI for WiFi |

The mock simulator drives realistic-looking evolution: Starlink does
a handover every ~15 seconds (RTT spike + brief loss); the state
machine flips between HEALTHY / TRANSIENT / DEGRADING based on
synthetic loss; the handover timeline accumulates green dots (hits)
and occasional red ones (misses).

## The real dashboard

When the router is flashed with `luci-app-ratan`, the dashboard at
`http://<router-ip>/ratan/` looks **identical** to this preview but
every number is real telemetry from the prober + classifier +
handover predictor. The HTML / CSS / JS structure is the same; only
the data source differs.
