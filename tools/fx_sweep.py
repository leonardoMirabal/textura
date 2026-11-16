#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""fx_sweep: every effect control turned 0 -> top -> 0 at a human rate (a step every 20 ms) and moved once (its ends
both ways and one detent at the middle; a selector: every neighbouring pair both ways) under a ringing chord, against
renders of the same control held (Frodhes' tools/control_sweep.py checks, on our effects; bench/txfxsweep.c renders).

    make tx-fx-sweep                      (or: tools/fx_sweep.py BIN [NAME ...])

  click   the largest |second difference| from 3 ms before to 30 ms after the move, against the larger of the two
          held renders' (and, for a control that glides through values, the held values between): over 10x flags
  trans   20 ms windows over 300 ms after the move outside the held renders' range (3 dB over the louder, 6 dB under
          the quieter; the wet alone on the bus)
  zipper  the sweep's largest |second difference| per 1024 samples against the held renders' there: over 2x flags
  jump    a level step between two 1024-sample windows of the sweep beyond the held renders' own: over 1.5 dB flags
  stuck   after the sweep, back at 0: 300 ms against the render held at 0, over 1.5 dB flags
KNOWN: the flags understood (each with its reason); any other flag fails (exit 1). Renders are cached by the binary's
hash (build/fxsweep/)."""
import hashlib, os, subprocess, sys
from concurrent.futures import ThreadPoolExecutor
import numpy as np

FS = 44100
T_FLIP, T_SW0, STEP = 2000, 1000, 20
LIM = {"click": 10.0, "trans_hi": 3.0, "trans_lo": 6.0, "zipper": 2.0, "jump": 1.5, "stuck": 1.5}
BIN = sys.argv[1] if len(sys.argv) > 1 else "build/bench/txfxsweep"
# every control from the binary's own tables (GROUP ID MAX DEF NAME): the cases follow the effects as they change
PAR = {}
for line in subprocess.run([BIN, "list"], capture_output=True, text=True, check=True).stdout.splitlines():
    g, i, mx, df, name = line.split(" ", 4)
    PAR[(g, name)] = (int(i), int(mx))
# (name, group, pid, setup, max, selector, through)
CASES = []
def add(name, g, pn, setup, sel=False, through=False):
    pid, mx = PAR[(g, pn)]
    CASES.append((name, g, pid, setup, mx, sel, through))
def pid(g, pn):
    return PAR[(g, pn)][0]
D0 = f"d{pid('d', 'AMOUNT')}=64"
add("drive AMOUNT", "d", "AMOUNT", "")
add("drive TONE", "d", "TONE", D0)
add("drive MIX", "d", "MIX", D0, through=True)   # (MIX passes its law's middle: both whole there)
add("drive TYPE", "d", "TYPE", D0, sel=True)
for t, n in [(1, "KHLON"), (2, "TSH8"), (3, "OHCD"), (4, "BHLUE")]:
    add(f"drive {n} AMOUNT", "d", "AMOUNT", f"d{pid('d', 'TYPE')}={t}")
LP, HP, RS = pid("f", "LOW PASS"), pid("f", "HIGH PASS"), pid("f", "RESONANCE")
add("filter HIGH PASS", "f", "HIGH PASS", "", through=True)
add("filter LOW PASS", "f", "LOW PASS", "", through=True)
add("filter HIGH PASS RESO", "f", "HIGH PASS", f"f{RS}=90", through=True)
add("filter LOW PASS RESO", "f", "LOW PASS", f"f{RS}=90", through=True)
add("filter RESONANCE", "f", "RESONANCE", f"f{LP}=64")
add("filter SLOPE", "f", "SLOPE", f"f{LP}=64,f{RS}=60", sel=True)
G0 = f"g{pid('g', 'MIX')}=64"
for (g, n), (i, mx) in sorted(PAR.items(), key=lambda t: t[1][0]):
    if g != "g":
        continue
    setup = "" if n == "MIX" else G0 + (f",g{pid('g', 'MOD')}=64" if n == "INTERVALS" else "")
    add(f"grain {n}", "g", n, setup, sel=n in ("FREEZE", "INTERVALS"))
RT = pid("r", "TYPE")
RM = f"r{pid('r', 'MIX')}=64"
add("reverb TYPE", "r", "TYPE", RM, sel=True)
for t, tn in [(1, "ROOM"), (2, "HALL"), (3, "INF"), (7, "PLATE")]:
    add(f"reverb MIX {tn}", "r", "MIX", f"r{RT}={t}")
    add(f"reverb MOD {tn}", "r", "MOD", f"r{RT}={t},{RM}")
for (g, n), (i, mx) in sorted(PAR.items(), key=lambda t: t[1][0]):
    if g != "r" or n in ("TYPE", "MIX", "MOD"):
        continue
    setup = f"r{RT}=2,{RM}" + (f",r{pid('r', 'SHIMMER')}=64" if n == "SHIFT" else "")
    add(f"reverb {n}", "r", n, setup, sel=n == "SHIFT")

KNOWN = {   # (case, check): why it is not a bug of the control
    ("reverb TYPE", "trans"): "a new TYPE starts empty after the old one's fade (SWELL ducks while played)",
    ("grain FREEZE", "trans"): "FREEZE loops the line's last 1.45 s: the echo then replays the chord as it was then, "
                               "louder than now (+5 dB at the default TIME, 220 ms after the freeze); no click",
    ("reverb PREDELAY", "trans"): "a time, as delay TIME: the tail rings on with the send shifted, so held tones "
                                  "interfere (one step, 63 -> 64, does it too); no click, the move crossfades in 46 ms",
}
CACHE = os.path.join("build", "fxsweep")
os.makedirs(CACHE, exist_ok=True)
BH = hashlib.sha1(open(BIN, "rb").read()).hexdigest()[:12]

def render(g, pid, setup, mode, a, b):
    key = hashlib.sha1(f"{BH} {g} {pid} {setup} {mode} {a} {b}".encode()).hexdigest()[:20]
    f = os.path.join(CACHE, key + ".bin")
    if not os.path.exists(f):
        subprocess.run([BIN, g, str(pid), setup, mode, str(a), str(b), f + ".tmp"], check=True)
        os.replace(f + ".tmp", f)
    x = np.fromfile(f, dtype=np.int32).reshape(-1, 2).astype(np.float64)
    return x

def d2max(x, s, e):
    y = x[s:e]
    return np.max(np.abs(y[2:] - 2 * y[1:-1] + y[:-2]))

def lvl(x, s, e):
    y = x[s:e]
    return 10 * np.log10(np.mean(y ** 2) + 1e-9)

DRY = None
def wet(g, x):
    if g in "gr":
        n = min(len(x), len(DRY))
        return x[:n] - DRY[:n]
    return x

def case(c):
    name, g, pid, setup, mx, sel, through = c
    out = {}
    # flips
    pairs = [(i, i + 1) for i in range(mx)] if sel else [(0, mx), (mx // 2, mx // 2 + 1)]
    worst_click, worst_trans, wpair = 0.0, 0.0, None
    s0, s1 = int((T_FLIP - 3) * FS / 1000), int((T_FLIP + 30) * FS / 1000)
    for a, b in pairs:
        for x0, x1 in ((a, b), (b, a)):
            mv = render(g, pid, setup, "f", x0, x1)
            h0 = render(g, pid, setup, "s", x0, 0)
            h1 = render(g, pid, setup, "s", x1, 0)
            ref = max(d2max(h0, s0, s1), d2max(h1, s0, s1), 1.0)
            if through:                                  # (a glide passes the values between: their statics too)
                ref = max([ref] + [d2max(render(g, pid, setup, "s", v, 0), s0, s1) for v in (32, 64, 96)
                                   if min(x0, x1) < v < max(x0, x1)])
            r = d2max(mv, s0, s1) / ref
            if r > worst_click:
                worst_click, wpair = r, (x0, x1)
            # trans: 20 ms windows over 300 ms after the move, the wet alone on the bus
            wm, w0, w1 = wet(g, mv), wet(g, h0), wet(g, h1)
            extra = []
            if through:
                lo, hi = min(x0, x1), max(x0, x1)
                extra = [wet(g, render(g, pid, setup, "s", v, 0)) for v in (0, 32, 64, 96, 127) if lo < v < hi]
            for k in range(15):
                s = int((T_FLIP + 20 * k) * FS / 1000); e = s + int(0.02 * FS)
                L = [lvl(w0, s, e), lvl(w1, s, e)] + ([lvl(z, s, e) for z in extra] if k < 3 else [])
                v = lvl(wm, s, e)
                if max(L) < -80 * 1 + 10 * np.log10(2 ** 46):   # (a level in Q23^2: under -80 dBFS)
                    continue
                d = max(v - max(L) - LIM["trans_hi"], min(L) - v - LIM["trans_lo"], 0.0)
                worst_trans = max(worst_trans, d)
    out["click"] = (worst_click, worst_click > LIM["click"], wpair)
    out["trans"] = (worst_trans, worst_trans > 0)
    if not sel:
        sw = render(g, pid, setup, "w", 0, 0)
        stat = [render(g, pid, setup, "s", v, 0) for v in sorted(set([0, 32, 64, 96, mx]))]
        n = min(len(sw), min(len(s) for s in stat))
        e_sw = T_SW0 * FS // 1000 + int(2 * mx * STEP * FS / 1000)
        zr, jr = 0.0, 0.0
        prev = None
        for s in range(T_SW0 * FS // 1000, min(e_sw, n - 1024), 1024):
            z = d2max(sw, s, s + 1024) / max(max(d2max(st, s, s + 1024) for st in stat), 1.0)
            zr = max(zr, z)
            cur = (lvl(sw, s, s + 1024), [lvl(st, s, s + 1024) for st in stat])
            if prev is not None and cur[0] > -100 + 10 * np.log10(2 ** 46):
                jst = max(abs(a - b) for a, b in zip(cur[1], prev[1]))
                jr = max(jr, abs(cur[0] - prev[0]) - jst)
            prev = cur
        out["zipper"] = (zr, zr > LIM["zipper"])
        out["jump"] = (jr, jr > LIM["jump"])
        s = e_sw + int(0.5 * FS); e = s + int(0.3 * FS)
        e = min(e, n)
        st = abs(lvl(wet(g, sw), s, e) - lvl(wet(g, stat[0]), s, e)) if e - s > 1000 else 0.0
        out["stuck"] = (st, st > LIM["stuck"])
    return name, out

def main():
    global DRY
    DRY = render("g", pid("g", "MIX"), "", "s", 0, 0)   # (GRAIN DELAY and REVERB off: the dry chain)
    sel = [c for c in CASES if len(sys.argv) < 3 or any(a in c[0] for a in sys.argv[2:])]
    with ThreadPoolExecutor(8) as ex:
        res = list(ex.map(case, sel))
    flags = known = 0
    for name, out in res:
        cols, bad, kn = [], [], []
        for k in ("click", "trans", "zipper", "jump", "stuck"):
            if k in out:
                v = out[k]
                cols.append(f"{k} {v[0]:6.2f}{'!' if v[1] else ' '}")
                if v[1]:
                    (kn if (name, k) in KNOWN else bad).append(k + (f" {v[2][0]}->{v[2][1]}" if k == "click" else ""))
        flags += bool(bad)
        known += bool(kn) and not bad
        tail = ("   FLAG " + ", ".join(bad) if bad else "   OK") + ("   (known: " + ", ".join(kn) + ")" if kn else "")
        print(f"{name:24s} " + "  ".join(cols) + tail)
    print(f"{len(res)} cases: {flags} flagged, {known} with known flags only")
    return 1 if flags else 0

sys.exit(main())
