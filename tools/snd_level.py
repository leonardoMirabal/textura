#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Every sound's loudness, measured: each sound of the browser (firmware/src/landscapes.h) played alone on the
firmware's host build, lane 1 at LEVEL 100 with its trim (LEVEL_DB), at HOW MANY x HOW ACTIVE keys, dry; the loudness
of each (tools/loudness.py: BS.1770 integrated, gated, so a sparse sound counts as loud as its events are).

Each grid point is a factory-preset-like table (gen_presets.header) of one-sound "presets", loaded by MIDI PC on
host builds that take it as PR_TABLE (the engines compiled once). A sound's new trim brings its loudness at the
everyday keys (REF: HOW MANY 1 .. 3 x HOW ACTIVE rare .. middle, where a sound starts and the presets play; their
median) to TARGET LUFS (-28: the presets' foreground), but no further than its loudest 1 % of 50 ms peaks there (the
median over REF) at PEAK_MAX dBFS (under the limiter: the crackling and dripping
sounds stay under the target and keep their crest).

Across the keys a sound's own loudness may move 40 dB and more (a calm .. a gale). KEY_DB (audition/app.js; the
firmware's LS_KC) bends it into a common window: a gain at HOW MANY 1, 3, 6 x HOW ACTIVE rare, middle, busy
(bilinear between them, scape.c), so that what it rises above the everyday keys is squeezed into WIN_UP dB and
what it falls below them into WIN_DOWN dB: the top keys still sound bigger, never 20 dB over the other lanes;
the physics (density, timbre) still change. A boost stays under PEAK_MAX too. The bus COMPRESSOR is not linear:
run --write until nothing moves.

  .local/venv/bin/python tools/snd_level.py [-j JOBS] [-s SECS] [-t TARGET] [-o DIR] [--grid N:A,...] [--only F:name]
      prints a table (one row per sound: its trim, its loudness at each grid point, the range, the new trim) and
      writes DIR/snd_level.json; --write sets the new trims in audition/app.js LEVEL_DB and the curves in KEY_DB
      (then: make fw-landscapes)"""
import argparse
import glob
import json
import math
import re
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import gen_presets                              # noqa: E402
import loudness                                 # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
APP = ROOT / "audition" / "app.js"
DEF = (2, 4)                                    # the default keys: HOW MANY 2, HOW ACTIVE 4 (LEVEL_DB's old rule)
REF = [(1, 0), (1, 4), (2, 4), (3, 0), (3, 4)]  # the everyday keys: a sound starts at HOW MANY 1, rare; presets play here
NODES = [(1, 0), (1, 4), (1, 8), (3, 0), (3, 4), (3, 8), (6, 0), (6, 4), (6, 8)]   # KEY_DB's points (scape.c sc_kc)
GRID = NODES[:3] + [(2, 4)] + NODES[3:]
WIN_UP, WIN_DOWN = 6.0, 10.0                    # dB above / below the everyday keys' loudness: -22 .. -38 LUFS
KC_LO, KC_HI = -31.0, 12.0                      # dB: KEY_DB's range (LS_KC: int8 half dB)
T_LOAD = 1500                                   # ms: the PC
T_FROM = 1000                                   # ms after the load: the measurement (the load's glide is over)
PEAK_MAX = -2.0                                 # dBFS: the everyday keys' loudest 1 % of 50 ms peaks (the limiter: -1)
TRIM_LO, TRIM_HI = -24, 20                      # dB: LEVEL_DB's range
CHUNK = 100                                     # presets per host build (MIDI PC 0 .. 99)


def families():
    """[(family name, [sound names])] from landscapes.h (LS order)"""
    t = (ROOT / "firmware" / "src" / "landscapes.h").read_text()
    out = []
    for m in re.finditer(r'^\s*\{"([^"]+)", \d+, \{(.*)\}\},\s*$', t, re.M):
        out.append((m.group(1), re.findall(r'\{"([^"]+)", "[^"]+", \d+, \d, \d+\}', m.group(2))))
    return out


def level_db():
    import ast
    s = APP.read_text()
    i = s.index("const LEVEL_DB = {")
    j = s.index("\n};", i)
    return ast.literal_eval("{" + s[i + len("const LEVEL_DB = {"):j] + "}")


def key_db():
    import ast
    s = APP.read_text()
    i = s.index("const KEY_DB = {")
    j = s.index("\n};", i)
    return ast.literal_eval("{" + s[i + len("const KEY_DB = {"):j] + "}")


def kc_at(c, g):
    """KEY_DB's gain at keys g = (n, a), bilinear between its nodes (as scape.c, in the key index)"""
    n, a = g
    i, fn = (0, (n - 1) / 2) if n <= 3 else (1, (n - 3) / 3)
    j, fa = (0, a / 4) if a <= 4 else (1, (a - 4) / 4)
    v = lambda ii, jj: c[ii * 3 + jj]
    return ((1 - fn) * ((1 - fa) * v(i, j) + fa * v(i, j + 1)) + fn * ((1 - fa) * v(i + 1, j) + fa * v(i + 1, j + 1)))


def build(jobs, od, cc, objs):
    """host builds, CHUNK jobs each (job: (fam, snd, n, a)); [(path, [jobs])]"""
    out, procs = [], []
    for c in range(0, len(jobs), CHUNK):
        part = jobs[c:c + CHUNK]
        ps = [{"name": f"S{k:03d}", "fx": 0,
               "lanes": [{"fam": f, "snd": s, "n": n, "a": a, "v": 100, "p": 64, "m": 0}, None, None, None]}
              for k, (f, s, n, a) in enumerate(part)]
        h = od / f"chunk{c // CHUNK}.h"
        h.write_text(gen_presets.header([("SND", ps)]))
        exe = od / f"host{c // CHUNK}"
        cmd = ["nice", "-n", "10", cc, "-O2", "-w", "-DDINGHY_HOST", "-DAPP_VER=\"dev\"", f"-DPR_TABLE=\"{h}\"",
               "-Ifirmware/src", "-Ifirmware/gen", "-Iengines", "-o", str(exe), "firmware/src/textura.c"] + objs + ["-lm"]
        procs.append(subprocess.Popen(cmd, cwd=ROOT))
        out.append((exe, part))
    if any(p.wait() for p in procs):
        sys.exit("snd_level: a host build failed")
    return out


def engines(od, cc, j):
    """the engines compiled once: [object paths]"""
    srcs = sorted(glob.glob(str(ROOT / "engines" / "*.c")))
    (od / "o").mkdir(parents=True, exist_ok=True)

    def one(s):
        o = od / "o" / (Path(s).stem + ".o")
        subprocess.run(["nice", "-n", "10", cc, "-O2", "-w", "-DDINGHY_HOST", "-Iengines", "-c", s, "-o", str(o)],
                       cwd=ROOT, check=True)
        return str(o)
    with ThreadPoolExecutor(j) as ex:
        return list(ex.map(one, srcs))


def run(exe, i, secs, wav):
    """preset i of host exe: (I, S95, peak) from T_FROM after the load, secs long"""
    scr = wav.with_suffix(".txt")
    scr.write_text(f"{T_LOAD} midi 0xC0 {i} 0\n{T_LOAD + T_FROM + secs * 1000} end\n")
    subprocess.run(["nice", "-n", "10", str(exe), str(wav.with_suffix("")), str(scr)], check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    x = loudness.read(wav)
    x = x[int((T_LOAD + T_FROM) / 1000 * loudness.FS):]
    r = loudness.measure(x)
    scr.unlink()
    wav.unlink()
    return r + (loudness.p99(x),)



def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-j", type=int, default=4)
    ap.add_argument("-s", type=int, default=30)
    ap.add_argument("-t", type=float, default=-28.0)
    ap.add_argument("-o", default=str(ROOT / "build" / "snd_level"))
    ap.add_argument("--grid", default="")
    ap.add_argument("--only", action="append", default=[])
    ap.add_argument("--cc", default="cc")
    ap.add_argument("--write", action="store_true")
    a = ap.parse_args()
    grid = [tuple(map(int, g.split(":"))) for g in a.grid.split(",")] if a.grid else GRID
    for g in REF + [DEF]:
        if g not in grid:
            grid.append(g)
    fams, ldb = families(), level_db()
    sounds = [(f, s, fn, sn) for f, (fn, ss) in enumerate(fams) for s, sn in enumerate(ss)
              if not a.only or f"{fn}:{sn}" in a.only]
    jobs = [(f, s, n, ac) for f, s, _, _ in sounds for n, ac in grid]
    od = Path(a.o).resolve()
    od.mkdir(parents=True, exist_ok=True)
    objs = engines(od, a.cc, a.j)
    hosts = build(jobs, od, a.cc, objs)
    tasks = [(exe, i, job) for exe, part in hosts for i, job in enumerate(part)]
    with ThreadPoolExecutor(a.j) as ex:
        res = dict(zip([t[2] for t in tasks],
                       ex.map(lambda t: run(t[0], t[1], a.s, od / ("w%d_%d_%d_%d.wav" % t[2])), tasks)))
    rows = []
    kdb = key_db()
    hdr = " ".join(f"n{n}a{ac:<2d}" for n, ac in grid)
    print(f"{'family':<14s}{'sound':<20s} trim | {hdr} | range  peak | new  ref")
    med = lambda v: sorted(v)[len(v) // 2]
    for f, s, fn, sn in sounds:
        t0 = (ldb.get(fn) or {}).get(sn, 0)
        c0 = list((kdb.get(fn) or {}).get(sn, [0.0] * 9))
        r = {g: res[(f, s) + g] for g in grid}
        li = [r[g][0] for g in grid if r[g][0] > -90]
        d = med([r[g][0] for g in REF])
        pk = max(r[g][2] for g in grid)
        new, c1 = t0, c0
        if d > -90 and all(r[g][0] > -90 for g in NODES):
            raw = {g: r[g][0] - kc_at(c0, g) for g in grid}            # (as without KEY_DB, at the trim t0)
            rr = med([raw[g] for g in REF])
            hi = max(raw[g] - rr for g in grid)
            lo = min(raw[g] - rr for g in grid)
            ku = min(1.0, WIN_UP / hi) if hi > 0 else 1.0
            kd = min(1.0, WIN_DOWN / -lo) if lo < 0 else 1.0
            want = {g: a.t + (raw[g] - rr) * (ku if raw[g] > rr else kd) for g in grid}
            room = {g: PEAK_MAX - r[g][3] for g in grid}               # (dB more this point may take)
            need = {g: min(want[g] - r[g][0], room[g]) for g in grid}
            dt = med([need[g] for g in REF])
            new = int(round(max(TRIM_LO, min(TRIM_HI, t0 + dt))))
            c1 = [c0[q] + need[g] - (new - t0) for q, g in enumerate(NODES)]
            over = (kc_at(c1, DEF) - kc_at(c0, DEF) + new - t0) - room[DEF]   # (HOW MANY 2, between the nodes:
            if over > 0:                                                    # its peaks bound its neighbours too)
                for g in ((1, 4), (3, 4)):
                    c1[NODES.index(g)] -= over
            c1 = [round(2 * max(KC_LO, min(KC_HI, v))) / 2 for v in c1]
        rows.append({"family": fn, "sound": sn, "trim": t0, "new": new, "kc": c0, "kc_new": c1,
                     "grid": {f"{n}:{ac}": r[(n, ac)] for n, ac in grid}})
        rng = (max(li) - min(li)) if li else 0
        print(f"{fn:<14s}{sn:<20s} {t0:4d} | " + " ".join(f"{r[g][0]:6.1f}" for g in grid)
              + f" | {rng:5.1f} {pk:5.1f} | {new:4d} {d:6.1f}  kc " + " ".join(f"{v:g}" for v in c1))
    (od / "snd_level.json").write_text(json.dumps({"target": a.t, "grid": grid, "rows": rows}, indent=1))
    if a.write:
        s = APP.read_text()
        i = s.index("const LEVEL_DB = {")
        j = s.index("\n};", i)
        blk = s[i:j]
        for r in rows:
            fam_at = blk.index(f'"{r["family"]}": {{')
            end = blk.index("}", fam_at)
            seg = blk[fam_at:end]
            seg2, k = re.subn(r'("%s": )-?\d+' % re.escape(r["sound"]), lambda m: m.group(1) + str(r["new"]), seg, 1)
            if not k:
                sys.exit(f"snd_level: {r['family']}:{r['sound']} not in LEVEL_DB")
            blk = blk[:fam_at] + seg2 + blk[end:]
        s = s[:i] + blk + s[j:]
        for r in rows:
            kdb.setdefault(r["family"], {})[r["sound"]] = r["kc_new"]
        i = s.index("const KEY_DB = {")
        j = s.index("\n};", i)
        fmt = lambda v: str(int(v)) if v == int(v) else str(v)
        lines = []
        for fam, _ in fams:
            if fam in kdb:
                lines += [f'  "{fam}": {{'] + [f'    "{sn}": [' + ", ".join(fmt(v) for v in c) + "],"
                                              for sn, c in kdb[fam].items()] + ["  },"]
        s = s[:i] + "const KEY_DB = {\n" + "\n".join(lines) + s[j:]
        APP.write_text(s)
        print("snd_level: trims and key curves written to audition/app.js LEVEL_DB, KEY_DB (make fw-landscapes next)")

if __name__ == "__main__":
    main()
