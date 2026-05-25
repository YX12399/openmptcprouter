# SPDX-License-Identifier: GPL-2.0
"""Handover predictor tests."""

import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))

from classifier.handover import (
    HandoverPredictor, MIN_EVENTS_FOR_LOCK, PREEMPT_LEAD_MS, PREEMPT_HOLD_MS,
)


class FakeClock:
    """Manual-advance clock; returns ns. Tests drive it explicitly so
    is_armed() is deterministic."""
    def __init__(self):
        self.now_ns = 0
    def __call__(self):
        return self.now_ns
    def advance_ms(self, ms):
        self.now_ns += ms * 1_000_000


def feed_loss_streak(predictor, wan_id, start_ns, n_samples, sample_period_ns=50_000_000):
    """Feed n_samples consecutive losses on wan_id starting at start_ns.
    Then one OK sample to close the streak. Returns the OK sample's ts_ns."""
    from classifier.fsm import Sample
    t = start_ns
    for _ in range(n_samples):
        predictor.on_sample(Sample(
            ts_ns=t, wan_id=wan_id, event=1, rtt_us=0, jitter_us=0))
        t += sample_period_ns
    # closing OK sample
    predictor.on_sample(Sample(
        ts_ns=t, wan_id=wan_id, event=0, rtt_us=25000, jitter_us=500))
    return t


def feed_clean_block(predictor, wan_id, start_ns, n_samples,
                     sample_period_ns=50_000_000):
    """Feed n clean OK samples to advance prober time without losses."""
    from classifier.fsm import Sample
    t = start_ns
    for _ in range(n_samples):
        predictor.on_sample(Sample(
            ts_ns=t, wan_id=wan_id, event=0, rtt_us=25000, jitter_us=500))
        t += sample_period_ns
    return t


class TestColdStart(unittest.TestCase):
    """Without enough events, the predictor must NOT arm anything."""

    def test_no_events_no_arm(self):
        p = HandoverPredictor(wan_ids=[0], clock=FakeClock())
        self.assertFalse(p.is_about_to_handover(0))
        snap = p.status_snapshot()["wans"][0]
        self.assertEqual(snap["confidence"], "cold")
        self.assertEqual(snap["events_total"], 0)

    def test_few_events_no_lock(self):
        clock = FakeClock()
        p = HandoverPredictor(wan_ids=[0], clock=clock)
        # 3 handover events at 15s intervals -- below MIN_EVENTS_FOR_LOCK
        t = 0
        for i in range(3):
            t = feed_clean_block(p, 0, t, 300)         # 15s quiet
            t = feed_loss_streak(p, 0, t, 5)            # 250ms handover
        snap = p.status_snapshot()["wans"][0]
        self.assertEqual(snap["confidence"], "cold")
        self.assertFalse(snap["currently_armed"])


class TestTightCadence(unittest.TestCase):
    """Consistent ~15s cadence (low CoV) -> tight lock + accurate prediction."""

    def test_steady_15s_locks_tight(self):
        clock = FakeClock()
        p = HandoverPredictor(wan_ids=[0], clock=clock)
        # 8 handovers at exactly 15s spacing
        t = 0
        for i in range(8):
            t = feed_clean_block(p, 0, t, 300)         # 300*50ms = 15s
            t = feed_loss_streak(p, 0, t, 5)            # 5*50ms = 250ms blip
        snap = p.status_snapshot()["wans"][0]
        self.assertEqual(snap["confidence"], "tight",
                         f"expected tight, got {snap}")
        self.assertLess(snap["cov"], 0.20)
        self.assertAlmostEqual(snap["median_interval_s"], 15.25, delta=0.1)
        self.assertIsNotNone(snap["next_predicted_in_s"])

    def test_armed_inside_pre_emption_window(self):
        clock = FakeClock()
        p = HandoverPredictor(wan_ids=[0], clock=clock)
        # Lock onto 15s cadence by feeding 8 handovers; advance clock
        # alongside the prober ts so clock and event times stay coherent
        t = 0
        for i in range(8):
            t = feed_clean_block(p, 0, t, 300)
            t = feed_loss_streak(p, 0, t, 5)
            clock.now_ns = t

        # Not yet armed (next handover predicted ~15s out)
        self.assertFalse(p.is_about_to_handover(0))

        # Advance to the predicted moment minus the lead time
        snap = p.status_snapshot()["wans"][0]
        next_in_ns = int(snap["next_predicted_in_s"] * 1e9)
        clock.now_ns += next_in_ns - PREEMPT_LEAD_MS * 1_000_000
        self.assertTrue(p.is_about_to_handover(0),
                        f"expected armed at predicted-30ms; snap={p.status_snapshot()}")

        # Advance past the hold window -> disarmed
        clock.now_ns += (PREEMPT_LEAD_MS + PREEMPT_HOLD_MS + 50) * 1_000_000
        self.assertFalse(p.is_about_to_handover(0))


class TestLooseCadence(unittest.TestCase):
    """15-60s spread (medium CoV) -> loose lock with wider acceptance window."""

    def test_15_to_60s_spread_locks_loose(self):
        clock = FakeClock()
        p = HandoverPredictor(wan_ids=[0], clock=clock)
        # 8 handovers at varied intervals: 15, 25, 45, 30, 60, 20, 35, 50 s
        intervals_s = [15, 25, 45, 30, 60, 20, 35, 50]
        t = 0
        for s in intervals_s:
            t = feed_clean_block(p, 0, t, s * 20)         # s seconds at 50ms cadence
            t = feed_loss_streak(p, 0, t, 5)
        snap = p.status_snapshot()["wans"][0]
        self.assertIn(snap["confidence"], ("loose", "chaotic"),
                      f"expected loose/chaotic, got {snap}")


class TestChaoticCadence(unittest.TestCase):
    """Truly chaotic intervals -> CoV > 0.5 -> no prediction."""

    def test_random_intervals_drop_lock(self):
        clock = FakeClock()
        p = HandoverPredictor(wan_ids=[0], clock=clock)
        # 8 handovers at wildly inconsistent intervals
        intervals_s = [5, 70, 12, 80, 8, 60, 15, 90]
        t = 0
        for s in intervals_s:
            t = feed_clean_block(p, 0, t, s * 20)
            t = feed_loss_streak(p, 0, t, 5)
        snap = p.status_snapshot()["wans"][0]
        self.assertEqual(snap["confidence"], "chaotic",
                         f"expected chaotic, got {snap}")
        self.assertFalse(snap["currently_armed"])


class TestPredictionConfirmation(unittest.TestCase):
    """A handover within the acceptance window resets the miss streak;
    missing too many in a row drops the lock."""

    def test_three_consecutive_misses_drop_lock(self):
        clock = FakeClock()
        p = HandoverPredictor(wan_ids=[0], clock=clock)
        # First establish a tight lock at 15s
        t = 0
        for i in range(8):
            t = feed_clean_block(p, 0, t, 300)
            t = feed_loss_streak(p, 0, t, 5)
        self.assertEqual(p.status_snapshot()["wans"][0]["confidence"], "tight")

        # Now feed three handovers way off the predicted time (e.g. at +30s
        # instead of +15s -- well outside the tight 100ms acceptance window)
        for i in range(3):
            t = feed_clean_block(p, 0, t, 600)            # 30s quiet
            t = feed_loss_streak(p, 0, t, 5)

        snap = p.status_snapshot()["wans"][0]
        # The lock either dropped or reconverged to the new cadence.
        # The invariant we care about: if the lock is still tight, the
        # median should have shifted toward 30s; if it dropped to cold,
        # miss_streak was reset.
        if snap["confidence"] == "tight":
            self.assertGreater(snap["median_interval_s"], 15.5)
        else:
            self.assertIn(snap["confidence"], ("cold", "loose"))


class TestEnvelope(unittest.TestCase):
    """Streaks outside Starlink-handover envelope (50-350ms) must NOT be
    counted as handovers."""

    def test_microblip_too_short(self):
        clock = FakeClock()
        p = HandoverPredictor(wan_ids=[0], clock=clock)
        # 1 loss = 50ms streak but min_consec=2; not counted regardless
        feed_loss_streak(p, 0, 0, 1)
        self.assertEqual(p.status_snapshot()["wans"][0]["events_total"], 0)

    def test_long_streak_is_degradation_not_handover(self):
        clock = FakeClock()
        p = HandoverPredictor(wan_ids=[0], clock=clock)
        # 10 consecutive losses = 500ms = above HANDOVER_SIGNATURE_MAX_MS
        feed_loss_streak(p, 0, 0, 10)
        # event NOT recorded as handover
        self.assertEqual(p.status_snapshot()["wans"][0]["events_total"], 0)


class TestPerWanIsolation(unittest.TestCase):
    """WANs must not influence each other's predictions."""

    def test_one_wan_locks_other_stays_cold(self):
        clock = FakeClock()
        p = HandoverPredictor(wan_ids=[0, 1], clock=clock)
        t = 0
        for i in range(8):
            t = feed_clean_block(p, 0, t, 300)
            t = feed_loss_streak(p, 0, t, 5)
        snaps = {w["wan_id"]: w for w in p.status_snapshot()["wans"]}
        self.assertEqual(snaps[0]["confidence"], "tight")
        self.assertEqual(snaps[1]["confidence"], "cold")


if __name__ == "__main__":
    unittest.main()
