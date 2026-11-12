#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""The audio's hot code for the FM-1's flash cache (make fw-c1hot): firmware/c1_hot.txt, the functions tools/fm1_bench.py
places side by side at the start of .text, in this order.

The FM-1 runs code from flash through a 32 KB cache the two cores share (8 ways of 4 KB, 32-byte lines; a line missed
costs ~0.7 us). Where the hot functions land decides which of them evict which: placed together, the first 32 KB of
the list never evict one another, and the order puts first what runs every block whatever plays.

  tools/c1hot.py HOST_BIN DEVICE_ELF DEVICE_MAP PROF0.txt [PROF1.txt ...] > firmware/c1_hot.txt

HOST_BIN, PROFn: make fw-c1map's (build/fw/textura-c1prof and its C1PROF_OUT files, one a landscape: tests/c1prof.c);
DEVICE_ELF / DEVICE_MAP: make fm1-app's (build/fm1/textura-app.elf / .map). A function is the audio's when it is the
engines' (an engines/ object, its core-1 twin too) or lanes.c's / mod.c's / groove.c's (the firmware's audio side), runs
in flash (not .ram_text) and is called at least HOT_CALLS times a block in a landscape (the mean over its blocks). The
order: 1 the mix end (core 1's pipe job between the interrupts: the effects, the reverbs, the limiter; every block),
2 the firmware's audio side (every block), 3 the engines, the most time a block first (the mean over the landscapes,
both cores). Each tier by its time a block, the most first."""
import re
import subprocess
import sys
from pathlib import Path

HOT_CALLS = 0.2
BLOCKS = 21500 * 44100 // 256 // 1000            # (make fw-c1map: 21.5 s a landscape)
FW_AUDIO = ("lanes.c", "mod.c", "groove.c")
MIX_END = re.compile(r"o/tx_(fx(_[a-z0-9_]+)?|core_(mix|comp|limit|chamber))(_c1)?\.o$")
REV_ALL = re.compile(r"o/tx_(fx_(hall|hq|inf|plate|grain)|core_chamber)(_c1)?\.o$")   # (every TYPE: one runs, chosen live)


def defined_in(path):
    """the function names a C file defines (a line that starts a definition)"""
    txt = Path(path).read_text()
    return set(re.findall(r"^(?:static\s+)?(?:inline\s+)?(?:__attribute__\(\([^)]*\)\)\s+)*[a-z_0-9 *]+?\b([a-z_][a-z_0-9]*)\s*\(",
                          txt, re.M))


def host_names(path):
    out = subprocess.run(["nm", "-n", path], capture_output=True, text=True).stdout
    by = {}
    for ln in out.splitlines():
        p = ln.split()
        if len(p) == 3 and p[1] in "tT":
            by[int(p[0], 16)] = re.sub(r"\.\d+$", "", p[2][1:] if p[2].startswith("_") else p[2])
    return by


def device_sections(mapf):
    """the image's .text.NAME sections: NAME -> (address, size, object)"""
    secs = {}
    lines = Path(mapf).read_text().splitlines()
    for i, ln in enumerate(lines):
        m = re.match(r"^ \.text\.([A-Za-z_0-9.]+)\s*(?:(0x[0-9a-f]+)\s+(0x[0-9a-f]+)\s+(\S+))?$", ln)
        if not m:
            continue
        name, a, s, o = m.groups()
        if a is None and i + 1 < len(lines):
            m2 = re.match(r"^\s+(0x[0-9a-f]+)\s+(0x[0-9a-f]+)\s+(\S+)$", lines[i + 1])
            if not m2:
                continue
            a, s, o = m2.groups()
        secs.setdefault(name, (int(a, 16), int(s, 16), o))
    return secs


def main(argv):
    host, elf, mapf, profs = argv[0], argv[1], argv[2], argv[3:]
    hn = host_names(host)
    hmain = next(a for a, n in hn.items() if n == "main")
    secs = device_sections(mapf)
    fw = set()
    for f in FW_AUDIO:
        p = Path("firmware/src") / f
        if p.exists():
            fw |= defined_in(p)
    heat = {}                                    # name -> [us a block per landscape], calls a block (max)
    for li, pf in enumerate(profs):
        lines = Path(pf).read_text().splitlines()
        slide = int(lines[0].split()[1], 16) - hmain
        for ln in lines[2:]:
            a, k, c, ns = ln.split()
            n = hn.get(int(a, 16) - slide)
            if not n:
                continue
            h = heat.setdefault(n, [[0.0] * len(profs), 0.0])
            h[0][li] += int(ns) / 1000.0 / BLOCKS
            h[1] = max(h[1], int(c) / BLOCKS)
    tiers = {1: [], 2: [], 3: [], 4: []}
    total = {1: 0, 2: 0, 3: 0, 4: 0}
    for n, (us, calls) in heat.items():
        if calls < HOT_CALLS:
            continue
        mean = sum(us) / len(us)
        for name in (n + "_c1", n):              # (a TX_RAM function: core 1's twin in flash; core 0's in RAM)
            if name not in secs:
                continue
            a, s, o = secs[name]
            if not (0x02000000 <= a < 0x03000000):
                continue
            if MIX_END.search(o):
                t = 1
            elif o.endswith("o/textura.o"):
                if n not in fw:
                    continue
                t = 2
            elif re.search(r"o/tx_[a-z0-9_]+\.o$", o):
                t = 3
            else:
                continue
            tiers[t].append((mean, name, s))
            total[t] += s
    seen = {name for t in tiers.values() for _, name, _ in t}
    for name, (a, s, o) in sorted(secs.items(), key=lambda x: x[1][0]):   # 4: the reverbs' other TYPEs, the grains
        if name not in seen and REV_ALL.search(o) and 0x02000000 <= a < 0x03000000:
            tiers[4].append((-a, name, s))       # (their own order: as the objects have them)
            total[4] += s
    print("# The audio's hot code (tools/c1hot.py from make fw-c1map's profiles; make fw-c1hot writes it again):")
    print("# tools/fm1_bench.py places these functions side by side at the start of .text, in this order, so the")
    print("# flash cache the two cores share (32 KB) keeps them apart from the rest; the build fails on a name it no")
    print("# longer finds. 1 the mix end (core 1's pipe job), 2 the firmware's audio side, 3 the engines; each the most")
    print("# time a block first (host profile, the mean over 11 landscapes); 4 the reverbs' and the grain delay's other")
    print("# code (the TYPE the player picks: one runs at a time), as their objects have it.")
    run = 0
    for t, title in ((1, "the mix end"), (2, "the firmware's audio side"), (3, "the engines"),
                     (4, "the other reverb TYPEs, the grain delay")):
        print(f"# {t} {title}: {len(tiers[t])} functions, {total[t]} B")
        for mean, name, s in sorted(tiers[t], reverse=True):
            run += s
            print(f"{name:40s} # {s:5d} B, {run:6d} B so far" + (f", {mean:7.3f} us a block" if t < 4 else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
