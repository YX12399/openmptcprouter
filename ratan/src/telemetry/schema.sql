-- SPDX-License-Identifier: GPL-2.0
-- RATAN telemetry schema. Run once at first start (idempotent via IF NOT EXISTS).
-- All timestamps are uint64 ns from CLOCK_MONOTONIC of the router; absolute
-- wallclock is derived at export time using boot_time stored in metadata.

PRAGMA journal_mode = WAL;
PRAGMA synchronous  = NORMAL;
PRAGMA temp_store   = MEMORY;
PRAGMA foreign_keys = ON;

CREATE TABLE IF NOT EXISTS metadata (
    key   TEXT PRIMARY KEY,
    value TEXT
);

-- Continuous per-probe-result samples (from ratan-prober). High-volume:
-- 50ms cadence x 8 WANs ~= 160 rows/sec ~= 14M rows/day. Rolled up at 7d.
CREATE TABLE IF NOT EXISTS samples (
    ts_ns        INTEGER NOT NULL,
    wan_id       INTEGER NOT NULL,
    event        INTEGER NOT NULL,   -- RATAN_SAMPLE_*
    seq          INTEGER NOT NULL,
    rtt_us       INTEGER,            -- NULL/0 on loss
    loss_count   INTEGER NOT NULL,
    jitter_us    INTEGER NOT NULL,
    session_id   INTEGER REFERENCES sessions(id) ON DELETE SET NULL
);
CREATE INDEX IF NOT EXISTS samples_ts_idx        ON samples(ts_ns);
CREATE INDEX IF NOT EXISTS samples_wan_ts_idx    ON samples(wan_id, ts_ns);
CREATE INDEX IF NOT EXISTS samples_session_idx   ON samples(session_id);

-- Classifier FSM state transitions (Step 5 will emit these).
CREATE TABLE IF NOT EXISTS state_transitions (
    ts_ns       INTEGER NOT NULL,
    wan_id      INTEGER NOT NULL,
    from_state  TEXT NOT NULL,
    to_state    TEXT NOT NULL,
    reason      TEXT,
    session_id  INTEGER REFERENCES sessions(id) ON DELETE SET NULL
);
CREATE INDEX IF NOT EXISTS state_ts_idx          ON state_transitions(ts_ns);
CREATE INDEX IF NOT EXISTS state_session_idx     ON state_transitions(session_id);

-- Weight changes (from predictor or fast-failover from prober).
CREATE TABLE IF NOT EXISTS weights (
    ts_ns      INTEGER NOT NULL,
    wan_id     INTEGER NOT NULL,
    weight     INTEGER NOT NULL,    -- 0..100
    source     TEXT NOT NULL,       -- 'predict' | 'prober' | 'manual'
    session_id INTEGER REFERENCES sessions(id) ON DELETE SET NULL
);
CREATE INDEX IF NOT EXISTS weights_ts_idx        ON weights(ts_ns);
CREATE INDEX IF NOT EXISTS weights_session_idx   ON weights(session_id);

-- Discovery events (Step 4b will emit these).
CREATE TABLE IF NOT EXISTS discovery (
    ts_ns   INTEGER NOT NULL,
    kind    TEXT NOT NULL,         -- 'wan_up' | 'wan_down' | 'lan_neighbor' | 'mptcp_subflow' | ...
    detail  TEXT                   -- JSON blob
);
CREATE INDEX IF NOT EXISTS discovery_ts_idx ON discovery(ts_ns);

-- Per-flow events (Step 4b).
CREATE TABLE IF NOT EXISTS flows (
    ts_ns        INTEGER NOT NULL,
    event        TEXT NOT NULL,        -- 'flow_start' | 'flow_end' | 'flow_update'
    five_tuple   TEXT NOT NULL,        -- "proto:srcip:srcport-dstip:dstport"
    category     TEXT,                 -- nDPI category, e.g. 'VideoCall/Teams'
    mark         INTEGER
);
CREATE INDEX IF NOT EXISTS flows_ts_idx ON flows(ts_ns);

-- MOS per-second per-flow (Step 4b).
CREATE TABLE IF NOT EXISTS mos (
    ts_ns      INTEGER NOT NULL,
    flow_id    TEXT NOT NULL,
    mos        REAL NOT NULL,
    rtt_ms     REAL,
    loss_pct   REAL,
    jitter_ms  REAL,
    session_id INTEGER REFERENCES sessions(id) ON DELETE SET NULL
);
CREATE INDEX IF NOT EXISTS mos_ts_idx        ON mos(ts_ns);
CREATE INDEX IF NOT EXISTS mos_session_idx   ON mos(session_id);

-- Test session recordings (the bookends around inject-driven tests).
CREATE TABLE IF NOT EXISTS sessions (
    id         INTEGER PRIMARY KEY AUTOINCREMENT,
    name       TEXT NOT NULL,
    started_ns INTEGER NOT NULL,
    ended_ns   INTEGER,
    status     TEXT NOT NULL DEFAULT 'recording',
                   -- 'recording' | 'completed' | 'failed' | 'cancelled'
    metadata   TEXT,                  -- JSON: user-supplied at start
    summary    TEXT                   -- JSON: computed at end
);
CREATE INDEX IF NOT EXISTS sessions_started_idx ON sessions(started_ns);
CREATE INDEX IF NOT EXISTS sessions_status_idx  ON sessions(status);

-- Inject events fired during sessions (Step 4b emits via ratan-test).
CREATE TABLE IF NOT EXISTS injects (
    ts_ns      INTEGER NOT NULL,
    session_id INTEGER NOT NULL REFERENCES sessions(id) ON DELETE CASCADE,
    action     TEXT NOT NULL,           -- 'drop_iface' | 'restore_iface' | 'netem' | 'mark'
    target     TEXT,
    detail     TEXT                     -- JSON
);
CREATE INDEX IF NOT EXISTS injects_session_idx ON injects(session_id);
