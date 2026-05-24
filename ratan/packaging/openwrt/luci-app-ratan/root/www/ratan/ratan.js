/* SPDX-License-Identifier: GPL-2.0
 * ratan.js -- shared utilities for the RATAN dashboard pages.
 */

const RATAN_API = "/cgi-bin/ratan-api";

/* Fetch helpers ------------------------------------------------------ */

async function apiGet(path) {
    const r = await fetch(RATAN_API + path, { cache: "no-store" });
    if (!r.ok) throw new Error(`HTTP ${r.status} for ${path}`);
    return r.json();
}

async function apiPost(path, body) {
    const r = await fetch(RATAN_API + path, {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: body !== undefined ? JSON.stringify(body) : "{}",
    });
    if (!r.ok) throw new Error(`HTTP ${r.status} for ${path}`);
    return r.json();
}

async function apiDelete(path) {
    const r = await fetch(RATAN_API + path, { method: "DELETE" });
    if (!r.ok) throw new Error(`HTTP ${r.status} for ${path}`);
}

/* Try to fetch the on-disk handover status file via uhttpd's static
 * file serving (no CGI roundtrip). Falls back to null if not yet
 * written by the classifier. */
async function fetchHandoverStatus() {
    try {
        const r = await fetch("/ratan-data/handover-status.json",
                              { cache: "no-store" });
        if (!r.ok) return null;
        return await r.json();
    } catch (_) { return null; }
}

/* Header / nav ------------------------------------------------------- */

function renderHeader(active) {
    const nav = [
        { id: "overview", label: "Overview", href: "overview.html" },
        { id: "live",     label: "Live",     href: "live.html" },
        { id: "sessions", label: "Sessions", href: "sessions.html" },
        { id: "handover", label: "Handover", href: "handover.html" },
    ];
    const linksHtml = nav.map(n =>
        `<a href="${n.href}" class="${n.id === active ? 'active' : ''}">${n.label}</a>`
    ).join("");
    document.body.insertAdjacentHTML("afterbegin",
        `<header>
            <h1>RATAN</h1>
            <nav>${linksHtml}</nav>
            <span class="conn" id="conn">checking...</span>
        </header>`);
    pollDaemonHealth();
}

async function pollDaemonHealth() {
    const el = document.getElementById("conn");
    try {
        const r = await fetch(RATAN_API + "/healthz", { cache: "no-store" });
        if (r.ok) {
            el.textContent = "daemon: ok";
            el.className = "conn ok";
        } else {
            el.textContent = `daemon: HTTP ${r.status}`;
            el.className = "conn bad";
        }
    } catch (e) {
        el.textContent = "daemon: unreachable";
        el.className = "conn bad";
    }
    setTimeout(pollDaemonHealth, 5000);
}

/* Formatting --------------------------------------------------------- */

function fmtNs(ns) {
    if (ns === null || ns === undefined) return "—";
    // Monotonic ns from CLOCK_MONOTONIC -- not wallclock. Display as
    // seconds-since-boot for readability.
    return (ns / 1e9).toFixed(2) + "s";
}

function fmtDuration(s) {
    if (s === null || s === undefined) return "—";
    if (Math.abs(s) < 60) return s.toFixed(1) + "s";
    if (Math.abs(s) < 3600) return (s / 60).toFixed(1) + "m";
    return (s / 3600).toFixed(2) + "h";
}

function fmtUs(us) {
    if (us === null || us === undefined || us === 0) return "—";
    return (us / 1000).toFixed(2) + "ms";
}

function fmtBytes(b) {
    if (!b) return "0";
    if (b < 1024) return b + " B";
    if (b < 1024 * 1024) return (b / 1024).toFixed(1) + " KB";
    if (b < 1024 * 1024 * 1024) return (b / 1024 / 1024).toFixed(2) + " MB";
    return (b / 1024 / 1024 / 1024).toFixed(2) + " GB";
}

function tsAgo(tsNs, nowNs) {
    if (!tsNs) return "—";
    const ageS = (nowNs - tsNs) / 1e9;
    if (ageS < 1) return "just now";
    if (ageS < 60) return ageS.toFixed(0) + "s ago";
    if (ageS < 3600) return (ageS / 60).toFixed(0) + "m ago";
    return (ageS / 3600).toFixed(1) + "h ago";
}

function escapeHtml(s) {
    if (s === null || s === undefined) return "";
    return String(s)
        .replaceAll("&", "&amp;")
        .replaceAll("<", "&lt;")
        .replaceAll(">", "&gt;")
        .replaceAll('"', "&quot;");
}

/* Error display ------------------------------------------------------ */

function showError(elId, err) {
    const el = document.getElementById(elId);
    if (!el) return;
    el.innerHTML =
        `<div class="card" style="border-color:var(--bad)">
            <h3 style="color:var(--bad)">Error</h3>
            <div class="mono">${escapeHtml(err.message || err)}</div>
        </div>`;
}
