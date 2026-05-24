# SPDX-License-Identifier: GPL-2.0
"""Starlink-aware handover predictor (Step 9).

Implements the `preempter` interface that classifier/fsm.py's WanFsm
expects:
    .is_about_to_handover(wan_id) -> bool

Per-WAN it:
  1. Detects handover events from the prober sample stream
     (>=2 consecutive losses within the Starlink-blip envelope of
     50-350ms; longer streaks are real degradation, not handovers).
  2. Tracks inter-event intervals in a sliding window (last 20).
  3. Predicts the next handover from the median interval, gated by the
     coefficient of variation (CoV = sigma / mean):
        CoV < 0.2  -> "tight"   confidence, 100ms acceptance window
        CoV < 0.5  -> "loose"   confidence, 300ms acceptance window
        CoV >= 0.5 -> "chaotic" -- drop the lock, revert to reactive
  4. Arms is_about_to_handover() for [predicted - PREEMPT_LEAD_MS,
     predicted + PREEMPT_HOLD_MS]. The FSM sees this and proactively
     transitions to TRANSIENT, shifting weight off the handover-bound
     subflow ~30ms before the blip lands.
  5. Confirms predictions: a handover within +/- acceptance_window of
     the predicted time is a hit (miss_streak reset). Otherwise miss.
     MAX_MISS_STREAK consecutive misses drops the lock.

The cellular WAN can use the same predictor -- it has its own handover
pattern (tower handoffs as the vehicle moves) that the algorithm will
discover the same way.

Diagnostics: status_snapshot() returns a JSON-friendly dict per WAN
for the /handover endpoint we'll add in Step 11. The orchestrator
writes /run/ratan/handover-status.json periodically so anyone can
cat-and-see the current state.

This module is PURE except for the optional `clock` injection (defaults
to time.monotonic_ns). Tests use a controllable fake clock; production
uses real CLOCK_MONOTONIC coherent with the prober's sample timestamps.
"""

import logging
import statistics
import time
from collections import deque
from dataclasses import dataclass
from typing import Callable, Deque, Dict, List, Optional

log = logging.getLogger("ratan-classifier.handover")


# Tunables (Hz-and-ms-friendly; converted to ns at use sites)
HANDOVER_SIGNATURE_MIN_CONSEC = 2
HANDOVER_SIGNATURE_MIN_MS = 50
HANDOVER_SIGNATURE_MAX_MS = 350    # longer streaks are degradation, not handovers
INTER_EVENT_WINDOW = 20
EVENT_BUFFER_MAX = 200             # ~30 min at avg 9s interval
MIN_EVENTS_FOR_LOCK = 6
PREEMPT_LEAD_MS = 30
PREEMPT_HOLD_MS = 500
MAX_MISS_STREAK = 3
COV_TIGHT = 0.20
COV_LOOSE = 0.50
TIGHT_WINDOW_MS = 100
LOOSE_WINDOW_MS = 300


@dataclass
class HandoverEvent:
    start_ts_ns: int
    duration_ms: int
    consec_losses: int


@dataclass
class WanStats:
    """Diagnostic snapshot for one WAN. JSON-friendly."""
    wan_id: int
    events_total: int
    intervals_tracked: int
    confidence: str       # "cold" | "tight" | "loose" | "chaotic"
    cov: Optional[float]
    median_interval_s: Optional[float]
    next_predicted_in_s: Optional[float]
    miss_streak: int
    currently_armed: bool


class _PerWan:
    """Per-WAN handover detection + prediction state."""

    def __init__(self, wan_id: int, clock: Callable[[], int]):
        self.wan_id = wan_id
        self._clock = clock

        self.events: Deque[HandoverEvent] = deque(maxlen=EVENT_BUFFER_MAX)
        self.intervals_ns: Deque[int] = deque(maxlen=INTER_EVENT_WINDOW)

        # in-flight loss-streak detection
        self._streak_start_ns: Optional[int] = None
        self._streak_count: int = 0

        # prediction state
        self._predicted_next_ns: Optional[int] = None
        self._acceptance_window_ms: int = 0
        self._confidence: str = "cold"
        self._cov: Optional[float] = None
        self._median_interval_ns: Optional[int] = None
        self._miss_streak: int = 0

    # -------- detection --------

    def feed(self, ts_ns: int, is_loss: bool):
        if is_loss:
            if self._streak_start_ns is None:
                self._streak_start_ns = ts_ns
            self._streak_count += 1
            return
        # non-loss sample: close out any in-flight streak
        if self._streak_count >= HANDOVER_SIGNATURE_MIN_CONSEC and \
                self._streak_start_ns is not None:
            self._consider(self._streak_start_ns, ts_ns, self._streak_count)
        self._streak_start_ns = None
        self._streak_count = 0

    def _consider(self, start_ns: int, end_ns: int, count: int):
        duration_ms = (end_ns - start_ns) // 1_000_000
        if not (HANDOVER_SIGNATURE_MIN_MS <= duration_ms <= HANDOVER_SIGNATURE_MAX_MS):
            return
        ev = HandoverEvent(start_ts_ns=start_ns, duration_ms=duration_ms,
                           consec_losses=count)

        # Confirm or miss against the current prediction
        if self._predicted_next_ns is not None:
            window_ns = self._acceptance_window_ms * 1_000_000
            if abs(start_ns - self._predicted_next_ns) <= window_ns:
                self._miss_streak = 0
                log.debug("[wan=%d] handover HIT (delta=%dms)",
                          self.wan_id,
                          (start_ns - self._predicted_next_ns) // 1_000_000)
            else:
                self._miss_streak += 1
                log.info("[wan=%d] handover MISS #%d (delta=%dms)",
                         self.wan_id, self._miss_streak,
                         (start_ns - self._predicted_next_ns) // 1_000_000)
                if self._miss_streak >= MAX_MISS_STREAK:
                    log.warning("[wan=%d] dropping prediction lock after %d misses",
                                self.wan_id, self._miss_streak)
                    self._predicted_next_ns = None
                    self._confidence = "cold"
                    self._miss_streak = 0

        if self.events:
            interval = start_ns - self.events[-1].start_ts_ns
            if interval > 0:
                self.intervals_ns.append(interval)
        self.events.append(ev)
        log.info("[wan=%d] handover detected: %dms duration, %d losses",
                 self.wan_id, duration_ms, count)

        self._recompute(start_ns)

    # -------- prediction --------

    def _recompute(self, now_ns: int):
        n = len(self.intervals_ns)
        if n < MIN_EVENTS_FOR_LOCK - 1:
            self._confidence = "cold"
            self._predicted_next_ns = None
            self._cov = None
            self._median_interval_ns = None
            return

        intervals_s = [i / 1e9 for i in self.intervals_ns]
        mean = statistics.mean(intervals_s)
        stdev = statistics.stdev(intervals_s) if n >= 2 else 0.0
        cov = stdev / mean if mean > 0 else 1.0
        self._cov = cov

        median = statistics.median(intervals_s)
        self._median_interval_ns = int(median * 1e9)

        if cov < COV_TIGHT:
            self._confidence = "tight"
            self._acceptance_window_ms = TIGHT_WINDOW_MS
            self._predicted_next_ns = now_ns + self._median_interval_ns
        elif cov < COV_LOOSE:
            self._confidence = "loose"
            self._acceptance_window_ms = LOOSE_WINDOW_MS
            self._predicted_next_ns = now_ns + self._median_interval_ns
        else:
            self._confidence = "chaotic"
            self._predicted_next_ns = None

        if self._predicted_next_ns is not None:
            log.info("[wan=%d] %s lock; median=%.1fs CoV=%.3f -> next in ~%.1fs",
                     self.wan_id, self._confidence, median, cov,
                     self._median_interval_ns / 1e9)

    # -------- preempter interface --------

    def is_armed(self) -> bool:
        if self._predicted_next_ns is None:
            return False
        now = self._clock()
        arm_start = self._predicted_next_ns - PREEMPT_LEAD_MS * 1_000_000
        arm_end = self._predicted_next_ns + PREEMPT_HOLD_MS * 1_000_000
        return arm_start <= now <= arm_end

    # -------- diagnostics --------

    def snapshot(self) -> WanStats:
        now = self._clock()
        next_in_s = None
        if self._predicted_next_ns is not None:
            next_in_s = (self._predicted_next_ns - now) / 1e9
        return WanStats(
            wan_id=self.wan_id,
            events_total=len(self.events),
            intervals_tracked=len(self.intervals_ns),
            confidence=self._confidence,
            cov=self._cov,
            median_interval_s=(self._median_interval_ns / 1e9
                               if self._median_interval_ns else None),
            next_predicted_in_s=next_in_s,
            miss_streak=self._miss_streak,
            currently_armed=self.is_armed(),
        )


class HandoverPredictor:
    """Multi-WAN handover predictor. Implements the preempter interface
    expected by classifier.fsm.WanFsm.

    Usage:
        predictor = HandoverPredictor(wan_ids=[0, 1])
        for fsm in fsms.values():
            fsm.preempter = predictor

        for sample in sample_stream:
            predictor.on_sample(sample)        # detect handovers first
            transition = fsms[sample.wan_id].on_sample(sample)
            ...
    """

    def __init__(self, wan_ids: List[int],
                 clock: Optional[Callable[[], int]] = None):
        clk = clock or time.monotonic_ns
        self._per_wan: Dict[int, _PerWan] = {
            int(wid): _PerWan(int(wid), clk) for wid in wan_ids
        }

    def on_sample(self, sample) -> None:
        """Called by orchestrator BEFORE the FSM sees the sample."""
        st = self._per_wan.get(sample.wan_id)
        if st is None:
            return
        st.feed(sample.ts_ns, sample.is_loss)

    def is_about_to_handover(self, wan_id: int) -> bool:
        """FSM preempter API."""
        st = self._per_wan.get(int(wan_id))
        return False if st is None else st.is_armed()

    def status_snapshot(self) -> dict:
        """JSON-friendly diagnostic for /run/ratan/handover-status.json."""
        return {
            "wans": [vars(st.snapshot()) for st in self._per_wan.values()],
        }
