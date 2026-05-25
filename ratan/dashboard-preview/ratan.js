/* ============================================================================
   RATAN dashboard — shared client
   - Mock data simulator (drop-in replacement for /stream SSE + REST endpoints)
   - Tiny pub/sub bus
   - DOM helpers, number formatters, JSON syntax highlighter
   - Nav injection so every page shares one source of truth
   In production, swap MockBus for an EventSource('/stream') wrapper hitting
   the real router at 127.0.0.1:9180.
   ============================================================================ */

(() => {
  'use strict';

  // ---------- tiny pub/sub ------------------------------------------------

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

  // ---------- state ------------------------------------------------------

  const STATE = {
    counters: {
      samples_total: 12483,
      samples_loss: 27,
      samples_failover: 3,
      state_transitions: 18,
      weight_changes: 42,
      flow_events: 91,
      mos_events: 60,
      discovery_events: 200,
      http_requests: 884,
      sse_clients: 1,
      sessions_started: 4,
      sessions_completed: 3,
      active_session_id: 0,
    },
    wans: [
      {
        wan: 0,
        iface: 'wan_starlink',
        label: 'Starlink',
        state: 'HEALTHY',
        weight: 70,
        rtt_us: 25000,
        jitter_us: 500,
        loss: 0.0,
        bps: 18_400_000,
        rttHist: [],
        bpsHist: [],
        handover: {
          events_total: 47,
          intervals_tracked: 20,
          confidence: 'tight',
          cov: 0.18,
          median_interval_s: 15.3,
          next_predicted_in_s: 4.3,
          miss_streak: 0,
          currently_armed: false,
        },
        handoverTimeline: [], // [{t, hit}]
      },
      {
        wan: 1,
        iface: 'wan_lte',
        label: 'Cellular (LTE)',
        state: 'HEALTHY',
        weight: 30,
        rtt_us: 42000,
        jitter_us: 1200,
        loss: 0.1,
        bps: 6_200_000,
        rttHist: [],
        bpsHist: [],
        handover: {
          events_total: 0,
          intervals_tracked: 0,
          confidence: 'cold',
          cov: 0,
          median_interval_s: 0,
          next_predicted_in_s: 0,
          miss_streak: 0,
          currently_armed: false,
        },
        handoverTimeline: [],
      },
    ],
    flows: [
      { ts: Date.now()-12_000, five_tuple: 'udp:192.168.1.14:51820→52.114.7.8:443',  category: 'VideoCall/Teams',     mark: 42, bytes_orig: 8_412_000, bytes_reply: 14_220_000, wan: 0 },
      { ts: Date.now()-9_400,  five_tuple: 'udp:192.168.1.14:51821→142.250.179.78:443', category: 'VideoCall/Meet',  mark: 42, bytes_orig: 2_104_000, bytes_reply: 3_812_000, wan: 0 },
      { ts: Date.now()-180_000,five_tuple: 'tcp:192.168.1.20:54211→140.82.114.4:443',category: 'Web/GitHub',        mark: 10, bytes_orig: 412_000,   bytes_reply: 1_802_000, wan: 1 },
      { ts: Date.now()-310_000,five_tuple: 'tcp:192.168.1.20:54218→151.101.1.140:443',category: 'Web/Reddit',       mark: 10, bytes_orig: 28_000,    bytes_reply: 412_000,   wan: 0 },
      { ts: Date.now()-44_000, five_tuple: 'tcp:192.168.1.41:62110→104.16.85.20:443',category: 'Streaming/Spotify',  mark: 20, bytes_orig: 142_000,   bytes_reply: 4_812_000, wan: 0 },
      { ts: Date.now()-22_000, five_tuple: 'udp:192.168.1.32:50211→8.8.8.8:443',     category: 'DNS-over-QUIC',     mark: 5,  bytes_orig: 12_400,    bytes_reply: 18_200,    wan: 1 },
      { ts: Date.now()-720_000,five_tuple: 'tcp:192.168.1.51:33812→185.125.190.39:443',category: 'OS/Ubuntu-Update',mark: 10, bytes_orig: 2_412_000, bytes_reply: 142_812_000,wan: 0 },
    ],
    devices: [
      { ip: '192.168.1.14', mac: '8c:85:90:c2:11:4f', hostname: 'macbook-pro-rohan', last_seen: Date.now()-3_000 },
      { ip: '192.168.1.20', mac: 'aa:bb:cc:dd:ee:ff', hostname: 'ubuntu-workstn',     last_seen: Date.now()-12_000 },
      { ip: '192.168.1.32', mac: '3a:f1:a2:88:01:ce', hostname: 'iphone-15',          last_seen: Date.now()-44_000 },
      { ip: '192.168.1.41', mac: 'b8:27:eb:7a:09:11', hostname: 'rpi-mediaroom',      last_seen: Date.now()-120_000 },
      { ip: '192.168.1.51', mac: '94:c6:91:42:0a:b3', hostname: 'thinkpad-x1',        last_seen: Date.now()-600_000 },
      { ip: '192.168.1.78', mac: 'fc:fb:fb:01:9e:2a', hostname: 'unifi-cam-front',    last_seen: Date.now()-5_000 },
      { ip: '192.168.1.91', mac: '00:1e:c0:88:11:42', hostname: 'tplink-switch',      last_seen: Date.now()-2_000 },
    ],
    sessions: [
      { id: 1, name: 'demo_kpi_a',    started_ns: Date.now()*1e6 - 720_000*1e6, ended_ns: Date.now()*1e6 - 660_000*1e6, status: 'completed' },
      { id: 2, name: 'jlr_drive_01',  started_ns: Date.now()*1e6 - 540_000*1e6, ended_ns: Date.now()*1e6 - 420_000*1e6, status: 'completed' },
      { id: 3, name: 'leo_handover_burn_in', started_ns: Date.now()*1e6 - 360_000*1e6, ended_ns: Date.now()*1e6 - 280_000*1e6, status: 'completed' },
      { id: 4, name: 'wan1_netem_drop_30pct',started_ns: Date.now()*1e6 - 120_000*1e6, ended_ns: 0,                       status: 'recording' },
    ],
    events: [], // ring of last 600 SSE events for live page
  };

  // seed rtt history for each wan (60 points)
  STATE.wans.forEach(w => {
    for (let i = 0; i < 60; i++) {
      w.rttHist.push(w.rtt_us + (Math.random()-0.5)*4000);
      w.bpsHist.push(w.bps * (0.85 + Math.random()*0.30));
    }
  });

  // seed handover timeline for starlink (20 ticks over last 5 min)
  {
    const w = STATE.wans[0];
    const now = Date.now();
    for (let i = 19; i >= 0; i--) {
      w.handoverTimeline.push({
        t: now - i * 15_300,
        hit: Math.random() > 0.12, // mostly hits
      });
    }
  }

  // ---------- helpers ----------------------------------------------------

  const fmt = {
    int: (n) => n.toLocaleString('en-US'),
    bytes: (b) => {
      if (b < 1024) return b + ' B';
      if (b < 1024*1024) return (b/1024).toFixed(1) + ' KB';
      if (b < 1024*1024*1024) return (b/1024/1024).toFixed(1) + ' MB';
      return (b/1024/1024/1024).toFixed(2) + ' GB';
    },
    bps: (b) => {
      const bits = b * 8;
      if (bits < 1000) return bits.toFixed(0) + ' bps';
      if (bits < 1e6) return (bits/1000).toFixed(1) + ' Kbps';
      if (bits < 1e9) return (bits/1e6).toFixed(1) + ' Mbps';
      return (bits/1e9).toFixed(2) + ' Gbps';
    },
    us: (us) => (us/1000).toFixed(1),
    ms: (ms) => ms.toFixed(1),
    pct: (p) => p.toFixed(2) + '%',
    ago: (ts) => {
      const s = (Date.now() - ts) / 1000;
      if (s < 1) return 'now';
      if (s < 60) return Math.floor(s) + 's ago';
      if (s < 3600) return Math.floor(s/60) + 'm ago';
      if (s < 86400) return Math.floor(s/3600) + 'h ago';
      return Math.floor(s/86400) + 'd ago';
    },
    dur: (ms) => {
      const s = Math.floor(ms/1000);
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

  // syntax highlight JSON
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

  // sparkline svg from array of numbers
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

  // ---------- mock simulator --------------------------------------------

  // Drives realistic evolution of WANs. State machine:
  //  - HEALTHY -> TRANSIENT on a high-loss tick
  //  - TRANSIENT -> DEGRADING after sustained loss
  //  - DEGRADING -> DOWN on cliff
  //  - any -> HEALTHY when recovery condition met
  // Also drives a Starlink-style handover phase tracker on wan 0.

  let tick = 0;
  let starlinkPhaseT = 0; // seconds within ~15s LEO cycle

  function step() {
    tick++;

    STATE.wans.forEach((w, idx) => {
      // RTT drift
      const targetRtt = (idx === 0 ? 25000 : 42000);
      w.rtt_us += (targetRtt - w.rtt_us) * 0.18 + (Math.random()-0.5)*1500;

      // jitter
      w.jitter_us = Math.max(200, w.jitter_us + (Math.random()-0.5)*300);

      // loss baseline
      w.loss = Math.max(0, w.loss * 0.85 + (Math.random()<0.05 ? Math.random()*2.0 : 0) * 0.15);

      // throughput drift
      const targetBps = (idx === 0 ? 18_400_000 : 6_200_000);
      w.bps = Math.max(50_000, w.bps + (targetBps - w.bps)*0.12 + (Math.random()-0.5)*1_500_000);

      // starlink handover surge every ~15s on wan 0
      if (idx === 0) {
        starlinkPhaseT += 1;
        if (starlinkPhaseT >= 15) {
          // handover event!
          starlinkPhaseT = 0;
          const hit = Math.random() > 0.10;
          w.handoverTimeline.push({ t: Date.now(), hit });
          if (w.handoverTimeline.length > 20) w.handoverTimeline.shift();
          w.handover.events_total += 1;
          w.handover.miss_streak = hit ? 0 : w.handover.miss_streak + 1;
          // brief rtt spike + tiny loss
          w.rtt_us += 18_000;
          w.loss = Math.min(3, w.loss + 0.4);
          emit('inject', {
            ts_ns: Date.now()*1e6, session: STATE.counters.active_session_id,
            action: 'leo_handover', target: w.iface, detail: hit ? 'predicted' : 'missed',
          });
        }
        w.handover.next_predicted_in_s = Math.max(0, 15 - starlinkPhaseT);
        w.handover.currently_armed = starlinkPhaseT >= 12;
        w.handover.median_interval_s = 15.0 + (Math.random()-0.5)*0.4;
        w.handover.cov = 0.16 + (Math.random()-0.5)*0.04;
      }

      // state transitions
      const prevState = w.state;
      if (w.state === 'HEALTHY' && w.loss > 1.0) w.state = 'TRANSIENT';
      else if (w.state === 'TRANSIENT' && w.loss < 0.2) w.state = 'HEALTHY';
      else if (w.state === 'TRANSIENT' && w.loss > 2.5) w.state = 'DEGRADING';
      else if (w.state === 'DEGRADING' && w.loss < 0.8) w.state = 'TRANSIENT';
      if (prevState !== w.state) {
        STATE.counters.state_transitions++;
        emit('state', { ts_ns: Date.now()*1e6, wan: w.wan, from: prevState, to: w.state, reason: 'consec_loss_100ms' });
      }

      // weight nudge from state
      const targetWeight = w.state === 'HEALTHY' ? (idx === 0 ? 70 : 30)
                         : w.state === 'TRANSIENT' ? (idx === 0 ? 55 : 45)
                         : w.state === 'DEGRADING' ? (idx === 0 ? 20 : 80)
                         : 0;
      const prevWeight = w.weight;
      w.weight = Math.round(w.weight + (targetWeight - w.weight) * 0.15);
      if (Math.abs(prevWeight - w.weight) >= 2) {
        STATE.counters.weight_changes++;
        emit('weight', { ts_ns: Date.now()*1e6, wan: w.wan, weight: w.weight, source: 'predict' });
      }

      // history ring
      w.rttHist.push(w.rtt_us); if (w.rttHist.length > 60) w.rttHist.shift();
      w.bpsHist.push(w.bps);   if (w.bpsHist.length > 60) w.bpsHist.shift();

      // sample event every tick
      STATE.counters.samples_total++;
      if (w.loss > 0.3) STATE.counters.samples_loss++;
      emit('sample', {
        ts_ns: Date.now()*1e6, wan: w.wan, event: 0, seq: STATE.counters.samples_total,
        rtt_us: Math.round(w.rtt_us), loss: w.loss > 0.3 ? 1 : 0, jitter_us: Math.round(w.jitter_us),
      });
    });

    // occasional flow + mos
    if (tick % 7 === 0) {
      STATE.counters.flow_events++;
      const flows = ['VideoCall/Teams','VideoCall/Meet','Web/Cloudflare','Streaming/YouTube','OS/Ubuntu-Update'];
      const cat = flows[Math.floor(Math.random()*flows.length)];
      emit('flow', {
        ts_ns: Date.now()*1e6, event: 'flow_start',
        five_tuple: `udp:192.168.1.${10+Math.floor(Math.random()*40)}:${40000+Math.floor(Math.random()*20000)}->${randIP()}:443`,
        mark: cat.startsWith('VideoCall') ? 42 : 10,
        category: cat, bytes_orig: 0, bytes_reply: 0, family: 4,
      });
    }
    if (tick % 5 === 0) {
      STATE.counters.mos_events++;
      const w = STATE.wans[Math.floor(Math.random()*2)];
      const r = Math.max(60, 95 - w.loss*5 - (w.jitter_us/1000));
      const mos = 1 + 0.035*r + 0.000007*r*(r-60)*(100-r);
      emit('mos', {
        ts_ns: Date.now()*1e6, flow: 'udp:...', category: 'VideoCall/Teams',
        wan: w.wan, mos: +mos.toFixed(3), r: +r.toFixed(1),
        rtt_ms: +(w.rtt_us/1000).toFixed(2), loss_pct: +w.loss.toFixed(2), jitter_ms: +(w.jitter_us/1000).toFixed(2),
      });
    }
    if (tick % 11 === 0) {
      STATE.counters.discovery_events++;
      emit('discovery', { ts_ns: Date.now()*1e6, sub: 'neighbor_refresh',
        detail: JSON.stringify({ iface: 'br-lan', ip: '192.168.1.'+(10+Math.floor(Math.random()*80)), mac: randMAC() }) });
    }

    // sse client count drift
    if (tick % 30 === 0) {
      STATE.counters.sse_clients = 1 + Math.floor(Math.random()*3);
      STATE.counters.http_requests += 12 + Math.floor(Math.random()*20);
    }

    bus.emit('tick', { tick });
  }

  function emit(kind, payload) {
    const ev = Object.assign({ kind, ts_ns: Date.now()*1e6 }, payload);
    STATE.events.push(ev);
    if (STATE.events.length > 600) STATE.events.shift();
    bus.emit(kind, ev);
    bus.emit('event', ev);
  }

  function randIP() {
    const ips = ['52.114.7.8','142.250.179.78','140.82.114.4','151.101.1.140','104.16.85.20','185.125.190.39','8.8.4.4'];
    return ips[Math.floor(Math.random()*ips.length)];
  }
  function randMAC() {
    const h = () => Math.floor(Math.random()*256).toString(16).padStart(2,'0');
    return [h(),h(),h(),h(),h(),h()].join(':');
  }

  // start ticking
  setInterval(step, 1000);

  // ---------- nav injection ---------------------------------------------

  const PAGES = [
    { id: 'overview',  href: 'overview.html',  label: 'Overview' },
    { id: 'live',      href: 'live.html',      label: 'Live' },
    { id: 'sessions',  href: 'sessions.html',  label: 'Sessions' },
    { id: 'handover',  href: 'handover.html',  label: 'Handover' },
    { id: 'flows',     href: 'flows.html',     label: 'Flows' },
    { id: 'devices',   href: 'devices.html',   label: 'Devices' },
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
          <span class="dot"></span>
          <span id="nav-uptime">uplink ok</span>
          <span style="color:var(--fg-3);">·</span>
          <span id="nav-build">build a8c19f2</span>
        </div>
      </nav>
    `;
  }

  // expose
  window.RATAN = { bus, STATE, fmt, highlightJSON, sparkline, mountNav };
})();
