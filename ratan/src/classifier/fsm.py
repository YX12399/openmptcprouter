# SPDX-License-Identifier: GPL-2.0
"""Per-WAN finite state machine for the RATAN classifier.

States:
    HEALTHY    -- normal operation; baseline (capacity-proportional) weight
    TRANSIENT  -- expected blip (handover or matched-signature spike);
                  weight unchanged so aggregation continues
    DEGRADING  -- sustained worsening; weight smoothly decayed; MP_PRIO=backup
    DOWN       -- probe loss >90% for >150ms; weight=0 immediately

Transitions and the "Starlink-aware" trick:
    The two-channel decomposition (rtt_baseline vs rtt_transient) is what
    stops the 15s-handover thrash. Brief spikes only move the transient
    channel; only sustained worsening of the BASELINE channel moves us
    out of HEALTHY into DEGRADING. The TRANSIENT state is a holding pen
    for blips that match the handover signature -- weight stays at
    baseline so the user never feels the blip.

This module is PURE -- takes samples, returns transitions. No I/O. The
orchestrator (ratan_classifier.py) calls .on_sample() and acts on the
returned transition object.
"""

import enum
from collections import deque
from dataclasses import dataclass, field
from typing import Optional

from .ewma import Ewma


class State(enum.Enum):
    HEALTHY = "HEALTHY"
    TRANSIENT = "TRANSIENT"
    DEGRADING = "DEGRADING"
    DOWN = "DOWN"


@dataclass
class Sample:
    ts_ns: int
    wan_id: int
    event: int     # RATAN_SAMPLE_OK=0, LOSS=1, FAST_FAILOVER=2, RECOVERY=3
    rtt_us: int    # 0 on loss
    jitter_us: int

    @property
    def is_loss(self) -> bool:
        return self.event in (1, 2)


@dataclass
class FsmConfig:
    """All thresholds tunable from /etc/ratan/classifier.json."""

    baseline_weight: int = 50

    # Signal decomposition (alpha values; see ewma.py for cadence math).
    rtt_baseline_alpha: float = 0.01    # slow; ~3.5s half-life at 50ms
    rtt_transient_alpha: float = 0.5    # fast; 50ms half-life

    # Loss rolling window (samples). At 50ms cadence, 20=1s, 200=10s.
    # Used only for the "sustained degradation" check; blip + DOWN
    # detection use time-based consecutive-loss duration (cadence-independent).
    loss_window_samples: int = 20
    loss_window_long_samples: int = 200

    # HEALTHY -> DEGRADING: baseline must worsen sustainably.
    # Uses the SHORT loss window so past handovers age out within ~1s and
    # don't artificially keep the worsening signal high. A single Starlink
    # handover in the 1s window yields ~25% loss for ~1s and then decays
    # below threshold; 2s sustain means it never trips DEGRADING.
    degrading_rtt_factor: float = 2.5
    degrading_loss_pct: float = 10.0
    degrading_sustain_ms: int = 2000

    # HEALTHY -> TRANSIENT: consecutive-loss streak >= this many ms.
    # Matches Starlink handover signatures (50-300ms).
    transient_trigger_ms: int = 100
    # TRANSIENT -> HEALTHY: continuous-OK streak >= this many ms (the blip
    # really ended; the long-loss window doesn't gate this because a single
    # past blip dilutes it for 10s, way longer than the actual recovery).
    transient_recover_ms: int = 200
    # TRANSIENT -> DEGRADING: time-in-state past this means the blip wasn't
    # really a blip; promote to real degradation.
    transient_budget_ms: int = 500

    # DEGRADING decay schedule.
    degrading_weight_floor: int = 5
    degrading_decay_ms: int = 1500

    # DEGRADING -> HEALTHY recovery.
    healthy_rtt_factor: float = 1.5
    healthy_loss_pct: float = 1.0
    healthy_sustain_ms: int = 3000

    # DOWN trigger: consecutive-loss streak this long means the link is
    # really gone, not just handing over. Note the PROBER independently
    # writes weight=0 to the BPF map at 3 consecutive losses (~150ms) for
    # the sub-200ms KPI -- we don't duplicate that here; we just track
    # state so we can manage recovery cleanly.
    down_sustain_ms: int = 1000

    # DOWN -> HEALTHY (skip DEGRADING when probes resume).
    down_recovery_sustain_ms: int = 2000


@dataclass
class Transition:
    wan_id: int
    ts_ns: int
    from_state: State
    to_state: State
    reason: str
    old_weight: int
    new_weight: int


@dataclass
class _Window:
    """Bounded deque of (ts_ns, is_loss) for rolling stats."""
    cap: int
    items: deque = field(init=False)

    def __post_init__(self):
        self.items = deque(maxlen=self.cap)

    def push(self, ts_ns: int, is_loss: bool):
        self.items.append((ts_ns, is_loss))

    def loss_pct(self) -> float:
        if not self.items:
            return 0.0
        return 100.0 * sum(1 for _, l in self.items if l) / len(self.items)

    def consecutive_losses(self) -> int:
        n = 0
        for _, is_loss in reversed(self.items):
            if not is_loss:
                break
            n += 1
        return n

    def __len__(self):
        return len(self.items)


class WanFsm:
    """One instance per monitored WAN.

    Usage:
        fsm = WanFsm(wan_id=0, capacity_mbps=200, cfg=FsmConfig())
        for sample in stream:
            transition = fsm.on_sample(sample)
            if transition:
                emit(transition)
    """

    def __init__(self, wan_id: int, capacity_mbps: float, cfg: FsmConfig,
                 baseline_weight: Optional[int] = None,
                 preempter=None):
        self.wan_id = wan_id
        self.capacity_mbps = capacity_mbps
        self.cfg = cfg
        # capacity-proportional baseline if not overridden
        self.baseline_weight = baseline_weight if baseline_weight is not None \
            else cfg.baseline_weight
        self.preempter = preempter  # optional, .is_about_to_handover(wan_id) -> bool

        self.state = State.HEALTHY
        self.state_entered_ts_ns = 0
        self.weight = self.baseline_weight

        self.rtt_baseline = Ewma(cfg.rtt_baseline_alpha)
        self.rtt_transient = Ewma(cfg.rtt_transient_alpha)
        # Baseline of "what healthy RTT was the last time we WERE healthy",
        # so the degrading-factor check has a real reference point.
        self.healthy_rtt_anchor: Optional[float] = None
        self.loss_short = _Window(cap=cfg.loss_window_samples)
        self.loss_long  = _Window(cap=cfg.loss_window_long_samples)

        # First-loss timestamp for time-based loss-streak detection (the
        # blip-vs-DOWN distinguisher). None when the most recent sample was
        # OK; set to the first-loss ts otherwise.
        self._loss_streak_start_ns: Optional[int] = None
        # Recovery-streak start for healthy/down recovery
        self._recover_streak_start_ns: Optional[int] = None
        # Worsening-streak start for DEGRADING-window tracking
        self._worsen_streak_start_ns: Optional[int] = None

    # ------- public API -------

    def on_sample(self, s: Sample) -> Optional[Transition]:
        # 1. update signal filters and windows
        if not s.is_loss and s.rtt_us > 0:
            self.rtt_baseline.update(s.rtt_us)
            self.rtt_transient.update(s.rtt_us)
        # rolling-window loss state
        self.loss_short.push(s.ts_ns, s.is_loss)
        self.loss_long.push(s.ts_ns, s.is_loss)

        # Time-based loss-streak tracking: independent of sample cadence.
        if s.is_loss:
            if self._loss_streak_start_ns is None:
                self._loss_streak_start_ns = s.ts_ns
        else:
            self._loss_streak_start_ns = None

        # 2. anchor the "healthy RTT" reference periodically while HEALTHY
        if self.state == State.HEALTHY and self.rtt_baseline.initialized:
            self.healthy_rtt_anchor = self.rtt_baseline.value

        # 3. evaluate transitions
        old_state = self.state
        old_weight = self.weight
        new_state, reason = self._evaluate_transitions(s)
        if new_state != self.state:
            self._enter_state(new_state, s.ts_ns)

        # 4. compute weight for the current state (every sample so
        #    DEGRADING decay can progress smoothly)
        self.weight = self._compute_weight(s.ts_ns)

        if new_state != old_state or self.weight != old_weight:
            return Transition(
                wan_id=self.wan_id, ts_ns=s.ts_ns,
                from_state=old_state, to_state=self.state,
                reason=reason if new_state != old_state else "weight_decay",
                old_weight=old_weight, new_weight=self.weight,
            )
        return None

    # ------- internals -------

    def _enter_state(self, new_state: State, ts_ns: int):
        self.state = new_state
        self.state_entered_ts_ns = ts_ns
        # streak resets that fire on every transition
        self._worsen_streak_start_ns = None
        self._recover_streak_start_ns = None

    def _evaluate_transitions(self, s: Sample):
        """Returns (new_state, reason). Pure function of current state +
        per-WAN history; no side effects beyond streak-start tracking."""
        cfg = self.cfg
        now = s.ts_ns
        loss_streak_ms = (
            (now - self._loss_streak_start_ns) / 1_000_000
            if self._loss_streak_start_ns is not None else 0
        )
        loss_pct_short = self.loss_short.loss_pct()
        loss_pct_long = self.loss_long.loss_pct()

        # DOWN trigger has highest priority: a sustained loss streak
        # longer than down_sustain_ms means no probes are returning at all.
        # This takes precedence over TRANSIENT because correctness >
        # gentleness here -- if the link really is gone we MUST drain.
        if loss_streak_ms >= cfg.down_sustain_ms and self.state != State.DOWN:
            return State.DOWN, f"consec_loss>={cfg.down_sustain_ms}ms"

        if self.state == State.DOWN:
            # DOWN -> HEALTHY when probes resume sustainably
            if not s.is_loss:
                if self._recover_streak_start_ns is None:
                    self._recover_streak_start_ns = now
                elif now - self._recover_streak_start_ns >= \
                        cfg.down_recovery_sustain_ms * 1_000_000:
                    return State.HEALTHY, f"probes_resumed_{cfg.down_recovery_sustain_ms}ms"
            else:
                self._recover_streak_start_ns = None
            return State.DOWN, ""

        if self.state == State.HEALTHY:
            # Preempted by Step 9's handover predictor
            if self.preempter and self.preempter.is_about_to_handover(self.wan_id):
                return State.TRANSIENT, "handover_predicted"
            # Blip signature: short consecutive-loss streak
            if loss_streak_ms >= cfg.transient_trigger_ms:
                return State.TRANSIENT, f"consec_loss_{int(loss_streak_ms)}ms"
            # Sustained worsening of baseline -> DEGRADING
            if self._is_baseline_worsening(loss_pct_short):
                if self._worsen_streak_start_ns is None:
                    self._worsen_streak_start_ns = now
                elif now - self._worsen_streak_start_ns >= \
                        cfg.degrading_sustain_ms * 1_000_000:
                    return State.DEGRADING, "baseline_worsening_sustained"
            else:
                self._worsen_streak_start_ns = None
            return State.HEALTHY, ""

        if self.state == State.TRANSIENT:
            # Recovery: continuous-OK streak long enough.
            if not s.is_loss:
                if self._recover_streak_start_ns is None:
                    self._recover_streak_start_ns = now
                elif now - self._recover_streak_start_ns >= \
                        cfg.transient_recover_ms * 1_000_000:
                    return State.HEALTHY, f"blip_ended_{cfg.transient_recover_ms}ms"
            else:
                self._recover_streak_start_ns = None
            # Budget exceeded with no clean recovery -> DEGRADING
            if now - self.state_entered_ts_ns >= \
                    cfg.transient_budget_ms * 1_000_000:
                return State.DEGRADING, f"transient_exceeded_{cfg.transient_budget_ms}ms"
            return State.TRANSIENT, ""

        if self.state == State.DEGRADING:
            if self._is_recovering(loss_pct_short):
                if self._recover_streak_start_ns is None:
                    self._recover_streak_start_ns = now
                elif now - self._recover_streak_start_ns >= \
                        cfg.healthy_sustain_ms * 1_000_000:
                    return State.HEALTHY, f"recovery_sustained_{cfg.healthy_sustain_ms}ms"
            else:
                self._recover_streak_start_ns = None
            return State.DEGRADING, ""

        return self.state, ""

    def _is_baseline_worsening(self, loss_pct: float) -> bool:
        cfg = self.cfg
        if loss_pct >= cfg.degrading_loss_pct:
            return True
        if self.healthy_rtt_anchor is None or not self.rtt_baseline.initialized:
            return False
        return self.rtt_baseline.value >= \
            cfg.degrading_rtt_factor * self.healthy_rtt_anchor

    def _is_recovering(self, loss_pct: float) -> bool:
        cfg = self.cfg
        if loss_pct >= cfg.healthy_loss_pct:
            return False
        if self.healthy_rtt_anchor is None or not self.rtt_baseline.initialized:
            return True  # no reference -> assume any sample below loss threshold
        return self.rtt_baseline.value <= \
            cfg.healthy_rtt_factor * self.healthy_rtt_anchor

    def _compute_weight(self, ts_ns: int) -> int:
        cfg = self.cfg
        if self.state == State.HEALTHY:
            return self.baseline_weight
        if self.state == State.TRANSIENT:
            # critical: weight UNCHANGED during transient -- this is what
            # stops Starlink handover thrash from defeating aggregation.
            return self.baseline_weight
        if self.state == State.DOWN:
            return 0
        if self.state == State.DEGRADING:
            # linear decay from baseline -> floor over degrading_decay_ms
            elapsed_ms = (ts_ns - self.state_entered_ts_ns) / 1_000_000
            if elapsed_ms <= 0:
                return self.baseline_weight
            if elapsed_ms >= cfg.degrading_decay_ms:
                return cfg.degrading_weight_floor
            t = elapsed_ms / cfg.degrading_decay_ms
            return int(round(
                self.baseline_weight - t * (self.baseline_weight - cfg.degrading_weight_floor)
            ))
        return self.weight  # unreachable
