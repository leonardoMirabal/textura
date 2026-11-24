#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""tools/hil_suite.py's analysis on synthetic signals, and the suite offline (--wav) on synthetic recordings.
No hardware: python3 -I tests/hil_suite_check.py (needs numpy). Exit code = failures."""
import contextlib
import io
import os
import sys
import tempfile

import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import hil_suite as H  # noqa: E402

SR = 44100
fails = 0


def check(name, cond, text=""):
    global fails
    if not cond:
        fails += 1
    print(f"{'ok  ' if cond else 'FAIL'}  {name}  {text}")


def noise(n=SR * 8, seed=1, amp=0.1):
    """band-limited noise bursts (like a dense texture): lowpassed gaussian noise with a slow envelope"""
    r = np.random.RandomState(seed)
    x = r.randn(n)
    x = np.convolve(x, np.ones(32) / 32, mode="same")
    env = 0.6 + 0.4 * np.sin(2 * np.pi * 0.7 * np.arange(n) / SR)
    return x * env * amp / np.std(x)


def stereo(seed=1):
    return np.stack([noise(seed=seed), noise(seed=seed + 7)], axis=1)


# clean
x = stereo()
c = H.analyse_clicks(x, SR)
check("clean: no clicks", not c["clicks"], f"{len(c['clicks'])}")
check("clean: no repeats", not c["repeats"], f"{len(c['repeats'])}")
check("clean: no dropouts", not c["drops"], f"{len(c['drops'])}")

# injected clicks
y = x.copy()
for pos in (30000, 100000, 250000):
    y[pos:, 0] += 0.2                       # a step: a late block's jump
c = H.analyse_clicks(y, SR)
check("clicks: 3 injected found", len(c["clicks"]) >= 3, f"{len(c['clicks'])}")
check("clicks: found near the injections", all(any(abs(i - p) < 4 for i in c["clicks"]) for p in (30000, 100000, 250000)))

# a discontinuity from a dropped (deleted) block
ps = np.arange(90000, 110000)
cut = int(ps[np.argmax(np.abs(x[ps - 1, 0] - x[ps + 255, 0]))])      # where the cut jumps most (noise is noise)
y = np.concatenate([x[:cut], x[cut + 256:]])
c = H.analyse_clicks(y, SR)
check("deleted block: a click", any(abs(i - cut) < 4 for i in c["clicks"]),
      f"jump {abs(x[cut - 1, 0] - x[cut + 255, 0]):.3f} at {cut}: {c['clicks'][:5]}")

# zeroed block
y = x.copy()
y[120000:120256] = 0
c = H.analyse_clicks(y, SR)
check("zeroed block: dropout", any(abs(i - 120000) < 300 for i in c["drops"]), f"{c['drops'][:5]}")

# repeated block
y = x.copy()
y[150000 + 256:150000 + 512] = y[150000:150000 + 256]
c = H.analyse_clicks(y, SR)
check("repeated block: found", any(abs(i - 150000) < 600 for i in c["repeats"]), f"{c['repeats'][:5]}")

# repeated block at the jack's 48 kHz block length (279 samples), 1 channel
b = H.block_for(48000)
z = noise(48000 * 8, seed=5)
z[90000 + b:90000 + 2 * b] = z[90000:90000 + b]
c = H.analyse_clicks(z, 48000)
check("repeated block 48 kHz: found", any(abs(i - 90000) < 2 * b for i in c["repeats"]), f"b={b} {c['repeats'][:5]}")

# a held tone is not a repeat
t = 0.2 * np.sin(2 * np.pi * SR / 128 * np.arange(SR * 4) / SR)    # period 128: 256 samples = 2 periods
rep, n, tonal = H.find_repeats(t)
check("tone: skipped as tonal", tonal and not rep)

# natural gaps in a signal that stops are not dropouts
g = np.concatenate([noise(SR * 3, 3), np.zeros(SR * 2), noise(SR * 3, 4)])
check("long silence: no dropout flagged", not H.find_dropouts(g), f"{H.find_dropouts(g)[:3]}")

# lag
l = noise(seed=2)
for lag in (25, 29, 33):
    r = np.concatenate([np.zeros(lag), l[:-lag]])
    check(f"lag {lag}", H.lag_of(l, r) == lag, f"{H.lag_of(l, r)}")

# stereo stats: mono, anti-phase, wide
m = noise(seed=3)
s = H.stereo_stats(np.stack([m, m], axis=1))
check("mono: side gone, corr 1", s["side"] < s["mid"] - 60 and s["corr"] > 0.999, f"{s}")
s = H.stereo_stats(np.stack([m, -m], axis=1))
check("anti-phase: mid gone, corr -1", s["mid"] < s["side"] - 60 and s["corr"] < -0.999, f"{s}")
s = H.stereo_stats(stereo())
check("independent: corr near 0, |L-R| small", abs(s["corr"]) < 0.1 and abs(s["l"] - s["r"]) < 1.0, f"{s}")

# wav round trip
with tempfile.TemporaryDirectory() as d:
    p = os.path.join(d, "a.wav")
    H.write_wav(p, x, SR)
    back, sr = H.read_wav(p)
    check("wav round trip", sr == SR and back.shape == x.shape and np.max(np.abs(back - x)) < 2e-7)

# presets file
allp = H.parse_presets()
check("presets parsed", len(allp) >= 100 and all(len(p["lanes"]) == 4 for p in allp), f"{len(allp)}")
dp = H.default_presets(allp)
check("default presets: 10, busy", len(dp) == 10 and min(H.density(p) for p in dp) > 0, f"{[p['name'] for p in dp]}")


# the suite offline on synthetic recordings
def run_suite(wavdir, only, secs="6"):
    out = io.StringIO()
    with contextlib.redirect_stdout(out):
        rc = H.main(["--wav", wavdir, "--only", only, "--presets", "STEADY RAIN,FULL STORM", "--seconds", secs])
    return rc, out.getvalue()


def put(d, key, usb=None, jack=None):
    if usb is not None:
        H.write_wav(os.path.join(d, f"{key}__usb.wav"), usb, SR)
    if jack is not None:
        H.write_wav(os.path.join(d, f"{key}__jack.wav"), jack, 48000)


with tempfile.TemporaryDirectory() as d:
    j = np.stack([noise(48000 * 8, 11), noise(48000 * 8, 12)], axis=1)
    for p in ("F006__STEADY_RAIN", "F013__FULL_STORM"):
        for mode in ("SPEAKERS", "HEADPHONES"):
            put(d, f"clicks__{p}__{mode}", stereo(), j)
    rc, out = run_suite(d, "clicks")
    check("suite clicks clean: exit 0", rc == 0, out.splitlines()[-1])
    jc = j.copy()
    jc[200000:, 0] += 0.2
    jc[200001:, 0] -= 0.2                     # a click in the jack only
    put(d, "clicks__F006__STEADY_RAIN__SPEAKERS", stereo(), jc)
    rc, out = run_suite(d, "clicks")
    check("suite: jack click, USB clean -> FAIL", rc == 1 and "USB clean" in out, f"rc {rc}")
    rp = stereo()
    rp[100000 + 256:100000 + 512] = rp[100000:100000 + 256]
    put(d, "clicks__F006__STEADY_RAIN__SPEAKERS", rp, j)
    rc, out = run_suite(d, "clicks")
    check("suite: repeated block -> FAIL", rc == 1 and "repeated block" in out, f"rc {rc}")

with tempfile.TemporaryDirectory() as d:
    base = noise(seed=21)
    left = lambda lag, far: np.stack([base, far * np.concatenate([np.zeros(lag), base[:len(base) - lag]])], axis=1)
    sec = 6
    cut = lambda a: a[:SR * sec]
    put(d, "binaural__SPEAKERS", cut(left(0, 1e-4)))
    put(d, "binaural__HEADPHONES_SMALL", cut(left(25, 10 ** (-14 / 20))))
    put(d, "binaural__HEADPHONES_MEDIUM", cut(left(29, 10 ** (-14 / 20))))
    put(d, "binaural__HEADPHONES_LARGE", cut(left(33, 10 ** (-14 / 20))))
    put(d, "binaural__HEADPHONES_AMOUNT_0", cut(left(0, 1e-4)))
    rc, out = run_suite(d, "binaural")
    check("suite binaural good: exit 0", rc == 0, out.strip().splitlines()[-1])
    put(d, "binaural__HEADPHONES_LARGE", cut(left(29, 10 ** (-14 / 20))))     # wrong lag
    put(d, "binaural__HEADPHONES_AMOUNT_0", cut(left(29, 10 ** (-14 / 20))))  # amount 0 still binaural
    rc, out = run_suite(d, "binaural")
    check("suite binaural bad: 2 FAIL", rc == 2, f"rc {rc}")

with tempfile.TemporaryDirectory() as d:
    a, b = noise(seed=31), noise(seed=32)
    mid_, side_ = a, 0.4 * b
    ms = lambda m, s: np.stack([m + s, m - s], axis=1)[:SR * 6]
    put(d, "ms__default", ms(mid_, side_))
    put(d, "ms__MID_off", ms(0 * a, side_))
    put(d, "ms__SIDE_off", ms(mid_, 0 * b))
    put(d, "ms__WIDTH_0", ms(mid_, 0 * b))
    put(d, "ms__MONO", ms(mid_, 0 * b))
    rc, out = run_suite(d, "ms")
    check("suite ms good: exit 0", rc == 0, out.strip().splitlines()[-1])
    put(d, "ms__SIDE_off", ms(mid_, side_))
    rc, out = run_suite(d, "ms")
    check("suite ms SIDE not off: FAIL", rc == 1, f"rc {rc}")

with tempfile.TemporaryDirectory() as d:
    mid_ = noise(seed=41)
    good = np.stack([mid_ + 0.1 * noise(seed=42), mid_ - 0.1 * noise(seed=42)], axis=1)[:SR * 8]
    anti = np.stack([mid_, -mid_], axis=1)[:SR * 8]
    put(d, "stereo__F006__STEADY_RAIN", good)
    put(d, "stereo__F013__FULL_STORM", anti)
    rc, out = run_suite(d, "stereo")
    check("suite stereo: anti-phase preset FAILs, good passes", rc == 1 and out.count("FAIL") == 2, f"rc {rc}")

with tempfile.TemporaryDirectory() as d:
    live = stereo()[:SR * 15]
    dead = np.concatenate([live[:SR * 5], np.zeros((SR * 10, 2))])
    for li in range(1, 5):
        put(d, f"activity__F006__STEADY_RAIN__lane{li}", live)
        put(d, f"activity__F013__FULL_STORM__lane{li}", live)
    put(d, "activity__F006__STEADY_RAIN__lane2", dead)
    rc, out = run_suite(d, "activity", "15")
    check("suite activity: one lane goes silent -> 1 FAIL", rc == 1 and "lane 2" in [l for l in out.splitlines() if l.startswith("FAIL")][0], f"rc {rc}")

# a missing directory content: skip, not a failure
with tempfile.TemporaryDirectory() as d:
    rc, out = run_suite(d, "clicks")
    check("suite: no recordings -> skips, exit 0", rc == 0 and "skip" in out)

print(f"{'FAIL' if fails else 'ok'}: {fails} failure(s)")
sys.exit(1 if fails else 0)
