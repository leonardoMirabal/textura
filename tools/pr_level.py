#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""The factory presets' levels, measured: each preset played on the firmware's host build (build/fw/textura-host,
make fw-host), the whole mix and each lane alone (four builds of it whose table has that lane at its LEVEL, the
others at 0), from 0.5 s after the load for SECS seconds; the loudness of each (tools/loudness.py: BS.1770 integrated, gated, so a sparse lane counts as loud as
its events are).

Each lane then gets the LEVEL that puts it at its place in the mix (presets.txt r: the foreground at TARGET LUFS, a
midground 6 dB under it, a background 12, or the dB given): LEVEL is a square law (tx_level_q15: 40 log10(v / 100)
dB), 127 at most (+4.2 dB), and a lane alone keeps its loudest 1 % of 50 ms peaks under PEAK_MAX (loudness.p99), so a preset whose source cannot reach its place
(sparse, or all transients) comes down whole when that source is its foreground; another lane short of its
place plays at its most.

  .local/venv/bin/python tools/pr_level.py [-j JOBS] [-s SECS] [-t TARGET] [-o DIR] [--write] [PRESET ...]
      measure (all presets, or the numbers 1 .. 100 given); prints the table, and with --write sets the levels in
      firmware/presets.txt (then: python3 tools/gen_presets.py, make fw-host, and again to check)
  .local/venv/bin/python tools/pr_level.py --mix [-o DIR] [PRESET ...]  the mixes only (the table's mix columns), the WAVs kept in DIR"""
import argparse
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
HOST = ROOT / "build" / "fw" / "textura-host"
T_LOADS = (1500, 1617, 1733)                    # ms: the PC (three loads: three seeds, tx_seed takes the time)
T_FROM = 500                                    # ms after the load: the measurement
SOLO = {}                                       # lane k alone: its host build (solo_hosts)
PEAK_MAX = -2.0                                 # dBFS: a lane alone, its loudest 1 % of 50 ms peaks (the limiter: -2)


def solo_hosts(cats, od, cc):
    """four host builds, lane k alone in every preset at its LEVEL (the bus COMPRESSOR is not linear: a lane is
    measured where it plays, and a second run corrects what the first moved; the others at 0 from the load on: a LEVEL turned down
    later reaches some engines only as their voices renew, seconds later): {k: path}"""
    import glob
    import copy
    out, procs = {}, []
    for k in range(4):
        c2 = copy.deepcopy(cats)
        for _, ps in c2:
            for p in ps:
                for j, l in enumerate(p["lanes"]):
                    if l:
                        l["v"] = l["v"] if j == k else 0
        h = od / f"solo{k + 1}.h"
        h.parent.mkdir(parents=True, exist_ok=True)
        h.write_text(gen_presets.header(c2))
        out[k] = od / f"textura-host-solo{k + 1}"
        cmd = ["nice", "-n", "10", cc, "-O2", "-w", "-DDINGHY_HOST", "-DAPP_VER=\"dev\"", f"-DPR_TABLE=\"{h}\"",
               "-Ifirmware/src", "-Ifirmware/gen", "-Iengines", "-o", str(out[k]), "firmware/src/textura.c"]
        procs.append(subprocess.Popen(cmd + sorted(glob.glob("engines/*.c")) + ["-lm"], cwd=ROOT))
    if any(p.wait() for p in procs):
        sys.exit("pr_level: a solo build failed")
    return out


def run(out, i, solo, secs):
    """preset i (0-based), each of T_LOADS for secs from T_FROM, all lanes (solo None) or lane solo alone, the three
    joined: (I, S95, peak)"""
    import numpy as np
    xs = []
    for n, t0 in enumerate(T_LOADS):
        lines = [f"{t0} midi 0xC0 {i} 0"]
        lines.append(f"{t0 + T_FROM + secs * 1000} end")
        o = out.with_name(out.name + f"_s{n + 1}")
        o.parent.mkdir(parents=True, exist_ok=True)
        scr = o.with_suffix(".txt")
        scr.write_text("\n".join(lines) + "\n")
        subprocess.run(["nice", "-n", "10", str(HOST if solo is None else SOLO[solo]), str(o), str(scr)], check=True, stdout=subprocess.DEVNULL)
        x = loudness.read(o.with_suffix(".wav"))
        xs.append(x[int((t0 + T_FROM) / 1000 * loudness.FS):])
        scr.unlink()
        if solo is not None or n:
            o.with_suffix(".wav").unlink()
    x = np.concatenate(xs)
    return loudness.measure(x) + (loudness.p99(x),)


def db_of(v):
    return 40 * math.log10(max(v, 1) / 100)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-j", type=int, default=4)
    ap.add_argument("-s", type=int, default=50)
    ap.add_argument("-t", type=float, default=-28.0)
    ap.add_argument("-o", default=str(ROOT / "build" / "presets"))
    ap.add_argument("--write", action="store_true")
    ap.add_argument("--mix", action="store_true")
    ap.add_argument("--cc", default="cc")
    ap.add_argument("presets", nargs="*", type=int)
    a = ap.parse_args()
    if not HOST.exists():
        sys.exit("pr_level: no build/fw/textura-host (make fw-host)")
    cats = gen_presets.parse()
    ps = [p for _, c in cats for p in c]
    want = [n - 1 for n in a.presets] if a.presets else list(range(len(ps)))
    od = Path(a.o).resolve()
    if not a.mix:
        SOLO.update(solo_hosts(cats, od, a.cc))
    jobs = []
    for i in want:
        jobs.append((i, None))
        if not a.mix:
            jobs += [(i, k) for k, l in enumerate(ps[i]["lanes"]) if l and l["n"]]
    with ThreadPoolExecutor(a.j) as ex:
        res = dict(zip(jobs, ex.map(lambda j: run(od / (f"p{j[0] + 1:03d}" + ("" if j[1] is None else f"_l{j[1] + 1}")),
                                                  j[0], j[1], a.s), jobs)))
    src = gen_presets.SRC.read_text().splitlines()
    print("  #  name         mix I  S95  peak | lane: level  I  peak (alone, at its level)  place")
    for i in want:
        p = ps[i]
        mi, ms, mp, _ = res[(i, None)]
        if a.mix:
            print(f"{i + 1:3d}  {p['name']:<11s} {mi:6.1f} {ms:5.1f} {mp:5.1f}")
            continue
        cells, new = [], {}
        lanes = [(k, l) for k, l in enumerate(p["lanes"]) if l and l["n"]]
        l100 = {k: res[(i, k)][0] - db_of(p["lanes"][k]["v"]) for k, l in lanes
                if res[(i, k)][0] > -90}                                       # (alone, as at LEVEL 100)
        if l100:
            need = {k: a.t + p["lanes"][k]["r"] - l100[k] for k in l100}       # dB from LEVEL 100
            cap = {k: min(db_of(127), db_of(p["lanes"][k]["v"]) + PEAK_MAX - res[(i, k)][3]) for k in l100}
            fg = [k for k in need if p["lanes"][k]["r"] == 0] or list(need)   # (the foreground sets the preset's
            over = max(0.0, max(need[k] - cap[k] for k in fg))                 # place; another lane short of its
            for k in need:                                                     # own just plays at its most)
                need[k] = min(need[k] - over, cap[k]) + over
            for k in need:
                new[k] = max(4, min(127, round(100 * 10 ** ((need[k] - over) / 40))))
        for k, l in lanes:
            li, _, lp, _ = res[(i, k)]
            cells.append(f"L{k + 1} {l['v']:3d} {li:6.1f} {lp:5.1f} {'FMB'[[0, -6, -12].index(l['r'])] if l['r'] in (0, -6, -12) else l['r']}"
                         + (f" -> {new[k]}" if k in new and new[k] != l["v"] else ""))
        print(f"{i + 1:3d}  {p['name']:<11s} {mi:6.1f} {ms:5.1f} {mp:5.1f} | " + " | ".join(cells))
        if a.write and new:
            ln = p["line"] - 1
            parts = src[ln].split("|")
            for k, v in new.items():
                parts[k + 1] = re.sub(r"\bv\d+\b", f"v{v}", parts[k + 1])
            src[ln] = "|".join(parts)
    if a.write:
        gen_presets.SRC.write_text("\n".join(src) + "\n")
        print("pr_level: levels written to firmware/presets.txt (python3 tools/gen_presets.py next)")


if __name__ == "__main__":
    main()
