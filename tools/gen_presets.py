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
# the effects: each set names the values it moves off the defaults; the rest are the defaults (scenes.c scn_factory
# writes a record only for a value off its default, so a set costs the scene a record for each value it moves). The
# order is presets.h's PR_FX column order and scenes.c FX_ID's: never reorder, append only. (name, default, top)
FXP = [
    ("rv_type", 0, 7), ("rv_mix", 35, 127), ("rv_decay", 64, 127), ("rv_tone", 64, 127), ("rv_predelay", 0, 127),
    ("gd_mix", 0, 127), ("gd_time", 70, 133), ("gd_feedback", 50, 127), ("gd_tone", 127, 127), ("gd_grain", 64, 127),
    ("rv_size", 0, 127), ("rv_mod", 64, 127), ("rv_lowcut", 0, 127), ("rv_shimmer", 0, 127), ("rv_shift", 0, 3),
    ("gd_grains", 3, 29), ("gd_size", 64, 127), ("gd_spray", 20, 127), ("gd_pitch", 64, 127), ("gd_reverse", 0, 127),
    ("gd_spread", 64, 127), ("gd_freeze", 0, 1), ("gd_mod", 0, 127), ("gd_modrate", 40, 127), ("gd_ivl", 2, 7),
    ("mf_hp", 0, 127), ("mf_lp", 127, 127), ("mf_reso", 0, 127), ("mf_slope", 0, 2), ("ms_side", 115, 127),
]
RV = {"OFF": 0, "ROOM": 1, "HALL": 2, "INFINITE": 3, "DARK": 4, "DRIFT": 5, "SWELL": 6, "PLATE": 7}   # rv_type
IVL = {"NONE": 0, "OCT": 1, "5TH-OCT": 2, "4TH-5TH": 3, "MAJOR": 4, "MINOR": 5, "ALL": 6, "UP": 7}   # gd_ivl
# The sets, fxN in presets.txt (16 at most: pr_t.cf's low nibble). 0 .. 4: small and mostly the same, a touch of air,
# never a designed effect (their MIX values keep the dry / wet balance they had under the old equal-power crossfade:
# 16 -> 13, 18 -> 14, 24 -> 20, 32 -> 27, 40 -> 35: the same wet over dry, the dry now whole). 5 ..: the places beyond
# the real one, vast and unreal (the INFINITE tank and its kin, SHIMMER, the grain cloud, the M/S wider): each one
# clean (no hiss, no build-up past the limiter: tools/pr_level.py --fx measures the presets on them)
FX = [
    ("dry", {}),
    ("air", dict(rv_type=RV["HALL"], rv_mix=13, rv_decay=44, rv_tone=52)),               # a hall, far back: outdoor air
    ("space", dict(rv_type=RV["HALL"], rv_mix=20, rv_tone=56, rv_predelay=6)),           # a room, a wood, a valley
    ("echo", dict(rv_type=RV["HALL"], rv_mix=14, rv_decay=50, rv_tone=52,                # air and a soft, dark echo:
                  gd_mix=13, gd_time=92, gd_feedback=34, gd_tone=64, gd_grain=24)),      # rock faces
    ("cave", dict(rv_type=RV["HALL"], rv_mix=27, rv_decay=96, rv_tone=48, rv_predelay=12)),   # a long, dark tail
    ("vast", dict(rv_type=RV["INFINITE"], rv_mix=26, rv_decay=85, rv_tone=50, rv_predelay=40, rv_mod=80,     # a space
                  rv_lowcut=30, ms_side=120)),                                           # with no walls: ~30 s
    ("infinite", dict(rv_type=RV["INFINITE"], rv_mix=20, rv_decay=122, rv_tone=52, rv_predelay=20,  # SUSTAIN: the
                      rv_lowcut=64, ms_side=121)),                                       # tail never dies, bounded
    ("frozen", dict(rv_type=RV["DARK"], rv_mix=30, rv_decay=110, rv_tone=56, rv_lowcut=30, ms_side=118)),   # a still,
                                                                                         # deep bed of ice
    ("shimmer", dict(rv_type=RV["INFINITE"], rv_mix=28, rv_decay=95, rv_tone=60, rv_predelay=30, rv_lowcut=50,
                     rv_shimmer=45, ms_side=121)),                                       # a halo an octave up
    ("ice hall", dict(rv_type=RV["INFINITE"], rv_mix=28, rv_decay=80, rv_tone=84, rv_predelay=50, rv_mod=90,
                      rv_lowcut=40, rv_shimmer=30, rv_shift=2, ms_side=120)),            # glass walls ringing in 12ths
    ("drift echo", dict(rv_type=RV["DRIFT"], rv_mix=24, rv_decay=90, rv_tone=50, gd_mix=22, gd_time=110,
                        gd_feedback=85, gd_tone=70, gd_grain=40, gd_spread=100, gd_mod=50, gd_modrate=30,
                        ms_side=120)),                                                   # echoes bending away
    ("grain cloud", dict(rv_type=RV["HALL"], rv_mix=22, rv_decay=100, rv_tone=56, gd_mix=40, gd_time=80,
                         gd_feedback=75, gd_grains=14, gd_size=100, gd_spray=90, gd_grain=127, gd_spread=127,
                         gd_mod=90, gd_modrate=50, gd_ivl=IVL["OCT"], gd_tone=90, ms_side=122)),   # the sound
                                                                                         # scattered in octaves
    ("swell", dict(rv_type=RV["SWELL"], rv_mix=40, rv_decay=100, rv_tone=54, rv_predelay=20, rv_lowcut=30,
                   ms_side=120)),                                                        # the tail blooms after
    ("abyss", dict(rv_type=RV["DARK"], rv_mix=34, rv_decay=115, rv_tone=40, rv_lowcut=20, rv_shimmer=25,
                   rv_shift=3, mf_lp=112, ms_side=118)),                                 # an octave under, far down
    ("halo", dict(rv_type=RV["INFINITE"], rv_mix=26, rv_decay=105, rv_tone=70, rv_lowcut=60, rv_shimmer=60,
                  rv_shift=1, gd_mix=18, gd_time=90, gd_feedback=60, gd_grains=8, gd_grain=127, gd_reverse=60,
                  gd_mod=60, ms_side=121)),                                              # rising fifths, grains
                                                                                         # turning back
    ("mirage", dict(rv_type=RV["DRIFT"], rv_mix=28, rv_decay=100, rv_tone=56, gd_mix=30, gd_time=95,
                    gd_feedback=70, gd_grains=10, gd_size=110, gd_spray=100, gd_grain=127, gd_reverse=50,
                    gd_spread=127, gd_mod=110, gd_ivl=IVL["UP"], ms_side=122)),           # climbing, wavering
]
assert len(FX) <= 16, "16 effects sets at most (pr_t.cf)"


def fx_row(d):
    """a set's values in FXP order (the defaults where it names none)"""
    names = [n for n, _, _ in FXP]
    for k, v in d.items():
        assert k in names, f"FX: no parameter {k}"
        assert 0 <= v <= FXP[names.index(k)][2], f"FX: {k} {v} out of range"
    return [d.get(n, dv) for n, dv, _ in FXP]


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
        if len(parts) != 5 or not head or not re.match(r"^fx\d{1,2}$", head[-1]):
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
    if len(cats) > 15 or len(names) > 100 + 128 - PC_F2:
        sys.exit(f"presets.txt: 15 categories, {100 + 128 - PC_F2} presets at most (MIDI PC: pc_of)")
    return cats


def header(cats):
    n = sum(len(ps) for _, ps in cats)
    o = ["/* SPDX-License-Identifier: GPL-3.0-only */",
         "/* generated by tools/gen_presets.py from firmware/presets.txt: do not edit. make fw-test fails when it no",
         " * longer matches. presets.c reads it. */",
         "#ifndef TX_PRESETS_H", "#define TX_PRESETS_H", "#include <stdint.h>", "",
         f"#define PR_N {n}u", f"#define PR_NCAT {len(cats)}u", f"#define PR_NFX {len(FX)}u", f"#define PR_FX_NP {len(FXP)}u",
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
    o.append("/* the effects sets (scenes.c scn_factory FX_ID: the reverb's TYPE MIX DECAY TONE PREDELAY, the grain delay's MIX"
             " TIME FEEDBACK TONE GRAIN,")
    o.append(" * the reverb's SIZE MOD LOWCUT SHIMMER SHIFT, the grain delay's GRAINS SIZE SPRAY PITCH REVERSE SPREAD FREEZE MOD"
             " MOD RATE INTERVALS,")
    o.append(" * the FILTER's HIGH PASS LOW PASS RESONANCE SLOPE, the M/S's SIDE): "
             + ", ".join(f"{i} {nm}" for i, (nm, _) in enumerate(FX)) + " */")
    o.append("static const uint8_t PR_FX[PR_NFX][PR_FX_NP] = {")
    o += ["    {" + ", ".join(map(str, fx_row(v))) + "}," for _, v in FX]
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


PC_F2 = 116                                     # MIDI PC: F101 .. (0 .. 99 F001 .. F100, 100 .. 115 U01 .. U16;
                                                # scenes.c SCN_PC_F2)


def pc_of(i):
    """preset i's (0-based) MIDI program"""
    return i if i < 100 else PC_F2 + i - 100


def script(cats):
    """every preset loaded by MIDI PC, 4 s each: it loads, its lanes play what the table says, something sounds within
    its 4 s, no sample clips, no event dropped"""
    o = ["# the factory presets (generated by tools/gen_presets.py from firmware/presets.txt: do not edit): each one",
         "# loaded by MIDI PC (0 .. 99: F001 .. F100, 116 ..: F101 ..), 4 s apiece, EFFECTS ON LOAD at ALWAYS (each",
         "# one's effects come with it): it loads, what plays is the preset (\"expect preset\": every lane's sound, HOW",
         "# MANY, HOW ACTIVE, level, pan; the records, its effects' among them), the engines run it, something sounds",
         "# in its last 3 s, and over the whole run no sample clips and no event is dropped",
         "1000 midi 0xB4 99 4", "1000 midi 0xB4 98 38", "1000 midi 0xB4 6 0", "1100 expect state p426 0"]
    t, i = 1500, 0
    for _, ps in cats:
        for p in ps:
            o.append(f"{t} midi 0xC0 {pc_of(i)} 0")
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
