# SPDX-License-Identifier: GPL-3.0-only
"""Write engines/tx_src_lin.h: the SOURCE's ACTIVITY made perceptually even (tx_core_lin.h), one table per sound.

ACTIVITY's knob is perceptually even in what the sound does: its activity quantity runs lo + (hi - lo) x^2.5 over the
knob (x = knob / 127), from the value it had at ACTIVITY 0 to the value it had at 127 (both ends unchanged), so each
step sounds the same amount more. The tables below only describe each sound's old law; the curve is tx_core_lin.h's. Per sound the quantity is (all read
from the engines' own laws, as tx_eng_source.c and the hosted engines' parameter help give them):
  rain, stones, water drops   events/s of an individual (IND_Y)
  cicadas                     the share of time a caller sings (song / (song + pause), CIC[0])
  birds                       songs per minute (1 / pause: +-2 octaves over the knob)
  hosted event engines        presses per second (e0 / e64 / e127), times the bout share below 64 (k0 & 2)
  hosted rate engines         their activity parameter's own rate (drips/s, bubbles/s, cracks/s, steps/s, mm/h,
                              crackles/s, grains/s), times the bout share
  hosted flows                speed (wind m/s, avalanche m/s, sliding m/s), the singing share of crickets and katydids
  waves, the stream in water  none: identity (their breakers already rise about linearly)
Each table holds log2 of the quantity (Q16) at ACTIVITY 0, 8 .. 120, 127 (its unit cancels out). All zeros: identity.

    python3 tools/gen_lin.py            (rewrites engines/tx_src_lin.h; reads engines/*.c)
"""
import math, os, re, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = open(os.path.join(ROOT, "engines/tx_eng_source.c")).read()
VS = [8 * k for k in range(16)] + [127]


def eng_src(name):
    return open(os.path.join(ROOT, "engines/tx_eng_%s.c" % name)).read()


def host_rows():
    i = SRC.index("static const host_t HOST[] = {")
    j = SRC.index("\n};", i)
    body = SRC[i:j]
    rows = []
    # each row: {"eng", g0, g64, g127, CURVES({curves}) or NO_CURVES, k0, k64, k127[, e0, e64, e127, oneshot]}
    for m in re.finditer(r'\{"(\w+)",\s*-?\d+,\s*-?\d+,\s*-?\d+,\s*(?:CURVES\((.*?)\)|NO_CURVES),\s*(\d+),\s*(\d+),\s*(\d+),\s*(\d+),\s*(\d+),\s*(\d+),\s*(\d+)\}',
                         body, re.S):
        curves = re.findall(r'\{"(\w+)",\s*(-?\d+),\s*(\d+),\s*(\d+)\}', m.group(2) or "")
        rows.append(dict(eng=m.group(1), c0=curves[0] if curves else None, k0=int(m.group(3)),
                         e=(int(m.group(6)), int(m.group(7)), int(m.group(8)))))
    return rows


def curve_at(c, v):
    v0, v64, v127 = int(c[1]), int(c[2]), int(c[3])
    p = v0 + (v64 - v0) * v / 64.0 if v < 64 else v64 + (v127 - v64) * (v - 64) / 63.0
    return min(127.0, max(0.0, round(p)))


def duty(v):                                    # host_gate: the bout share below ACTIVITY 64
    return 1.0 if v >= 64 else (120 + v * v * 3976 / 4096.0) / 4096.0


def press_rate(e, v):                           # host_rate: 1 / the mean gap, log between e0, e64, e127
    l = [math.log2(x) for x in e]
    lv = l[0] + (l[1] - l[0]) * v / 64.0 if v < 64 else l[1] + (l[2] - l[1]) * (v - 64) / 63.0
    return 2.0 ** -lv


def pop_share(law, p):                          # tx_core_pop.h: mean bout / (bout + pause), log between the ends
    b = 2.0 ** ((law[0] + (law[1] - law[0]) * p / 127.0) / 65536.0)
    q = 2.0 ** ((law[2] + (law[3] - law[2]) * p / 127.0) / 65536.0)
    return b / (b + q)


def law_of(eng):
    m = re.search(r"static const tx_pop_law_t LAW = \{(-?\d+), (-?\d+), (-?\d+), (-?\d+)\}", eng_src(eng))
    return [int(x) for x in m.groups()]


EXP = {  # an activity parameter on a log scale: the ratio of its top to its bottom (the help lines' ranges)
    ("stream", "FLOW"): 1000.0, ("fire", "MOIST"): 30.0, ("gravel", "RATE"): 20.0, ("rainsurf", "RATE"): 1000.0,
    ("lakeice", "RATE"): 100.0, ("sandsong", "FLOW"): 20.0, ("mineral", "SPEED"): 100.0, ("aeolian", "WIND"): 20.0,
    ("drip", "RATE"): 200.0, ("sand", "ORIFICE"): 20.0 ** 2.5,   # sand: Beverloo, flow ~ D0^2.5 (the jam left out)
}


def host_q(r):
    eng, c, k0, e = r["eng"], r["c0"], r["k0"], r["e"]
    if eng in ("surf",) or (eng == "stream" and r is ROWS[1]):
        return None                                                 # identity
    gate = (lambda v: duty(v)) if k0 & 2 else (lambda v: 1.0)
    if e[1]:                                                        # an event engine: its presses
        return lambda v: press_rate(e, v) * gate(v)
    name = c[0]
    if (eng, name) in EXP:
        x = EXP[(eng, name)]
        return lambda v: x ** (curve_at(c, v) / 127.0) * gate(v)
    if eng == "wind":
        return lambda v: max(0.05, 28.0 * (curve_at(c, v) / 127.0) ** 1.5) * gate(v)
    if eng in ("crickets", "katydids"):
        law = law_of(eng)
        return lambda v: pop_share(law, curve_at(c, v)) * gate(v)
    if eng == "bees":
        return lambda v: (1.0 + 7.0 * curve_at(c, v) / 127.0) * gate(v)
    if eng == "candle":                                             # the roar's amplitude, -30 .. 0 dB over FLOW
        return lambda v: 10.0 ** (1.5 * curve_at(c, v) / 127.0) * gate(v)
    sys.exit("gen_lin: no law for %s %s" % (eng, name))


def ind_y(k):
    i = SRC.index("static const int32_t IND_Y[3][4]")
    rows = re.findall(r"\{\s*(-?\d+),\s*(-?\d+),\s*(-?\d+),\s*(-?\d+)\s*\}", SRC[i:i + 400])
    y = [int(x) for x in rows[k]]
    at = [0, 9, 64, 127]
    def f(v):
        for j in range(3):
            if v <= at[j + 1]:
                return y[j] + (y[j + 1] - y[j]) * (v - at[j]) / (at[j + 1] - at[j])
        return y[3]
    return lambda v: 2.0 ** (f(v) / 65536.0)


def cicada_share():
    i = SRC.index("static const cic_t CIC[4]")
    m = re.search(r"\{(\d+), (\d+), (\d+), (\d+), (\d+), (\d+), (\d+),", SRC[i:])
    song_l2, pause_l2 = int(m.group(6)), int(m.group(7))
    def f(v):                                                       # bout_len: a = ACTIVITY - 64
        a = round(v) - 64
        k = 3 if a < 0 else 2
        s = 2.0 ** ((song_l2 + a * 512 * (5 if a < 0 else 2)) / 65536.0)
        p = 2.0 ** ((pause_l2 - a * 1024 * k) / 65536.0)
        s, p = min(max(s, 32), 65535), min(max(p, 32), 65535)
        return s / (s + p)
    return f


def table(q):
    if q is None:
        return [0] * 17
    l = [round(math.log2(q(v)) * 65536) for v in VS]
    for k in range(1, 17):                                          # monotone (a flat step: no knob lost)
        l[k] = max(l[k], l[k - 1])
    if l[16] <= l[0]:
        return [0] * 17
    return l


ROWS = host_rows()


# ---- each sound's own curve, from its measurement (tools/lin_sounds.txt: per sound, at ACTIVITY 0, 8 .. 120, 127 with
# the knob read as the place (no curve), its integrated loudness and its sounding share, at COUNT 8 and COUNT 1: the
# two densities averaged, so the curve holds for one individual and for a crowd). Perceived density: the mean of
# the two in one unit (30 dB louder counts as much as sounding all the time from never: a near-constant bed's
# sounding share hardly weighs, a sparse sound's does), made monotone (pool adjacent violators: noise never makes a step go back). The curve ends at the audible top,
# the first place within 2 % of the most; the table holds the place at knob 0, 8 .. 120, 127 where the density is the
# same fraction of its span as the knob of its own (equal steps).
def pava(y):
    b = [[x, 1] for x in y]
    i = 0
    while i < len(b) - 1:
        if b[i][0] > b[i + 1][0] + 1e-12:
            b[i] = [(b[i][0] * b[i][1] + b[i + 1][0] * b[i + 1][1]) / (b[i][1] + b[i + 1][1]), b[i][1] + b[i + 1][1]]
            del b[i + 1]
            if i > 0:
                i -= 1
        else:
            i += 1
    o = []
    for v, w in b:
        o += [v] * w
    for k in range(1, len(o)):
        if o[k] <= o[k - 1]:
            o[k] = o[k - 1] + 1e-4
    return o


def density(L, S):
    L = [max(x, max(L) - 50) for x in L]                    # (a silent window reads -120: a floor 50 dB under the top)
    lr, sr = max(L) - L[0], max(S) - S[0]
    d = [(l - L[0]) / 30.0 + (x - S[0]) / 100.0 for l, x in zip(L, S)]   # 30 dB louder ~ sounding all the time from never
    if max(d) - d[0] < 0.05:
        return None
    d = [x / max(d) for x in d]
    d = [d[0]] + [(d[i - 1] + 2 * d[i] + d[i + 1]) / 4 for i in range(1, 16)] + [d[16]]   # (1 2 1: a point's noise shared)
    m = pava(d)
    if lr < 15 and sr < 30:                                 # a sparse singer or call (birds, insects): its density, noisy
        top = max(1, next(i for i, x in enumerate(m) if x >= m[-1] - 0.02))   # even over 80 s, read as one power law
        best = None
        for q in [0.2 + 0.05 * k for k in range(97)]:
            e = sum((d[i] - (m[0] + (m[top] - m[0]) * (VS[i] / VS[top]) ** q)) ** 2 for i in range(top + 1))
            if best is None or e < best[0]:
                best = (e, q)
        q = best[1]
        m = [m[0] + (m[top] - m[0]) * (min(v, VS[top]) / VS[top]) ** q + (1e-4 * i if v > VS[top] else 0)
             for i, v in enumerate(VS)]
    return m


def density2(r):                                           # COUNT 8 and COUNT 1 alike: the mean of the two
    a, b = density(r["L"], r["S"]), density(r["L1"], r["S1"])
    if a is None or b is None:
        return a or b
    return pava([(x + y) / 2 for x, y in zip(a, b)])


def fit_table(m, vs):
    """the places (whole knob) at knob 0, 8 .. 120, 127 where the density m (at the places vs) is that share of its span"""
    top = max(1, next(i for i, x in enumerate(m) if x >= m[-1] - 0.02))
    t = [vs[0]]
    for u in VS[1:]:
        want = m[0] + (m[top] - m[0]) * u / 127.0
        for i in range(1, top + 1):
            if m[i] >= want:
                f = (want - m[i - 1]) / (m[i] - m[i - 1])
                t.append(int(round(vs[i - 1] + (vs[i] - vs[i - 1]) * f)))
                break
        else:
            t.append(vs[top])
    t[-1] = vs[top]
    for i in range(1, 17):
        t[i] = max(t[i], t[i - 1])
    return t


def sound_table(r):
    m = density2(r)
    if m is None:
        return None
    return fit_table(m, VS)


def sounds():
    presets = re.search(r'\{"PRESET", NPRE - 1, 0, "the source: ([^"]*)"', SRC).group(1)
    idx = {n.strip(): int(k) for k, n in re.findall(r"(\d+) ([^|]+)", presets)}
    rows = []
    for line in open(os.path.join(ROOT, "tools/lin_sounds.txt")):
        if line.startswith("#") or not line.strip():
            continue
        name, pre, typ, L, S, L1, S1 = [f.strip() for f in line.split("|")]
        fl = lambda x: [float(y) for y in x.split()]
        rows.append(dict(name=name, pre=pre, type=int(typ), L=fl(L), S=fl(S), L1=fl(L1), S1=fl(S1), idx=idx.get(pre)))
    return rows


def main():
    m = re.search(r"static const host_t HOST\[\] = \{", SRC)
    out = ["/* SPDX-License-Identifier: GPL-3.0-only */",
           "/* generated by tools/gen_lin.py from the engines' sources: do not edit. ACTIVITY made perceptually even per sound (tx_core_lin.h):",
           " * log2 of each sound's activity quantity (Q16) at ACTIVITY 0, 8 .. 120, 127; all zeros: the knob as it is. */",
           "#ifndef TX_SRC_LIN_H", "#define TX_SRC_LIN_H", "#include <stdint.h>", ""]
    out.append("#define SRC_LIN_NHOST %du" % len(ROWS))
    out.append("static const int32_t SRC_LIN_HOST[SRC_LIN_NHOST][17] = {   /* as HOST[] */")
    for r in ROWS:
        t = table(host_q(r))
        out.append("    {%s},   /* %s %s */" % (", ".join(str(x) for x in t), r["eng"], r["c0"][0] if r["c0"] else ""))
    out.append("};")
    out.append("enum { SRC_LIN_RAIN, SRC_LIN_STONES, SRC_LIN_WATER, SRC_LIN_CICADAS, SRC_LIN_BIRDS, SRC_LIN_NPRE };")
    out.append("static const int32_t SRC_LIN_PRE[SRC_LIN_NPRE][17] = {")
    for name, q in (("rain: drops/s", ind_y(0)), ("stones: knocks/s", ind_y(1)), ("water: drops/s", ind_y(2)),
                    ("cicadas: the singing share", cicada_share()), ("birds: songs / min", lambda v: 2.0 ** ((v - 64) / 32.0))):
        out.append("    {%s},   /* %s */" % (", ".join(str(x) for x in table(q)), name))
    out.append("};")
    snd = [r for r in sounds() if r["idx"] is not None]
    tabs = [(r, sound_table(r)) for r in snd]
    tabs = [(r, t) for r, t in tabs if t]
    tabs.sort(key=lambda x: (x[0]["idx"], x[0]["type"]))
    out.append("/* each measured sound's own curve (tx_lin_tab): the place at ACTIVITY 0, 8 .. 120, 127, by PRESET and TYPE */")
    out.append("#define SRC_LIN_NSND %du" % len(tabs))
    out.append("static const uint8_t SRC_LIN_SND_KEY[SRC_LIN_NSND][2] = {")
    out.append("    " + ", ".join("{%d, %d}" % (r["idx"], r["type"]) for r, _ in tabs))
    out.append("};")
    out.append("static const uint8_t SRC_LIN_SND[SRC_LIN_NSND][17] = {")
    for r, t in tabs:
        out.append("    {%s},   /* %s */" % (", ".join(str(x) for x in t), r["name"]))
    out.append("};")
    out.append("#endif")
    open(os.path.join(ROOT, "engines/tx_src_lin.h"), "w").write("\n".join(out) + "\n")
    st = [r for r in sounds() if r["pre"] == "#storm"]
    if st:
        t = sound_table(st[0])
        open(os.path.join(ROOT, "engines/tx_lin_storm.h"), "w").write("\n".join([
            "/* SPDX-License-Identifier: GPL-3.0-only */",
            "/* generated by tools/gen_lin.py from tools/lin_sounds.txt: do not edit. The storm's INTENSITY through its own",
            " * measured curve (tx_lin_tab): the place on the old scale at INTENSITY 0, 8 .. 120, 127 */",
            "#ifndef TX_LIN_STORM_H", "#define TX_LIN_STORM_H", "#include <stdint.h>",
            "static const uint8_t TX_LIN_STORM[17] = {%s};" % ", ".join(str(x) for x in t), "#endif", ""]))


if __name__ == "__main__":
    if sys.argv[1:] == ["--show"]:                          # each sound's curve, for a look
        for r in sounds():
            t = sound_table(r)
            print("%-20s %s" % (r["name"], " ".join("%3d" % x for x in t) if t else "none (it does not move)"))
    else:
        main()
