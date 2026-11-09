# SPDX-License-Identifier: GPL-3.0-only
"""Loudness of a stereo 16-bit WAV (BS.1770: K-weighting, 400 ms blocks every 100 ms, the -70 LUFS and -10 LU gates):
integrated loudness, the short-term (3 s) 95th percentile, the sample peak. tools/pr_level.py measures the presets
with it.

  loudness.py FILE.wav [START_S [END_S]]   prints: I (LUFS)  S95 (LUFS)  peak (dBFS)"""
import sys
import numpy as np
from scipy.signal import lfilter

FS = 44100


def kweight(x):
    # the high shelf (+4 dB above ~1.7 kHz) and the RLB high-pass, BS.1770's analog designs at 44.1 kHz
    f0, g, q = 1681.974450955533, 3.999843853973347, 0.7071752369554196
    k = np.tan(np.pi * f0 / FS); vh = 10 ** (g / 20); vb = vh ** 0.4996667741545416
    a0 = 1 + k / q + k * k
    b = [(vh + vb * k / q + k * k) / a0, 2 * (k * k - vh) / a0, (vh - vb * k / q + k * k) / a0]
    a = [1, 2 * (k * k - 1) / a0, (1 - k / q + k * k) / a0]
    y = lfilter(b, a, x, axis=0)
    f0, q = 38.13547087602444, 0.5003270373238773
    k = np.tan(np.pi * f0 / FS)
    a = [1, 2 * (k * k - 1) / (1 + k / q + k * k), (1 - k / q + k * k) / (1 + k / q + k * k)]
    return lfilter([1, -2, 1], a, y, axis=0)


def read(path):
    d = np.fromfile(path, dtype=np.int16, offset=44).reshape(-1, 2)
    return d.astype(np.float64) / 32768.0


def measure(x):
    """(integrated LUFS, short-term 95th percentile LUFS, peak dBFS) of a stereo float array"""
    peak = np.abs(x).max() if len(x) else 0.0
    y = kweight(x)
    p = (y ** 2).sum(axis=1)                    # the channels' powers summed (L, R weights 1)
    c = np.concatenate([[0.0], np.cumsum(p)])

    def blocks(n, hop):
        i = np.arange(0, len(p) - n + 1, hop)
        return (c[i + n] - c[i]) / n
    m = blocks(int(0.4 * FS), int(0.1 * FS))
    lk = lambda z: -0.691 + 10 * np.log10(np.maximum(z, 1e-20))
    g = m[lk(m) > -70]
    if len(g):
        g = g[lk(g) > lk(g.mean()) - 10]
    i = lk(g.mean()) if len(g) else -99.0
    s = blocks(3 * FS, int(0.1 * FS))
    s95 = float(np.percentile(lk(s), 95)) if len(s) else -99.0
    return float(i), max(s95, -99.0), float(20 * np.log10(max(peak, 1e-10)))


def p99(x):
    """the 99th percentile of the 50 ms blocks' peaks, dBFS: the loudest 1 % (a rare pop may pass it; the limiter is
    at -1 dBFS)"""
    n = int(0.05 * FS)
    b = np.abs(x[:len(x) // n * n]).max(axis=1).reshape(-1, n).max(axis=1)
    return float(20 * np.log10(max(np.percentile(b, 99), 1e-10))) if len(b) else -99.0


if __name__ == "__main__":
    x = read(sys.argv[1])
    a = int(float(sys.argv[2]) * FS) if len(sys.argv) > 2 else 0
    b = int(float(sys.argv[3]) * FS) if len(sys.argv) > 3 else len(x)
    print("%.1f %.1f %.1f" % measure(x[a:b]))
