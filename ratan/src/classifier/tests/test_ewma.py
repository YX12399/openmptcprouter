# SPDX-License-Identifier: GPL-2.0
"""EWMA filter tests."""

import sys, os
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))

import unittest
from classifier.ewma import Ewma


class TestEwma(unittest.TestCase):
    def test_initial_value_takes_first_sample(self):
        e = Ewma(alpha=0.1)
        self.assertFalse(e.initialized)
        e.update(100.0)
        self.assertEqual(e.value, 100.0)
        self.assertTrue(e.initialized)

    def test_low_alpha_responds_slowly(self):
        e = Ewma(alpha=0.01)
        e.update(0.0)
        for _ in range(10):
            e.update(100.0)
        # 10 samples at alpha=0.01 -> very small movement toward 100
        self.assertLess(e.value, 15.0)

    def test_high_alpha_responds_fast(self):
        e = Ewma(alpha=0.9)
        e.update(0.0)
        e.update(100.0)
        # 0 -> 0.9*100 + 0.1*0 = 90
        self.assertAlmostEqual(e.value, 90.0, places=3)

    def test_half_life_arithmetic(self):
        # alpha=0.5 -> half-life 1; alpha=0.1 -> ~6.58
        self.assertAlmostEqual(Ewma(alpha=0.5).half_life_samples, 1.0, places=3)
        self.assertAlmostEqual(Ewma(alpha=0.1).half_life_samples, 6.5788, places=3)

    def test_invalid_alpha_rejected(self):
        with self.assertRaises(ValueError):
            Ewma(alpha=0.0)
        with self.assertRaises(ValueError):
            Ewma(alpha=1.5)


if __name__ == "__main__":
    unittest.main()
