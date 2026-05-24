# SPDX-License-Identifier: GPL-2.0
"""FSM unit tests. Run with: python3 -m unittest discover -s src/classifier
Or:                        cd src/classifier && python3 -m unittest tests"""

import unittest
import sys
import os
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))

from classifier.fsm import WanFsm, FsmConfig, Sample, State

NS = 1_000_000


def mk_sample(t_ms: int, wan_id: int = 0, rtt_us: int = 25000,
              loss: bool = False) -> Sample:
    return Sample(
        ts_ns=t_ms * NS,
        wan_id=wan_id,
        event=1 if loss else 0,
        rtt_us=0 if loss else rtt_us,
        jitter_us=500,
    )


def seed_healthy_baseline(fsm: WanFsm, t0: int = 0, n: int = 100,
                          rtt_us: int = 25000) -> int:
    """Feed n clean samples 50ms apart so the baseline EWMA + anchor settle.
    Returns the timestamp after the last sample."""
    t = t0
    for i in range(n):
        fsm.on_sample(mk_sample(t, rtt_us=rtt_us))
        t += 50
    return t


class TestStarlinkBlip(unittest.TestCase):
    """The crucial scenario: a 200ms loss blip (Starlink handover signature)
    MUST NOT push us out of HEALTHY into DEGRADING with full weight decay."""

    def setUp(self):
        self.cfg = FsmConfig()
        self.fsm = WanFsm(wan_id=0, capacity_mbps=200, cfg=self.cfg,
                          baseline_weight=70)

    def test_short_blip_enters_transient_not_degrading(self):
        t = seed_healthy_baseline(self.fsm)
        self.assertEqual(self.fsm.state, State.HEALTHY)
        self.assertEqual(self.fsm.weight, 70)

        # blip: 5 consecutive losses = 250ms of loss
        for _ in range(5):
            self.fsm.on_sample(mk_sample(t, loss=True))
            t += 50

        # After enough losses the short window hits the blip threshold.
        # Should be TRANSIENT, not DEGRADING.
        self.assertEqual(self.fsm.state, State.TRANSIENT)
        # CRITICAL: weight unchanged
        self.assertEqual(self.fsm.weight, 70,
                         "weight must NOT change during TRANSIENT")

    def test_short_blip_recovers_to_healthy(self):
        t = seed_healthy_baseline(self.fsm)

        # 4 losses, then 25 OK samples -> should return to HEALTHY
        for _ in range(4):
            self.fsm.on_sample(mk_sample(t, loss=True))
            t += 50
        # may or may not have entered TRANSIENT yet; both are valid
        for _ in range(40):
            self.fsm.on_sample(mk_sample(t))
            t += 50
        self.assertEqual(self.fsm.state, State.HEALTHY)
        self.assertEqual(self.fsm.weight, 70)

    def test_repeated_blips_never_decay_weight(self):
        """The Starlink-thrash regression: ten 250ms handovers in a row
        should leave us in HEALTHY (or TRANSIENT) at full baseline weight
        the whole time."""
        t = seed_healthy_baseline(self.fsm)

        min_weight = self.fsm.weight
        for handover in range(10):
            # blip
            for _ in range(5):
                self.fsm.on_sample(mk_sample(t, loss=True))
                t += 50
            # quiet period (recover)
            for _ in range(60):
                self.fsm.on_sample(mk_sample(t))
                t += 50
            min_weight = min(min_weight, self.fsm.weight)

        # weight should never have dropped from baseline
        self.assertEqual(min_weight, 70,
                         "ten back-to-back handovers must NOT decay weight")


class TestSustainedDegradation(unittest.TestCase):
    """A real degradation (sustained loss or sustained high RTT) MUST
    eventually move us out of HEALTHY into DEGRADING."""

    def setUp(self):
        self.cfg = FsmConfig()
        self.fsm = WanFsm(wan_id=0, capacity_mbps=200, cfg=self.cfg,
                          baseline_weight=70)

    def test_sustained_intermittent_loss_enters_degrading(self):
        t = seed_healthy_baseline(self.fsm)

        # 4s of 20% intermittent loss (1 in every 5 samples).
        # Below down_loss_pct (1.0s consec) but above degrading_loss_pct
        # (10% in 1s window) -- should trigger DEGRADING via sustained
        # worsening, NOT DOWN (no long consecutive streak).
        for _ in range(80):
            self.fsm.on_sample(mk_sample(t, loss=True)); t += 50
            for _ in range(4):
                self.fsm.on_sample(mk_sample(t)); t += 50

        self.assertEqual(self.fsm.state, State.DEGRADING,
                         f"expected DEGRADING, got {self.fsm.state}")
        self.assertLess(self.fsm.weight, 70,
                        "weight must have decayed")

    def test_degrading_decay_is_smooth(self):
        cfg = FsmConfig(degrading_decay_ms=1000, degrading_weight_floor=5)
        fsm = WanFsm(wan_id=0, capacity_mbps=200, cfg=cfg, baseline_weight=70)
        t = seed_healthy_baseline(fsm)
        # force DEGRADING by setting state directly (we already trust
        # _evaluate_transitions; here we just check _compute_weight)
        fsm._enter_state(State.DEGRADING, t * NS)
        weights = []
        for ms in (0, 250, 500, 750, 1000, 1500):
            w = fsm._compute_weight((t + ms) * NS)
            weights.append(w)
        # monotonic decay from baseline to floor
        self.assertEqual(weights[0], 70)
        self.assertEqual(weights[-1], 5)
        for i in range(len(weights) - 1):
            self.assertGreaterEqual(weights[i], weights[i + 1])


class TestDown(unittest.TestCase):
    """A full blackhole MUST hit DOWN within down_sustain_ms and set weight 0."""

    def test_sustained_blackhole_hits_down(self):
        """1.5s of pure loss -> DOWN. (Note: shorter blackholes are handled
        by the prober's independent fast-failover writing weight=0 at
        ~150ms; the classifier's DOWN state is for the sustained case.)"""
        cfg = FsmConfig()
        fsm = WanFsm(wan_id=0, capacity_mbps=200, cfg=cfg, baseline_weight=70)
        t = seed_healthy_baseline(fsm)

        # 30 losses x 50ms = 1.5s of pure loss
        for _ in range(30):
            fsm.on_sample(mk_sample(t, loss=True))
            t += 50

        self.assertEqual(fsm.state, State.DOWN)
        self.assertEqual(fsm.weight, 0)

    def test_down_recovery(self):
        cfg = FsmConfig(down_recovery_sustain_ms=500)
        fsm = WanFsm(wan_id=0, capacity_mbps=200, cfg=cfg, baseline_weight=70)
        t = seed_healthy_baseline(fsm)
        # 1.5s blackhole
        for _ in range(30):
            fsm.on_sample(mk_sample(t, loss=True)); t += 50
        self.assertEqual(fsm.state, State.DOWN)

        # 1s of clean samples (config's down_recovery_sustain_ms=500ms)
        for _ in range(20):
            fsm.on_sample(mk_sample(t)); t += 50
        self.assertEqual(fsm.state, State.HEALTHY)
        self.assertEqual(fsm.weight, 70)


class TestHandoverPreemption(unittest.TestCase):
    """The optional preempter hook lets Step 9 push us to TRANSIENT
    proactively, before the blip even hits."""

    def test_preempter_forces_transient(self):
        class Preempter:
            def __init__(self): self.armed = False
            def is_about_to_handover(self, wan_id): return self.armed

        cfg = FsmConfig()
        preempter = Preempter()
        fsm = WanFsm(wan_id=0, capacity_mbps=200, cfg=cfg,
                     baseline_weight=70, preempter=preempter)
        # seed runs with preempter disarmed -- stays HEALTHY
        seed_healthy_baseline(fsm)
        self.assertEqual(fsm.state, State.HEALTHY)

        # arm and feed one sample -- preempter fires
        preempter.armed = True
        tr = fsm.on_sample(mk_sample(5000))
        self.assertIsNotNone(tr)
        self.assertEqual(tr.to_state, State.TRANSIENT)
        self.assertEqual(tr.reason, "handover_predicted")
        # critical invariant: weight unchanged
        self.assertEqual(fsm.weight, 70)


class TestNoSpuriousTransitions(unittest.TestCase):
    """A steady-state clean stream must NOT emit transitions or weight changes."""

    def test_clean_stream_silent(self):
        cfg = FsmConfig()
        fsm = WanFsm(wan_id=0, capacity_mbps=200, cfg=cfg, baseline_weight=70)
        seed_healthy_baseline(fsm)
        transitions = 0
        for i in range(100, 500):
            tr = fsm.on_sample(mk_sample(i * 50))
            if tr is not None:
                transitions += 1
        self.assertEqual(transitions, 0,
                         "steady state must be silent (no spurious transitions)")


if __name__ == "__main__":
    unittest.main()
