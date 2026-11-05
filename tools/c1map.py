#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""The two cores' code, per landscape (make fw-c1map): what core 1 runs in its jobs, what core 0 runs meanwhile, and
where each function sits in the FM-1 image (RAM: .ram_text, XIP: flash through its cache; "-": inlined there).

  tools/c1map.py HOST_BIN DEVICE_ELF PROF0.txt [PROF1.txt ...] [--names NAME,...]

HOST_BIN: build/fw/textura-c1prof; PROFn: its C1PROF_OUT files, one a landscape (tests/c1prof.c); DEVICE_ELF:
build/fm1/textura-app.elf (make fm1-app). Per function: its FM-1 place and size; per place (c1: core 1's jobs, c0||:
core 0 while a job runs, c0: core 0 otherwise) calls a block and host self time a block (us, the mean over the
landscapes, and the worst landscape's). nm reads both (the host's and the FM-1's symbols)."""
import re
import subprocess
import sys


def nm(path):
    """the FM-1 image's functions: name -> (address, size)"""
    out = subprocess.run(["nm", "-S", "-n", path], capture_output=True, text=True).stdout
    syms = {}
    for ln in out.splitlines():
        p = ln.split()
        if len(p) == 4 and p[2] in "tT":
            syms.setdefault(p[3], (int(p[0], 16), int(p[1], 16)))
    return syms


def host_names(path):
    out = subprocess.run(["nm", "-n", path], capture_output=True, text=True).stdout
    by = {}
    for ln in out.splitlines():
        p = ln.split()
        if len(p) == 3 and p[1] in "tT":
            by[int(p[0], 16)] = re.sub(r"\.\d+$", "", p[2][1:] if p[2].startswith("_") else p[2])
    return by


BLOCKS = 21500 * 44100 // 256 // 1000   # (make fw-c1map: 21.5 s a landscape, the blocks every profile spans)


def place(addr):
    return "RAM" if 0x01C00000 <= addr < 0x01C06000 else "XIP" if addr >= 0x02000000 else "?"


def main(argv):
    names = None
    if "--names" in argv:
        i = argv.index("--names")
        names = set(argv[i + 1].split(","))
        argv = argv[:i] + argv[i + 2:]
    host, dev, profs = argv[0], argv[1], argv[2:]
    hn = host_names(host)
    hmain = next(a for a, n in hn.items() if n == "main")
    dv = {}
    for n, (a, s) in nm(dev).items():
        dv[re.sub(r"\.\d+$", "", n)] = (a, s)
    rows = {}                                    # name -> place -> [per landscape (calls/blk, us/blk)]
    for li, pf in enumerate(profs):
        lines = open(pf).read().splitlines()
        slide = int(lines[0].split()[1], 16) - hmain
        recs = []
        blocks = BLOCKS
        for ln in lines[2:]:
            a, k, c, ns = ln.split()
            n = hn.get(int(a, 16) - slide, a)
            recs.append((n, int(k), int(c), int(ns)))
        for n, k, c, ns in recs:
            r = rows.setdefault(n, {}).setdefault(k, [(0.0, 0.0)] * len(profs))
            r[li] = (c / blocks, ns / 1000.0 / blocks)
    print(f"{'function':34s} {'FM-1':4s} {'bytes':>6s}  {'c1 calls':>8s} {'c1 us':>7s} {'(max)':>7s}  "
          f"{'c0|| calls':>10s} {'c0|| us':>7s} {'(max)':>7s}")
    out = []
    for n, by in rows.items():
        c1 = by.get(1)
        c0p = by.get(2)
        if not c1 and not c0p:
            continue
        if names and n not in names:
            continue
        a, s = dv.get(n, (None, 0))
        pl = place(a) if a is not None else "-"
        def agg(v):
            if not v:
                return 0.0, 0.0, 0.0
            return (sum(x[0] for x in v) / len(v), sum(x[1] for x in v) / len(v), max(x[1] for x in v))
        a1, b1, m1 = agg(c1)
        a2, b2, m2 = agg(c0p)
        out.append((b1 + b2, n, pl, s, a1, b1, m1, a2, b2, m2))
    for _, n, pl, s, a1, b1, m1, a2, b2, m2 in sorted(out, reverse=True):
        print(f"{n[:34]:34s} {pl:4s} {s:6d}  {a1:8.2f} {b1:7.2f} {m1:7.2f}  {a2:10.2f} {b2:7.2f} {m2:7.2f}")


if __name__ == "__main__":
    main(sys.argv[1:])
