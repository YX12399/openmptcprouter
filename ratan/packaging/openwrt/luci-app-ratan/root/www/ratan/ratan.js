/* ============================================================================
   RATAN dashboard — shared client (real-backend adapter)
   ----------------------------------------------------------------------------
   Public API (unchanged from the design's mock version):
     window.RATAN = { bus, STATE, fmt, highlightJSON, sparkline, mountNav, api }
   Pages read STATE.* and subscribe via bus.on('sample'|'state'|'weight'|
   'flow'|'mos'|'discovery'|'inject'|'event'|'tick', fn).
   Mutations call api.startSession()/stopSession()/deleteSession() instead of
   pushing into STATE.sessions directly.

   Data sources (all via /cgi-bin/ratan-api/<path>):
     /healthz                  -- connection indicator
     /metrics                  -- Prometheus counters -> STATE.counters
     /wans                     -- per-WAN snapshot -> STATE.wans
     /sessions                 -- list -> STATE.sessions
     /flows?limit=50           -- recent flow events -> STATE.flows
     /discovery?limit=200      -- discovery events -> STATE.devices (from
                                  dhcp_lease + neighbor entries)
     /mos?limit=50             -- recent MOS samples (emitted as 'mos' events)
     /events?since=<ts>&limit=N-- catch-up events between polls (emitted as
                                  sample/state/weight/flow/discovery/mos)
     /ratan-data/handover-status.json  -- per-WAN handover predictor state

   Cadences:
     metrics      every 1s
     wans         every 1s
     handover     every 1s
     sessions     every 5s
     flows        every 3s
     discovery    every 5s
     events catch-up every 1s (drives bus.emit for the Live page)

   SSE not yet proxied (shell CGI buffers). Future commit replaces the
   1s metrics+events poll with a real EventSource against /stream via
   uhttpd url-rewrite or socat. Public API doesn't change when that lands.
   ============================================================================ */

(() => {
  'use strict';

  const API = '/cgi-bin/ratan-api';
  const HANDOVER_FILE = '/ratan-data/handover-status.json';

  // ---------- tiny pub/sub (unchanged) ------------------------------------

  const bus = (() => {
    const subs = new Map();
    return {
      on(kind, fn) {
        if (!subs.has(kind)) subs.set(kind, new Set());
        subs.get(kind).add(fn);
        return () => subs.get(kind).delete(fn);
      },
      emit(kind, payload) {
        (subs.get(kind) || []).forEach(fn => { try { fn(payload); } catch (e) { console.error(e); } });
        (subs.get('*') || []).forEach(fn => { try { fn(kind, payload); } catch (e) { console.error(e); } });
      },
    };
  })();

  // ---------- STATE shape (same as design's mock; populated from API) -----

  const STATE = {
    counters: {
      samples_total: 0,
      samples_loss: 0,
      samples_failover: 0,
      state_transitions: 0,
      weight_changes: 0,
      flow_events: 0,
      mos_events: 0,
      discovery_events: 0,
      http_requests: 0,
      sse_clients: 0,
      sessions_started: 0,
      sessions_completed: 0,
      active_session_id: 0,
    },
    wans: [],          // [{wan, iface, label, state, weight, rtt_us, jitter_us, loss, bps, rttHist, bpsHist, handover, handoverTimeline}]
    flows: [],
    devices: [],
    sessions: [],
    events: [],        // ring of last 600 events for live page
    connected: false,  // /healthz reachable
  };

  // ---------- HTTP helpers ------------------------------------------------

  async function getJSON(path) {
    const r = await fetch(API + path, { cache: 'no-store' });
    if (!r.ok) throw new Error(`HTTP ${r.status} ${path}`);
    return r.json();
  }
  async function postJSON(path, body) {
    const r = await fetch(API + path, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(body || {}),
    });
    if (!r.ok) throw new Error(`HTTP ${r.status} ${path}`);
    return r.json();
  }
  async function delJSON(path) {
    const r = await fetch(API + path, { method: 'DELETE' });
    if (!r.ok) throw new Error(`HTTP ${r.status} ${path}`);
  }
  async function getStaticJSON(path) {
    try {
      const r = await fetch(path, { cache: 'no-store' });
      if (!r.ok) return null;
      return await r.json();
    } catch { return null; }
  }
  async function getText(path) {
    const r = await fetch(API + path, { cache: 'no-store' });
    if (!r.ok) throw new Error(`HTTP ${r.status} ${path}`);
    return r.text();
  }

  // ---------- formatters (unchanged from design) --------------------------

  const fmt = {
    int:  (n) => (n||0).toLocaleString('en-US'),
    bytes: (b) => {
      if (b < 1024) return b + ' B';
      if (b < 1024*1024) return (b/1024).toFixed(1) + ' KB';
      if (b < 1024*1024*1024) return (b/1024/1024).toFixed(1) + ' MB';
      return (b/1024/1024/1024).toFixed(2) + ' GB';
    },
    bps: (b) => {
      const bits = (b||0) * 8;
      if (bits < 1000) return bits.toFixed(0) + ' bps';
      if (bits < 1e6) return (bits/1000).toFixed(1) + ' Kbps';
      if (bits < 1e9) return (bits/1e6).toFixed(1) + ' Mbps';
      return (bits/1e9).toFixed(2) + ' Gbps';
    },
    us: (us) => (us/1000).toFixed(1),
    ms: (ms) => ms.toFixed(1),
    pct: (p) => (p||0).toFixed(2) + '%',
    ago: (ts) => {
      const s = (Date.now() - ts) / 1000;
      if (s < 1) return 'now';
      if (s < 60) return Math.floor(s) + 's ago';
      if (s < 3600) return Math.floor(s/60) + 'm ago';
      if (s < 86400) return Math.floor(s/3600) + 'h ago';
      return Math.floor(s/86400) + 'd ago';
    },
    dur: (ms) => {
      const s = Math.floor(Math.abs(ms)/1000);
      const h = Math.floor(s/3600), m = Math.floor((s%3600)/60), ss = s%60;
      const pad = (n) => String(n).padStart(2,'0');
      return (h ? pad(h)+':' : '') + pad(m) + ':' + pad(ss);
    },
    ts: (d=new Date()) => {
      const pad = (n,w=2) => String(n).padStart(w,'0');
      return pad(d.getHours()) + ':' + pad(d.getMinutes()) + ':' + pad(d.getSeconds()) + '.' + pad(d.getMilliseconds(),3);
    },
    mac: (m) => m,
  };

  // ---------- JSON syntax highlighter (unchanged) -------------------------

  function highlightJSON(obj) {
    const s = typeof obj === 'string' ? obj : JSON.stringify(obj, null, 2);
    return s.replace(/(&|<|>)/g, c => ({'&':'&amp;','<':'&lt;','>':'&gt;'}[c]))
      .replace(/("(?:[^"\\]|\\.)*")(\s*:)?/g, (m, str, colon) =>
        colon ? `<span class="json-key">${str}</span><span class="json-punct">${colon}</span>`
              : `<span class="json-str">${str}</span>`)
      .replace(/\b(true|false)\b/g, '<span class="json-bool">$1</span>')
      .replace(/\bnull\b/g, '<span class="json-null">null</span>')
      .replace(/(-?\d+\.?\d*(?:e[+-]?\d+)?)/gi, '<span class="json-num">$1</span>')
      .replace(/([\[\]\{\},])/g, '<span class="json-punct">$1</span>');
  }

  // ---------- sparkline SVG (unchanged) -----------------------------------

  function sparkline(values, opts = {}) {
    const w = opts.w || 200, h = opts.h || 36;
    if (!values || values.length < 2) return `<svg class="spark" viewBox="0 0 ${w} ${h}"></svg>`;
    const min = Math.min(...values), max = Math.max(...values);
    const range = (max - min) || 1;
    const pad = 2;
    const step = (w - pad*2) / (values.length - 1);
    const pts = values.map((v, i) => {
      const x = pad + i * step;
      const y = h - pad - ((v - min) / range) * (h - pad*2);
      return `${x.toFixed(1)},${y.toFixed(1)}`;
    });
    const linePath = 'M' + pts.join(' L ');
    const areaPath = linePath + ` L ${w-pad},${h-pad} L ${pad},${h-pad} Z`;
    return `<svg class="spark" viewBox="0 0 ${w} ${h}" preserveAspectRatio="none">
      <path class="spark-area" d="${areaPath}" />
      <path class="spark-line" d="${linePath}" />
    </svg>`;
  }

  // ---------- nav (unchanged from design) ---------------------------------

  const PAGES = [
    { id: 'overview',  href: 'overview.html',  label: 'Overview' },
    { id: 'live',      href: 'live.html',      label: 'Live' },
    { id: 'sessions',  href: 'sessions.html',  label: 'Sessions' },
    { id: 'handover',  href: 'handover.html',  label: 'Handover' },
    { id: 'flows',     href: 'flows.html',     label: 'Flows' },
    { id: 'devices',   href: 'devices.html',   label: 'Devices' },
  ];

  // LuCI quick-links. RATAN COEXISTS with LuCI -- we don't replace it.
  // Anything not specifically about RATAN telemetry (WAN config, firewall,
  // wizard, realtime graphs, syslog, system) stays in LuCI; we just link
  // there from one place at the top right.
  const LUCI_LINKS = [
    { href: '/cgi-bin/luci',                                              label: 'LuCI home' },
    { href: '/cgi-bin/luci/admin/system/openmptcprouter',                 label: 'OMR status' },
    { href: '/cgi-bin/luci/admin/system/openmptcprouter/wizard',          label: 'Setup wizard' },
    { href: '/cgi-bin/luci/admin/network/network',                        label: 'WAN/LAN config' },
    { href: '/cgi-bin/luci/admin/network/firewall',                       label: 'Firewall' },
    { href: '/cgi-bin/luci/admin/status/realtime/bandwidth',              label: 'Realtime bandwidth' },
    { href: '/cgi-bin/luci/admin/status/realtime/connections',            label: 'Realtime conns' },
    { href: '/cgi-bin/luci/admin/status/syslog',                          label: 'System log' },
    { href: '/cgi-bin/luci/admin/system/flash',                           label: 'Backup / flash' },
  ];

  function mountNav(activeId) {
    const slot = document.getElementById('nav-slot');
    if (!slot) return;
    slot.innerHTML = `
      <nav class="nav">
        <a class="nav-brand" href="overview.html">
          <span class="nav-brand-mark"></span>
          <span>RATAN</span>
        </a>
        <div class="nav-links">
          ${PAGES.map(p => `<a href="${p.href}" data-id="${p.id}" class="${p.id === activeId ? 'active' : ''}">${p.label}</a>`).join('')}
        </div>
        <div class="nav-right">
          <div class="luci-menu" id="luci-menu">
            <button type="button" class="luci-toggle" id="luci-toggle" aria-haspopup="true" aria-expanded="false">
              LuCI ▾
            </button>
            <div class="luci-dropdown" id="luci-dropdown" role="menu" hidden>
              ${LUCI_LINKS.map(l => `<a href="${l.href}" target="_blank" rel="noopener" role="menuitem">${l.label}</a>`).join('')}
            </div>
          </div>
          <span style="color:var(--fg-3);">·</span>
          <span class="dot" id="conn-dot"></span>
          <span id="nav-uptime">connecting…</span>
          <span style="color:var(--fg-3);">·</span>
          <span id="nav-build">router</span>
        </div>
      </nav>
    `;
    // dropdown toggle
    const tg = document.getElementById('luci-toggle');
    const dd = document.getElementById('luci-dropdown');
    if (tg && dd) {
      tg.addEventListener('click', (e) => {
        e.stopPropagation();
        const open = !dd.hidden;
        dd.hidden = open;
        tg.setAttribute('aria-expanded', String(!open));
      });
      document.addEventListener('click', () => { dd.hidden = true; tg.setAttribute('aria-expanded', 'false'); });
    }
  }

  // ---------- Prometheus parser -------------------------------------------

  function parsePromText(text) {
    const out = {};
    for (const line of text.split('\n')) {
      if (line.startsWith('#') || !line.trim()) continue;
      const sp = line.lastIndexOf(' ');
      if (sp < 0) continue;
      const name = line.slice(0, sp).trim().replace(/\{.*\}/, '');
      const v = parseFloat(line.slice(sp + 1));
      if (!isNaN(v)) out[name] = v;
    }
    return out;
  }

  // ---------- per-WAN state mapping ---------------------------------------

  // The /wans endpoint returns wan_id, state, weight, rtt_us, jitter_us,
  // loss_pct, samples_60s, last_*_ts_ns, weight_source. Pages expect:
  // wan, iface, label, state, weight, rtt_us, jitter_us, loss, bps,
  // rttHist[], bpsHist[], handover{...}, handoverTimeline[].
  //
  // We synthesize iface/label from wan id (Step 11b will let UCI override
  // these). bps is left null/0 until we add per-WAN traffic stats from
  // /sys/class/net/<iface>/statistics (Step 11b).

  const DEFAULT_LABELS = {
    0: { iface: 'wan0', label: 'WAN 0' },
    1: { iface: 'wan1', label: 'WAN 1' },
    2: { iface: 'wan2', label: 'WAN 2' },
    3: { iface: 'wan3', label: 'WAN 3' },
  };

  function ensureWan(wanId) {
    let w = STATE.wans.find(x => x.wan === wanId);
    if (!w) {
      const meta = DEFAULT_LABELS[wanId] || { iface: 'wan' + wanId, label: 'WAN ' + wanId };
      w = {
        wan: wanId,
        iface: meta.iface,
        label: meta.label,
        state: 'HEALTHY',
        weight: 0,
        rtt_us: 0,
        jitter_us: 0,
        loss: 0,
        bps: 0,
        rttHist: [],
        bpsHist: [],
        handover: {
          events_total: 0, intervals_tracked: 0, confidence: 'cold',
          cov: 0, median_interval_s: 0, next_predicted_in_s: 0,
          miss_streak: 0, currently_armed: false,
        },
        handoverTimeline: [],
      };
      STATE.wans.push(w);
      STATE.wans.sort((a, b) => a.wan - b.wan);
    }
    return w;
  }

  // ---------- backend pollers ---------------------------------------------

  let lastEventTsNs = 0;     // for /events?since= catch-up
  let lastRttPushTs = {};    // per-wan throttle for the rttHist push
  let prevCounters = null;

  async function pollHealth() {
    try {
      const r = await fetch(API + '/healthz', { cache: 'no-store' });
      STATE.connected = r.ok;
    } catch { STATE.connected = false; }
    const dot = document.getElementById('conn-dot');
    const lbl = document.getElementById('nav-uptime');
    if (dot) dot.style.background = STATE.connected ? 'var(--healthy)' : 'var(--down)';
    if (lbl) lbl.textContent = STATE.connected ? 'uplink ok' : 'daemon unreachable';
  }

  async function pollMetrics() {
    let text;
    try { text = await getText('/metrics'); } catch { return; }
    const m = parsePromText(text);
    const c = STATE.counters;
    c.samples_total      = m.ratan_samples_total       || 0;
    c.samples_loss       = m.ratan_samples_loss        || 0;
    c.samples_failover   = m.ratan_samples_failover    || 0;
    c.state_transitions  = m.ratan_state_transitions_total || 0;
    c.weight_changes     = m.ratan_weight_changes_total    || 0;
    c.flow_events        = m.ratan_flow_events_total       || 0;
    c.mos_events         = m.ratan_mos_events_total        || 0;
    c.discovery_events   = m.ratan_discovery_events_total  || 0;
    c.http_requests      = m.ratan_http_requests_total     || 0;
    c.sse_clients        = m.ratan_sse_clients             || 0;
    c.sessions_started   = m.ratan_sessions_started_total  || 0;
    c.sessions_completed = m.ratan_sessions_completed_total|| 0;
    c.active_session_id  = m.ratan_active_session_id       || 0;
    prevCounters = prevCounters || { ...c };
  }

  async function pollWans() {
    let d;
    try { d = await getJSON('/wans'); } catch { return; }
    if (!d || !d.wans) return;
    const now = Date.now();
    for (const incoming of d.wans) {
      const w = ensureWan(incoming.wan_id);
      // state from last transition (default HEALTHY if none yet)
      if (incoming.state) w.state = incoming.state;
      if (incoming.weight !== undefined && incoming.weight !== -1) w.weight = incoming.weight;
      w.rtt_us    = Math.round(incoming.rtt_us    || w.rtt_us);
      w.jitter_us = Math.round(incoming.jitter_us || w.jitter_us);
      w.loss      = incoming.loss_pct || 0;
      // rttHist: push one point per second (throttled per WAN)
      if (!lastRttPushTs[w.wan] || now - lastRttPushTs[w.wan] >= 950) {
        if (w.rtt_us > 0) w.rttHist.push(w.rtt_us);
        if (w.rttHist.length > 60) w.rttHist.shift();
        lastRttPushTs[w.wan] = now;
      }
    }
  }

  async function pollWanStats() {
    let d;
    try { d = await getJSON('/wan-stats'); } catch { return; }
    if (!d || !d.wans) return;
    const now = Date.now();
    for (const incoming of d.wans) {
      const w = ensureWan(incoming.wan_id);
      // Override iface/label from authoritative stats source (set via
      // --wan flag on the daemon, typically mirrored from prober UCI).
      if (incoming.iface) w.iface = incoming.iface;
      if (incoming.label) w.label = incoming.label;
      // bps = rx + tx (aggregate per WAN -- matches the design's bps field)
      w.bps = (incoming.rx_bps || 0) + (incoming.tx_bps || 0);
      w.rx_bps = incoming.rx_bps || 0;
      w.tx_bps = incoming.tx_bps || 0;
      if (!w._lastBpsPush || now - w._lastBpsPush >= 950) {
        w.bpsHist.push(w.bps);
        if (w.bpsHist.length > 60) w.bpsHist.shift();
        w._lastBpsPush = now;
      }
    }
  }

  async function pollHandover() {
    const ho = await getStaticJSON(HANDOVER_FILE);
    if (!ho || !ho.wans) return;
    for (const incoming of ho.wans) {
      const w = ensureWan(incoming.wan_id);
      w.handover = {
        events_total:        incoming.events_total        || 0,
        intervals_tracked:   incoming.intervals_tracked   || 0,
        confidence:          incoming.confidence          || 'cold',
        cov:                 incoming.cov                 || 0,
        median_interval_s:   incoming.median_interval_s   || 0,
        next_predicted_in_s: incoming.next_predicted_in_s || 0,
        miss_streak:         incoming.miss_streak         || 0,
        currently_armed:    !!incoming.currently_armed,
      };
    }
  }

  async function pollSessions() {
    let d;
    try { d = await getJSON('/sessions'); } catch { return; }
    if (!d || !d.sessions) return;
    STATE.sessions = d.sessions.map(s => ({
      id: s.id, name: s.name,
      started_ns: s.started_ns, ended_ns: s.ended_ns,
      status: s.status,
    }));
  }

  async function pollFlows() {
    let d;
    try { d = await getJSON('/flows?limit=80'); } catch { return; }
    if (!d || !d.flows) return;
    // Dedupe flow_start/end pairs into a single record by five_tuple
    const byTuple = new Map();
    for (const f of d.flows) {
      const k = f.five_tuple;
      const cur = byTuple.get(k) || {
        ts: Math.floor(f.ts_ns / 1e6), five_tuple: k, category: f.category, mark: f.mark,
        bytes_orig: 0, bytes_reply: 0, wan: 0,  // wan unknown until backend exposes it
      };
      // Keep most recent ts
      cur.ts = Math.max(cur.ts, Math.floor(f.ts_ns / 1e6));
      cur.category = f.category || cur.category;
      cur.mark = f.mark != null ? f.mark : cur.mark;
      byTuple.set(k, cur);
    }
    STATE.flows = Array.from(byTuple.values())
      .sort((a, b) => b.ts - a.ts).slice(0, 60);
  }

  async function pollDiscovery() {
    let d;
    try { d = await getJSON('/discovery?limit=200'); } catch { return; }
    if (!d || !d.events) return;
    // Build a current devices snapshot from dhcp_lease_add + neighbor_add
    // events. Most-recent-wins by MAC.
    const byMac = new Map();
    for (const e of d.events) {
      let info;
      try { info = JSON.parse(e.detail); } catch { continue; }
      const mac = info.mac;
      if (!mac) continue;
      const cur = byMac.get(mac) || { mac, ip: '', hostname: '', last_seen: 0 };
      cur.last_seen = Math.max(cur.last_seen, Math.floor(e.ts_ns / 1e6));
      if (info.ip)       cur.ip       = info.ip;
      if (info.host)     cur.hostname = info.host;
      if (info.hostname) cur.hostname = info.hostname;
      byMac.set(mac, cur);
    }
    STATE.devices = Array.from(byMac.values())
      .sort((a, b) => b.last_seen - a.last_seen);
  }

  // /events catch-up: pull new events since last tick, emit them on the bus
  // so the Live page's event log and the per-WAN handoverTimeline can update
  // without an SSE connection.
  async function pollEvents() {
    let d;
    try { d = await getJSON(`/events?since=${lastEventTsNs}&limit=200`); } catch { return; }
    if (!d || !d.events) return;
    const evs = d.events.slice().sort((a, b) => a.ts_ns - b.ts_ns);
    for (const ev of evs) {
      lastEventTsNs = Math.max(lastEventTsNs, ev.ts_ns);
      STATE.events.push(ev);
      if (STATE.events.length > 600) STATE.events.shift();
      bus.emit(ev.kind, ev);
      bus.emit('event', ev);
    }
  }

  // ---------- mutations API (replaces direct STATE writes) ----------------

  const api = {
    async startSession(name, metadata) {
      const r = await postJSON('/sessions', { name, metadata: metadata || {} });
      await pollSessions();
      return r;
    },
    async stopSession(id) {
      const r = await postJSON(`/sessions/${id}/stop`);
      await pollSessions();
      return r;
    },
    async deleteSession(id) {
      await delJSON(`/sessions/${id}`);
      await pollSessions();
    },
    downloadSessionUrl(id) { return `${API}/sessions/${id}/download`; },
    async getSession(id)   { return await getJSON(`/sessions/${id}`); },
  };

  // ---------- driver ------------------------------------------------------

  function startPolling() {
    // initial burst
    pollHealth(); pollMetrics(); pollWans(); pollWanStats();
    pollHandover(); pollSessions(); pollFlows();
    pollDiscovery(); pollEvents();

    // cadenced loops
    setInterval(pollHealth,    5000);
    setInterval(pollMetrics,   1000);
    setInterval(pollWans,      1000);
    setInterval(pollWanStats,  1000);
    setInterval(pollHandover,  1000);
    setInterval(pollSessions,  5000);
    setInterval(pollFlows,     3000);
    setInterval(pollDiscovery, 5000);
    setInterval(pollEvents,    1000);

    // 1Hz tick for pages that hook 'tick' (matches mock cadence)
    setInterval(() => bus.emit('tick', { tick: Date.now() }), 1000);
  }

  // start as soon as DOM is parsed
  if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', startPolling);
  } else {
    startPolling();
  }

  // ---------- expose ------------------------------------------------------

  window.RATAN = { bus, STATE, fmt, highlightJSON, sparkline, mountNav, api };
})();
