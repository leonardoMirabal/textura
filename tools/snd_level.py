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
sounds stay under the target and keep their crest). Run it again after --write: a sound the limiter held at the
old trim reads its peaks low the first time.

  .local/venv/bin/python tools/snd_level.py [-j JOBS] [-s SECS] [-t TARGET] [-o DIR] [--grid N:A,...] [--only F:name]
      prints a table (one row per sound: its trim, its loudness at each grid point, the range, the new trim) and
      writes DIR/snd_level.json; --write sets the new trims in audition/app.js LEVEL_DB (then: make fw-landscapes)"""
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
GRID = [(1, 0), (1, 4), (1, 8), (2, 4), (3, 0), (3, 4), (3, 8), (6, 0), (6, 4), (6, 8)]
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
    hdr = " ".join(f"n{n}a{ac:<2d}" for n, ac in grid)
    print(f"{'family':<14s}{'sound':<20s} trim | {hdr} | range  peak | new  default")
    for f, s, fn, sn in sounds:
        t0 = (ldb.get(fn) or {}).get(sn, 0)
        r = {g: res[(f, s) + g] for g in grid}
        li = [r[g][0] for g in grid if r[g][0] > -90]
        ref = sorted(r[g][0] for g in REF)
        d = ref[len(ref) // 2]
        pk = max(r[g][2] for g in grid)
        if d > -90:
            new = t0 + a.t - d
            pq = sorted(r[g][3] for g in REF)
            new = min(new, t0 + PEAK_MAX - pq[len(pq) // 2])
            new = int(round(max(TRIM_LO, min(TRIM_HI, new))))
        else:
            new = t0
        rows.append({"family": fn, "sound": sn, "trim": t0, "new": new,
                     "grid": {f"{n}:{ac}": r[(n, ac)] for n, ac in grid}})
        rng = (max(li) - min(li)) if li else 0
        print(f"{fn:<14s}{sn:<20s} {t0:4d} | " + " ".join(f"{r[g][0]:6.1f}" for g in grid)
              + f" | {rng:5.1f} {pk:5.1f} | {new:4d} {d + new - t0:6.1f}")
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
        APP.write_text(s[:i] + blk + s[j:])
        print("snd_level: trims written to audition/app.js LEVEL_DB (make fw-landscapes next)")


if __name__ == "__main__":
    main()
