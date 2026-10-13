"""Numerical regression checks for the bounded-cost PHY operations."""
import math
import unittest
import numpy as np

class PhyTests(unittest.TestCase):
    def test_polyphase_matches_convolution(self):
        rng = np.random.default_rng(73)
        for sps in (4, 8):
            taps = rng.integers(-20000, 20000, 10*sps+1, dtype=np.int64)
            poly = np.zeros((sps, 11), dtype=np.int64)
            for phase in range(sps):
                for k in range(11):
                    if phase+k*sps < len(taps):
                        poly[phase, k] = taps[phase+k*sps]
            for _ in range(100):
                levels = rng.choice([-3, -1, 1, 3], 96)
                padded = np.r_[np.zeros(11, dtype=int), levels, np.zeros(11, dtype=int)]
                impulse = np.zeros(108*sps, dtype=np.int64)
                impulse[3*sps//2:3*sps//2+96*sps:sps] = levels
                expected = np.convolve(impulse, taps)[:108*sps]
                actual = []
                for n in range(108*sps):
                    u = n-3*sps//2
                    actual.append(0 if u < 0 else sum(
                        padded[11+u//sps-k]*poly[u % sps, k] for k in range(11)))
                np.testing.assert_array_equal(actual, expected)

    def test_reciprocal_phase_error(self):
        table = np.rint(np.arctan(np.arange(257)/256)*65536/(2*np.pi)).astype(int)
        rng = np.random.default_rng(82)
        largest = 0
        for dot, cross in rng.integers(-4095, 4096, (100000, 2)):
            x, y = abs(int(dot)), abs(int(cross))
            high, low = max(x, y), min(x, y)
            if high == 0:
                continue
            shift = high.bit_length()-8
            high, low = (high >> shift, low >> shift) if shift > 0 else (high << -shift, low << -shift)
            index = min(256, (low*(65535//high)+128) >> 8)
            angle = int(table[index])
            if y > x:
                angle = 16384-angle
            if dot < 0:
                angle = 32768-angle
            if cross < 0:
                angle = -angle
            actual = math.trunc(angle*312500/65536)
            expected = math.atan2(cross, dot)*312500/(2*math.pi)
            largest = max(largest, abs(actual-expected))
        self.assertLess(largest, 550)
        print(f"Maximum phase approximation error: {largest:.2f} Hz at 312.5 kS/s")

if __name__ == "__main__":
    unittest.main()
