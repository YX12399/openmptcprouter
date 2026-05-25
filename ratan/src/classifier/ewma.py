# SPDX-License-Identifier: GPL-2.0
"""Tiny EWMA filter, separated for clean unit testing.

new = alpha * sample + (1 - alpha) * old

alpha is the responsiveness:
  - alpha=0.5 -> half-life 1 sample (very fast)
  - alpha=0.1 -> half-life ~7 samples
  - alpha=0.01 -> half-life ~69 samples (very slow)

At a 50ms sample cadence:
  - alpha=0.5 -> half-life 50ms      (used for `transient` filter)
  - alpha=0.01 -> half-life ~3.5s    (used for `baseline` filter)
"""

import math


class Ewma:
    def __init__(self, alpha: float, initial: float = 0.0):
        if not (0.0 < alpha <= 1.0):
            raise ValueError(f"alpha must be in (0, 1]; got {alpha}")
        self.alpha = alpha
        self.value = float(initial)
        self.initialized = False

    def update(self, sample: float) -> float:
        if not self.initialized:
            self.value = float(sample)
            self.initialized = True
        else:
            self.value = self.alpha * sample + (1.0 - self.alpha) * self.value
        return self.value

    def reset(self, initial: float = 0.0) -> None:
        self.value = float(initial)
        self.initialized = False

    @property
    def half_life_samples(self) -> float:
        if self.alpha >= 1.0:
            return 0.0
        return math.log(0.5) / math.log(1.0 - self.alpha)
