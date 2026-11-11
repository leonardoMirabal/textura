#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""The factory presets' table (firmware/src/presets.h) and their check script (tests/scripts/presets.txt) from
firmware/presets.txt (its header says the format).

  gen_presets.py           write both
  gen_presets.py --check   fail (exit 1) if either is not what presets.txt gives (make fw-test)

The sounds are looked up by name in firmware/src/landscapes.h (its LS table)."""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "firmware" / "presets.txt"
OUT = ROOT / "firmware" / "src" / "presets.h"
TEST = ROOT / "tests" / "scripts" / "presets.txt"
LSH = ROOT / "firmware" / "src" / "landscapes.h"

FAM = "WSIFDATXBY"                     # the families' letters, in LS order
ROLE = {"F": 0, "M": -6, "B": -12}
NAME_OK = re.compile(r"^[A-Z0-9 \-&'.]{1,11}$")
# the effects: reverb TYPE MIX DECAY TONE PREDELAY, grain delay MIX TIME FEEDBACK TONE GRAIN (scenes.c FX_ID; the
# defaults: 0 35 64 64 0, 0 70 50 100 64). Small and mostly the same: a touch of air, never a designed effect. The MIX
# values keep the dry / wet balance they had under the old equal-power crossfade (16 -> 13, 18 -> 14, 24 -> 20,
# 32 -> 27, 40 -> 35: the same wet over dry, the dry now whole)
FX = [
    ("dry", [0, 35, 64, 64, 0, 0, 70, 50, 100, 64]),
    ("air", [2, 13, 44, 52, 0, 0, 70, 50, 100, 64]),         # a hall, far back: outdoor air
    ("space", [2, 20, 64, 56, 6, 0, 70, 50, 100, 64]),       # a little more: a room, a wood, a valley
    ("echo", [2, 14, 50, 52, 0, 13, 92, 34, 64, 24]),        # air and a soft, dark echo: rock faces
    ("cave", [2, 27, 96, 48, 12, 0, 70, 50, 100, 64]),       # a long, dark tail
]


def sounds():
    """{family letter: [sound names]} from landscapes.h"""
    t = LSH.read_text()
    out = {}
    for i, m in enumerate(re.finditer(r'^\s*\{"([^"]+)", \d+, \{(.*)\}\},\s*$', t, re.M)):
        out[FAM[i]] = re.findall(r'\{"([^"]+)", "[^"]+", \d+, \d, \d+\}', m.group(2))
    assert len(out) == len(FAM), "landscapes.h: not the ten families"
    return out


TOK = re.compile(r"^(n\d|a\d|r[FMB]|r-?\d+|p\d+|v\d+|m)$")


def parse_lane(s, snd, where):
    s = s.strip()
    if s == "-":
        return None
    if len(s) < 3 or s[1] != ":" or s[0] not in FAM:
        sys.exit(f"{where}: a lane is C:sound ... or -: {s!r}")
    words = s[2:].split()
    i = 0
    while i < len(words) and not TOK.match(words[i]):
        i += 1
    name, toks = " ".join(words[:i]), words[i:]
    if name not in snd[s[0]]:
        sys.exit(f"{where}: no sound {name!r} in family {s[0]} ({', '.join(snd[s[0]])})")
    lane = {"fam": FAM.index(s[0]), "snd": snd[s[0]].index(name), "name": name, "n": 1, "a": 0, "r": 0, "p": 64,
            "v": 100, "m": 0}
    for t in toks:
        if not TOK.match(t):
            sys.exit(f"{where}: {t!r}?")
        if t == "m":
            lane["m"] = 1
        elif t[0] == "r":
            lane["r"] = ROLE[t[1]] if t[1] in ROLE else int(t[1:])
        else:
            lane[t[0]] = int(t[1:])
    if not (0 <= lane["n"] <= 6 and 0 <= lane["a"] <= 8 and 0 <= lane["p"] <= 127 and 0 <= lane["v"] <= 127):
        sys.exit(f"{where}: a value out of range: {s!r}")
    return lane


def parse(path=SRC):
    """[(category, [presets])], each preset {"name", "fx", "lanes": [lane or None] x 4, "line": i}"""
    snd = sounds()
    cats = []
    for i, raw in enumerate(path.read_text().splitlines(), 1):
        line = raw.split("#", 1)[0].rstrip()
        if not line.strip():
            continue
        if line.startswith("="):
            cats.append((line[1:].strip(), []))
            continue
        parts = line.split("|")
        head = parts[0].split()
        if len(parts) != 5 or not head or not re.match(r"^fx\d$", head[-1]):
            sys.exit(f"presets.txt:{i}: NAME fxN | lane | lane | lane | lane")
        name, fx = " ".join(head[:-1]), int(head[-1][2:])
        if not NAME_OK.match(name):
            sys.exit(f"presets.txt:{i}: the name {name!r} (11 characters at most: A-Z 0-9 space - & ' .)")
        if fx >= len(FX):
            sys.exit(f"presets.txt:{i}: no fx{fx}")
        lanes = [parse_lane(p, snd, f"presets.txt:{i}") for p in parts[1:]]
        seen = [(l["fam"], l["snd"]) for l in lanes if l]
        if len(seen) != len(set(seen)):
            sys.exit(f"presets.txt:{i}: a sound twice (its level is the sound's: one lane each)")
        if not seen:
            sys.exit(f"presets.txt:{i}: no sound")
        if not cats:
            sys.exit(f"presets.txt:{i}: a preset before the first category")
        cats[-1][1].append({"name": name, "fx": fx, "lanes": lanes, "line": i})
    names = [p["name"] for _, ps in cats for p in ps]
    if len(names) != len(set(names)):
        sys.exit("presets.txt: a name twice")
    if len(cats) > 15 or len(names) > 255:
        sys.exit("presets.txt: 15 categories, 255 presets at most")
    return cats


def header(cats):
    n = sum(len(ps) for _, ps in cats)
    o = ["/* SPDX-License-Identifier: GPL-3.0-only */",
         "/* generated by tools/gen_presets.py from firmware/presets.txt: do not edit. make fw-test fails when it no",
         " * longer matches. presets.c reads it. */",
         "#ifndef TX_PRESETS_H", "#define TX_PRESETS_H", "#include <stdint.h>", "",
         f"#define PR_N {n}u", f"#define PR_NCAT {len(cats)}u", f"#define PR_NFX {len(FX)}u", "#define PR_FX_NP 10u",
         "typedef struct { uint8_t fs, ca, vol, pan; } pr_lane_t;   /* fs: family << 4 | sound (0xFF: none); ca: mute << 7 |",
         "                                              * HOW MANY << 4 | HOW ACTIVE key; LEVEL; PAN */",
         "typedef struct { char name[12]; uint8_t cf; pr_lane_t l[4]; } pr_t;   /* cf: category << 4 | effects (PR_FX) */",
         "static const char *const PR_CAT[PR_NCAT] = {" + ", ".join(f'"{c}"' for c, _ in cats) + "};"]
    at, k = [], 0
    for _, ps in cats:
        at.append(k)
        k += len(ps)
    at.append(k)
    o.append("static const uint8_t PR_CAT_AT[PR_NCAT + 1u] = {" + ", ".join(map(str, at)) + "};   /* each one's first */")
    o.append("/* the effects (scenes.c scn_factory: reverb TYPE MIX DECAY TONE PREDELAY, grain delay MIX TIME FEEDBACK TONE"
             " GRAIN): " + ", ".join(f"{i} {nm}" for i, (nm, _) in enumerate(FX)) + " */")
    o.append("static const uint8_t PR_FX[PR_NFX][PR_FX_NP] = {")
    o += ["    {" + ", ".join(map(str, v)) + "}," for _, v in FX]
    o.append("};")
    o.append("static const pr_t PR[PR_N] = {")
    for c, (_, ps) in enumerate(cats):
        for p in ps:
            ls = []
            for l in p["lanes"]:
                if l is None:
                    ls.append("{0xFF, 0, 100, 64}")
                else:
                    ls.append("{0x%02X, 0x%02X, %d, %d}" % (l["fam"] << 4 | l["snd"], l["m"] << 7 | l["n"] << 4 | l["a"],
                                                          l["v"], l["p"]))
            o.append('    {"%s", 0x%X%X, {%s}},' % (p["name"], c, p["fx"], ", ".join(ls)))
    o += ["};", "", "#endif", ""]
    return "\n".join(o)


def script(cats):
    """every preset loaded by MIDI PC, 4 s each: it loads, its lanes play what the table says, something sounds within
    its 4 s, no sample clips, no event dropped"""
    o = ["# the factory presets (generated by tools/gen_presets.py from firmware/presets.txt: do not edit): each one",
         "# loaded by MIDI PC (0 .. 99: F001 .. F100), 4 s apiece: it loads, what plays is the preset (\"expect preset\":",
         "# every lane's sound, HOW MANY, HOW ACTIVE, level, pan; the records), the engines run it, something sounds",
         "# in its last 3 s, and over the whole run no sample clips and no event is dropped"]
    t, i = 1500, 0
    for _, ps in cats:
        for p in ps:
            o.append(f"{t} midi 0xC0 {i} 0")
            o.append(f"{t + 1200} expect state loading 0")
            o.append(f"{t + 1200} expect state scene F{i + 1:03d}_{p['name'].replace(' ', '_')}")
            o.append(f"{t + 1200} expect preset")
            for k, l in enumerate(p["lanes"]):
                o.append(f"{t + 1200} expect state run{k} {1 if l and l['n'] else 0}")
            o.append(f"{t + 3990} expect sound")
            o.append(f"{t + 3990} expect state heard 1")
            t += 4000
            i += 1
    o += [f"{t} expect state clips 0", f"{t} expect state dropped 0", f"{t + 10} end", ""]
    return "\n".join(o)


def main():
    cats = parse()
    h, s = header(cats), script(cats)
    if "--check" in sys.argv:
        bad = [p.name for p, v in ((OUT, h), (TEST, s)) if not p.exists() or p.read_text() != v]
        if bad:
            sys.exit("gen_presets: not what firmware/presets.txt gives: " + ", ".join(bad) + " (make fw-presets)")
        return
    OUT.write_text(h)
    TEST.write_text(s)
    print(f"gen_presets: {sum(len(p) for _, p in cats)} presets in {len(cats)} categories -> {OUT.name}, {TEST.name}")


if __name__ == "__main__":
    main()
