#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""tools/hil_suite.py's analysis on synthetic signals, and the suite offline (--wav) on synthetic recordings.
No hardware: python3 -I tests/hil_suite_check.py (needs numpy). With build/fw/textura-host (make fw-host) the glitch
detectors are also run on the host's render of FULL STORM (real crackle and thunder). Exit code = failures."""
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


def q16(x):
    return np.round(np.clip(x, -1, 1 - 1 / 32768) * 32768) / 32768


def crackle(n=SR * 8, seed=3):
    """storm-like stereo: noise bed plus sharp random pops (each a decaying burst), 16-bit as the USB sends it"""
    r = np.random.RandomState(seed)
    bed = stereo(seed) * 0.4
    for t in r.randint(100, n - 400, 3000):
        a = r.uniform(0.05, 0.4) * r.choice([-1, 1])
        k = np.arange(300)
        bed[t:t + 300, r.randint(2)] += a * np.exp(-k / 25.0) * np.cos(k * r.uniform(0.3, 1.5))
    return q16(bed[:n])


def jack_of(u, delay=2800.0, drift=541e-6, gain=10 ** (-12.6 / 20), seed=9):
    """the jack's recording of a USB signal: an inverting amp with a coupling high-pass (10 Hz) and a gentle top
    (one-pole at 16 kHz), the sides swapped, at 48 kHz on the M6's clock (drift), later by `delay` USB samples,
    a noise floor at -100 dBFS"""
    x = np.asarray(u, dtype=np.float64)
    a = 1 - np.exp(-2 * np.pi * 16000 / SR)
    hp = np.exp(-2 * np.pi * 10 / SR)
    y = np.zeros_like(x)
    lp = np.zeros(2)
    hs = np.zeros(2)
    prev = np.zeros(2)
    for i in range(len(x)):          # (plain loops: 8 s of audio, fine)
        lp += a * (x[i] - lp)
        hs = hp * (hs + lp - prev)
        prev = lp.copy()
        y[i] = hs
    y = -gain * y[:, ::-1]
    n = int(len(x) * 48000 / SR) - 4000
    pos = (np.arange(n) * SR / 48000) * (1 + drift) - delay
    pos = np.clip(pos, 0, len(x) - 1)
    j = np.stack([H._interp(y[:, c], pos) for c in range(2)], 1)
    return j + np.random.RandomState(seed).randn(*j.shape) * 1e-5


# ---- the crackle counter, kept (it counts; it no longer judges)
x = stereo()
c = H.analyse_clicks(x, SR)
check("clean: no clicks", not c["clicks"], f"{len(c['clicks'])}")
check("clean: no repeats", not c["repeats"], f"{len(c['repeats'])}")
check("clean: no dropouts", not c["drops"], f"{len(c['drops'])}")
y = x.copy()
for pos in (30000, 100000, 250000):
    y[pos:, 0] += 0.2
c = H.analyse_clicks(y, SR)
check("clicks: 3 injected found", len(c["clicks"]) >= 3, f"{len(c['clicks'])}")
y = x.copy()
y[120000:120256] = 0
c = H.analyse_clicks(y, SR)
check("zeroed block: dropout", any(abs(i - 120000) < 300 for i in c["drops"]), f"{c['drops'][:5]}")
t = 0.2 * np.sin(2 * np.pi * SR / 128 * np.arange(SR * 4) / SR)
rep, n, tonal = H.find_repeats(t)
check("tone: skipped as tonal", tonal and not rep)
g = np.concatenate([noise(SR * 3, 3), np.zeros(SR * 2), noise(SR * 3, 4)])
check("long silence: no dropout flagged", not H.find_dropouts(g), f"{H.find_dropouts(g)[:3]}")

# ---- USB digital artefacts (the unit's own 16-bit samples)
u = crackle()
d = H.usb_digital(u)
check("usb clean crackle: no repeats, no zero gaps, no grid", not d["repeats"] and not d["zeros"] and d["grid"][3] > H.GRID_P,
      f"{d['repeats'][:2]} {d['zeros'][:2]} grid {d['grid']}")
for lag in (128, 256):
    y = u.copy()
    y[150000 + lag:150000 + 2 * lag] = y[150000:150000 + lag]
    d = H.usb_digital(y)
    check(f"usb repeated {lag} samples: found", any(abs(i - 150000 - lag) <= 2 and l == lag for i, l in d["repeats"]), f"{d['repeats'][:3]}")
for ln in (128, 256):
    y = u.copy()
    y[200000:200000 + ln] = 0
    d = H.usb_digital(y)
    check(f"usb zeroed {ln} samples: found", any(abs(i - 200000) < 8 for i, l in d["zeros"]), f"{d['zeros'][:3]}")
y = u.copy()
y[200000:200300] = 0
check("usb zero gap of 300 (not a block): not flagged", not H.usb_digital(y)["zeros"])
y = np.concatenate([u[:SR * 3], np.zeros((SR, 2)), u[SR * 4:]])
check("usb sound that stops and starts: not flagged", not H.usb_digital(y)["zeros"])
y = stereo(5) * 0.5
for b in range(20, 1300, 37):
    y[b * 256 + 77:, 0] += 0.2 * (1 if b % 2 else -1)      # steps at one phase of the block grid
d = H.usb_digital(q16(y))
check("steps on the block grid: p tiny", d["grid"][3] < 1e-6 and d["grid"][2] in (76, 77), f"{d['grid']}")
d = H.usb_digital(q16(stereo(5) * 0.5))
check("no steps: grid p not small", d["grid"][3] > 1e-3, f"{d['grid']}")

# ---- the jack against the USB (aligned, the analog path fitted)
j = jack_of(u)
r = H.jack_vs_usb(u, SR, j, 48000)
check("jack vs usb clean: aligned, fitted, no glitch", r and r["ok"] and r["swap"] and abs(r["drift_ppm"] + 541) < 5,
      f"{r and {k: r[k] for k in ('model_db', 'worst', 'drift_ppm', 'swap', 'glitches')}}")
jz = j.copy()
jz[200000:200279] = 0
r = H.jack_vs_usb(u, SR, jz, 48000)
check("jack zeroed block: a glitch", r and any(abs(t - (200000 * SR / 48000 - 2800) / SR) < 0.02 for t, s in r["glitches"]),
      f"{r and r['glitches'][:3]}")
jr = j.copy()
jr[250000 + 139:250000 + 278] = jr[250000:250000 + 139]
r = H.jack_vs_usb(u, SR, jr, 48000)
check("jack repeated half block: a glitch", r and r["glitches"], f"{r and r['glitches'][:3]}")
jd = np.concatenate([j[:180000], j[180279:]])
r = H.jack_vs_usb(u, SR, jd, 48000)
check("jack dropped block (a slip): not ok (a step, or no one offset)", r and r["ok"] is False and r["steps"], f"{r and r['steps']}")

# ---- the verdict on a capture
f, notes, num = H.judge_clicks({"usb": (u, SR), "jack": (j, 48000)})
check("judge clean crackle: no failure", not f, f"{f}")
y = u.copy()
y[300000 + 256:300000 + 512] = y[300000:300000 + 256]
f, notes, num = H.judge_clicks({"usb": (y, SR), "jack": (j, 48000)})
check("judge usb repeated block: FAIL", any("repeated" in t for t in f), f"{f}")

# ---- the same on the host's render of FULL STORM (real crackle and thunder), when the host is built
host = H.Host()
storm = None
if host.bin:
    allp = H.parse_presets()
    p = [q for q in allp if q["name"] == "FULL STORM"][0]
    h = host.render(p, 8.0, settle=2.5)
    storm = q16(h[0]) if h else None
if storm is None:
    print("skip  host render of FULL STORM (make fw-host)")
else:
    js = jack_of(storm)
    f, notes, num = H.judge_clicks({"usb": (storm, SR), "jack": (js, 48000)})
    check("storm render: crackle is no glitch", not f, f"{f} crackle {num.get('crackle', 0):.0f}/10 s, "
          f"old counter {len(H.find_clicks(storm))}")
    cases = []
    y = storm.copy(); y[150000 + 128:150000 + 256] = y[150000:150000 + 128]; cases.append(("usb repeated half", y, js))
    y = storm.copy(); y[170000:170256] = 0; cases.append(("usb zeroed block", y, js))
    jj = js.copy(); jj[220000:220279] = 0; cases.append(("jack zeroed block", storm, jj))
    jj = js.copy(); jj[230000 + 139:230000 + 278] = jj[230000:230000 + 139]; cases.append(("jack repeated half", storm, jj))
    jj = np.concatenate([js[:200000], js[200279:]]); cases.append(("jack dropped block", storm, jj))
    for name, uu, jj in cases:
        f, notes, num = H.judge_clicks({"usb": (uu, SR), "jack": (jj, 48000)})
        check(f"storm render, {name}: FAIL", bool(f), f"{f[:1]}")

# ---- lag and coherence
l = noise(seed=2)
for lag in (25, 29, 33):
    r = np.concatenate([np.zeros(lag), l[:-lag]])
    check(f"lag {lag}", H.lag_of(l, r) == lag, f"{H.lag_of(l, r)}")
    check(f"lag in the lows {lag}", abs(H.lag_lo(l, r, SR) - lag) < 0.5, f"{H.lag_lo(l, r, SR):.2f}")
c = 1 - np.exp(-2 * np.pi * 380 / SR)
lo = l.copy()
for _ in range(2):
    acc, out_ = 0.0, np.empty_like(lo)
    for i, v in enumerate(lo):
        acc += c * (v - acc); out_[i] = acc
    lo = out_
r = l - lo + np.concatenate([np.zeros(29), lo[:-29]])
check("lag in the lows, the highs on time: 29", abs(H.lag_lo(l, r, SR) - 29) <= 2 and H.lag_of(l, r) == 0,
      f"lows {H.lag_lo(l, r, SR):.2f}, broadband {H.lag_of(l, r)}")
check("coherence: delayed copy ~1, independent noise low",
      H.coherence_lo(l, np.concatenate([np.zeros(29), l[:-29]]), SR) > 0.9 and H.coherence_lo(l, noise(seed=99), SR) < 0.3,
      f"{H.coherence_lo(l, np.concatenate([np.zeros(29), l[:-29]]), SR):.2f} {H.coherence_lo(l, noise(seed=99), SR):.2f}")

# ---- stereo stats, the preset's expected L-R, the host's span
m = noise(seed=3)
s = H.stereo_stats(np.stack([m, m], axis=1))
check("mono: side gone, corr 1", s["side"] < s["mid"] - 60 and s["corr"] > 0.999, f"{s}")
s = H.stereo_stats(np.stack([m, -m], axis=1))
check("anti-phase: mid gone, corr -1", s["mid"] < s["side"] - 60 and s["corr"] < -0.999, f"{s}")
lane = lambda p, r="F", n=1: dict(n=n, a=1, v=100, p=p, r=r, muted=False)
check("expected L-R: centre 0, hard left one lane None, two lanes", abs(H.expected_lr(dict(lanes=[lane(64)] + [None] * 3))) < 0.2
      and H.expected_lr(dict(lanes=[lane(0)] + [None] * 3)) is None
      and abs(H.expected_lr(dict(lanes=[lane(0), lane(127, "M"), None, None])) - 6.0) < 0.01)
hx = np.stack([m * 1.0, m * 0.8], axis=1)
span = H.host_lr_span((hx, SR), 2.0)
check("host L-R span", span and abs(span[0] - 1.94) < 0.05 and abs(span[1] - 1.94) < 0.05, f"{span}")
check("L-R verdict: in span, out of span, no host", H.lr_verdict(2.5, 0.0, (1.0, 2.0))[0] and not H.lr_verdict(4.0, 0.0, (1.0, 2.0))[0]
      and H.lr_verdict(2.5, 0.0, None)[0] and not H.lr_verdict(3.5, 0.0, None)[0])

# ---- the M/S alternation
seg = int(H.MS_SEG * SR)
a, b = noise(SR * 30, 31), noise(SR * 30, 32)
n = 2 * H.MS_PAIRS * seg + SR // 5
mono_on = np.zeros(n)
for i in range(1, 2 * H.MS_PAIRS, 2):
    mono_on[i * seg:(i + 1) * seg] = 1
alt = np.stack([a[:n] + 0.4 * b[:n] * (1 - mono_on), a[:n] - 0.4 * b[:n] * (1 - mono_on)], axis=1)
off, on = H.ms_segments(alt, SR)
good, txt = H.ms_verdict("MONO", off, on)
check("ms alternation: MONO keeps MID, SIDE gone", good and len(off) == H.MS_PAIRS, txt)
drop = np.where(mono_on[:, None] > 0, alt * 0.4, alt)
good, txt = H.ms_verdict("MONO", *H.ms_segments(drop, SR))
check("ms alternation: MID dropping 8 dB with the case: FAIL", not good, txt)

# ---- activity against the host
check("activity: silent, host never rests -> dead", H.activity_verdict([-40, -80, -45], 0.0, True)[0])
check("activity: silent, host rests too -> alive", not H.activity_verdict([-40, -80, -45], 0.2, True)[0])
check("activity: not meant to play -> alive", not H.activity_verdict([-240, -240, -240], None, False)[0])

# ---- wav, presets
with tempfile.TemporaryDirectory() as dd:
    pth = os.path.join(dd, "a.wav")
    H.write_wav(pth, x, SR)
    back, sr = H.read_wav(pth)
    check("wav round trip", sr == SR and back.shape == x.shape and np.max(np.abs(back - x)) < 2e-7)
allp = H.parse_presets()
check("presets parsed, pans and places", len(allp) >= 100 and all(len(p["lanes"]) == 4 for p in allp)
      and allp[0]["lanes"][1]["p"] == 40 and allp[0]["lanes"][2]["r"] == "B", f"{len(allp)}")
check("program numbers: F100 99, F101 116", H.prog(allp[99]) == 99 and H.prog(allp[100]) == 116)
check("--presets all / ranges", len(H.pick_presets(allp, "all")) == len(allp) and len(H.pick_presets(allp, "3-12")) == 10)
dp = H.default_presets(allp)
check("default presets: 10, busy", len(dp) == 10 and min(H.density(p) for p in dp) > 0, f"{[p['name'] for p in dp]}")


# ---- the suite offline on synthetic recordings (no host comparisons: their numbers would depend on the build)
def run_suite(wavdir, only, secs="6"):
    out = io.StringIO()
    with contextlib.redirect_stdout(out):
        rc = H.main(["--wav", wavdir, "--only", only, "--presets", "STEADY RAIN,FULL STORM", "--seconds", secs, "--no-host"])
    return rc, out.getvalue()


def put(d, key, usb=None, jack=None):
    if usb is not None:
        H.write_wav(os.path.join(d, f"{key}__usb.wav"), usb, SR)
    if jack is not None:
        H.write_wav(os.path.join(d, f"{key}__jack.wav"), jack, 48000)


with tempfile.TemporaryDirectory() as d:
    for p in ("F006__STEADY_RAIN", "F013__FULL_STORM"):
        for mode in ("SPEAKERS", "HEADPHONES"):
            put(d, f"clicks__{p}__{mode}", u, j)
    rc, out = run_suite(d, "clicks")
    check("suite clicks clean: exit 0", rc == 0, out.strip().splitlines()[-1])
    put(d, "clicks__F006__STEADY_RAIN__SPEAKERS", u, jz)
    rc, out = run_suite(d, "clicks")
    check("suite: jack glitch -> 1 FAIL", rc == 1 and "jack vs USB glitch" in out, f"rc {rc}")
    rp = u.copy()
    rp[100000 + 256:100000 + 512] = rp[100000:100000 + 256]
    put(d, "clicks__F006__STEADY_RAIN__SPEAKERS", rp, j)
    rc, out = run_suite(d, "clicks")
    check("suite: USB repeated block -> FAIL", rc >= 1 and "USB block repeated" in out, f"rc {rc}")

with tempfile.TemporaryDirectory() as d:
    base = noise(seed=21)
    left = lambda lag, far: np.stack([base, far * np.concatenate([np.zeros(lag), base[:len(base) - lag]])], axis=1)
    sec = 6
    cut = lambda a: a[:SR * sec]
    leak = lambda: np.stack([base, 10 ** (-34 / 20) * noise(seed=77)], axis=1)     # far ear: reverb-like, incoherent
    put(d, "binaural__SPEAKERS", cut(leak()))
    put(d, "binaural__HEADPHONES_SMALL", cut(left(25, 10 ** (-14 / 20))))
    put(d, "binaural__HEADPHONES_MEDIUM", cut(left(29, 10 ** (-14 / 20))))
    put(d, "binaural__HEADPHONES_LARGE", cut(left(33, 10 ** (-14 / 20))))
    put(d, "binaural__HEADPHONES_AMOUNT_0", cut(leak()))
    rc, out = run_suite(d, "binaural")
    check("suite binaural good (AMOUNT 0: an incoherent far ear, no lag to measure): exit 0", rc == 0,
          [ln for ln in out.splitlines() if "AMOUNT 0" in ln][:1])
    put(d, "binaural__HEADPHONES_LARGE", cut(left(29, 10 ** (-14 / 20))))     # wrong lag
    put(d, "binaural__HEADPHONES_AMOUNT_0", cut(left(29, 10 ** (-14 / 20))))  # amount 0 still binaural
    rc, out = run_suite(d, "binaural")
    check("suite binaural bad: 2 FAIL", rc == 2, f"rc {rc}")

with tempfile.TemporaryDirectory() as d:
    a, b = noise(SR * 30, 31), noise(SR * 30, 32)
    case = lambda mid_on, side_on: np.stack([a[:n] * mid_on + 0.4 * b[:n] * side_on, a[:n] * mid_on - 0.4 * b[:n] * side_on], axis=1)
    put(d, "ms__MID_off", case(1 - mono_on, 1))
    put(d, "ms__SIDE_off", case(1, 1 - mono_on))
    put(d, "ms__WIDTH_0", case(1, 1 - mono_on))
    put(d, "ms__MONO", case(1, 1 - mono_on))
    rc, out = run_suite(d, "ms")
    check("suite ms good: exit 0", rc == 0, out.strip().splitlines()[-1])
    put(d, "ms__SIDE_off", case(1, 1))
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
    check("suite activity: one lane goes silent (no host to compare) -> 1 FAIL",
          rc == 1 and "lane 2" in [ln for ln in out.splitlines() if ln.startswith("FAIL")][0], f"rc {rc}")

with tempfile.TemporaryDirectory() as d:
    rc, out = run_suite(d, "clicks")
    check("suite: no recordings -> skips, exit 0", rc == 0 and "skip" in out)

print(f"{'FAIL' if fails else 'ok'}: {fails} failure(s)")
sys.exit(1 if fails else 0)
