# SPDX-License-Identifier: GPL-3.0-only
"""txcover: how much of its family's variety a SOURCE preset can reach (the user: "the engine usually sounds similar
to one sample, not all of them").

For every reference recording of a preset's family (local only, .local/reference; never committed) and for renders
of the preset swept over its controls (INTENSITY, PROXIMITY, SIZE, TONE, HARD, LIFE), the same small feature vector:

  cent    log2 of the spectral centroid, 100 Hz .. 16 kHz    flat    spectral flatness (noise 1 .. tone 0), median
  slope   1/3-octave level slope, 100 Hz .. 16 kHz (dB/oct)  low     share of the power at 100-500 Hz (dB)
  rate    onsets per second (log10(1 + rate))              ioicv   inter-onset spread (CV)
  lvlsd   the onsets' level spread (dB)                    decay   the onsets' median decay to -20 dB (log10 ms)
  modlo   envelope modulation 0.5-4 Hz (dB re the total)   modhi   envelope modulation 4-32 Hz (dB re the total)
  kurt    log10 kurtosis of the 10 ms envelope (sparse)    dyn     10-90 % range of the 100 ms levels (dB)

Absolute level is left out (the recordings' gains are arbitrary); every signal gets a common noise floor 50 dB under its
loud frames (a render's digital silence would otherwise read as a huge level range against a recording's room). Each feature is put in units of a tolerance (TOL,
ours: half an octave of centroid, 1.5 dB/oct of slope, 40 % of rate or decay, 3 dB of modulation ...). A reference is
REACHED when some render of the sweep lies within 1 tolerance (RMS over the features) and no single feature is off by
more than 2; otherwise
the report names the features that miss (the nearest render's gap, signed: + = the reference is higher).

    .local/venv/bin/python bench/txcover.py [PRESET ...] [--json OUT] [--plots DIR]
        presets by name (rain stones cicadas water wind fire crickets sand gravel katydids bees ice wood minerals glass);
        default: all. Renders with build/cover/txrender, built from the committed engines (git HEAD), not the work tree.
"""
import glob
import json
import math
import os
import subprocess
import sys

import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "audition"))
import ab_prepare as A  # noqa: E402  (load: mono 44.1 kHz)

FS = 44100
REF = os.environ.get("TX_REF", os.path.join(ROOT, ".local", "reference"))
OUT = os.path.join(ROOT, "build", "cover")
RMS_IN, MAX_IN = 1.0, 2.0       # reached: the nearest render within 1 tolerance (RMS of the features), none off by 2
# a tolerance per feature (ours: about what tells two recordings of one kind apart by ear): the features' unit
TOL = {"cent": 0.5, "slope": 1.5, "low": 4.0, "flat": 0.08, "rate": 0.15, "ioicv": 0.3, "lvlsd": 2.0, "decay": 0.15,
       "modlo": 3.0, "modhi": 3.0, "kurt": 0.3, "dyn": 3.0}
FEATS = ["cent", "slope", "low", "flat", "rate", "ioicv", "lvlsd", "decay", "modlo", "modhi", "kurt", "dyn"]

# each SOURCE preset: its index, its reference folders (the family), hosted (only INTENSITY and PROXIMITY reach it)
PRESETS = {
    "rain": (0, ["rainwater", "rainsurf", "storm"], False),
    "stones": (1, ["stone", "krint", "rockfall"], False),
    "cicadas": (2, ["cicadas"], False),
    "water": (3, ["drip", "stream"], False),
    "wind": (4, ["aeolian"], True),
    "fire": (5, ["fire", "candle"], True),
    "crickets": (6, ["crickets"], True),
    "sand": (7, ["sand", "sandsong"], True),
    "gravel": (8, ["gravel", "pebble"], True),
    "katydids": (9, ["katydids"], True),
    "bees": (10, ["bees"], True),
    "ice": (11, ["ice", "lakeice"], True),
    "wood": (12, ["wood"], True),
    "minerals": (13, ["minerals", "mineral"], True),
    "glass": (14, ["glass"], True),
}


# ---- the features
def frames_db(x, n, hop):
    k = 1 + max(0, (len(x) - n) // hop)
    idx = np.arange(n)[None, :] + hop * np.arange(k)[:, None]
    return x[idx]


def onsets(x):
    """spectral-flux onsets (5 ms hops, 24 log bands 100 Hz .. 16 kHz): a peak 3 MADs over its 0.5 s median, the
    level 6 dB over the median level, at least 20 ms apart; per onset its peak level (dB) and its decay to -20 dB (ms)"""
    hop, n = 220, 1024
    fr = frames_db(x, n, hop) * np.hanning(n)
    S = np.abs(np.fft.rfft(fr, axis=1)) ** 2
    f = np.fft.rfftfreq(n, 1 / FS)
    edges = np.geomspace(100, 16000, 25)
    B = np.stack([S[:, (f >= edges[i]) & (f < edges[i + 1])].sum(1) for i in range(24)], 1) + 1e-12
    L = 10 * np.log10(B)
    flux = np.maximum(np.diff(L, axis=0), 0).sum(1)
    flux = np.concatenate([[0], flux])
    lev = 10 * np.log10(S.sum(1) + 1e-12)
    w = 50
    pad = np.pad(flux, w, mode="edge")
    med = np.array([np.median(pad[i:i + 2 * w]) for i in range(len(flux))])
    mad = np.array([np.median(np.abs(pad[i:i + 2 * w] - med[i])) for i in range(len(flux))]) + 1e-6
    lpad = np.pad(lev, w, mode="edge")
    lmed = np.array([np.median(lpad[i:i + 2 * w]) for i in range(len(lev))])
    on, last = [], -10
    for i in range(1, len(flux) - 1):
        if flux[i] > med[i] + 3 * mad[i] and flux[i] >= flux[i - 1] and flux[i] >= flux[i + 1] and lev[i] > lmed[i] + 6 \
                and i - last >= 4:
            on.append(i); last = i
    pk, dec = [], []
    for j, i in enumerate(on):
        end = on[j + 1] if j + 1 < len(on) else len(lev)
        seg = lev[i:min(end, i + 400)]
        p = seg.max(); pk.append(p)
        below = np.nonzero(seg < p - 20)[0]
        dec.append((below[0] if len(below) else len(seg)) * hop / FS * 1000)
    return np.array(on) * hop / FS, np.array(pk), np.array(dec)


def features(x):
    x = x - x.mean()
    rms = np.sqrt(np.mean(x ** 2)) + 1e-12
    x = x / rms * 0.05
    # a common noise floor, 50 dB under the loud frames (the 95th percentile of the 100 ms levels): the engine's digital
    # silence between events and a recording's room noise then read alike (level range, sparseness, onsets)
    l95 = np.percentile(np.sqrt(np.mean(frames_db(x, 4410, 4410) ** 2, axis=1)), 95)
    x = x + np.random.default_rng(7).standard_normal(len(x)) * l95 * 10 ** (-50 / 20)
    # spectrum: centroid, 1/3-octave slope, low share, flatness
    n = 4096
    fr = frames_db(x, n, n // 2) * np.hanning(n)
    S = np.abs(np.fft.rfft(fr, axis=1)) ** 2 + 1e-18
    f = np.fft.rfftfreq(n, 1 / FS)
    P = S.mean(0)
    sb = (f >= 100) & (f <= 16000)                             # (the recordings' rumble under 100 Hz left out)
    cent = math.log2((P[sb] * f[sb]).sum() / P[sb].sum())
    bands = [100 * 2 ** (k / 3) for k in range(23)]
    lv, cf = [], []
    for fc in bands:
        m = (f >= fc * 2 ** (-1 / 6)) & (f < fc * 2 ** (1 / 6))
        if m.any():
            lv.append(10 * math.log10(P[m].sum() + 1e-18)); cf.append(math.log2(fc))
    slope = np.polyfit(cf, lv, 1)[0]
    low = 10 * math.log10(P[(f >= 100) & (f < 500)].sum() / P[sb].sum() + 1e-12)
    band = (f > 100) & (f < 16000)
    gm = np.exp(np.mean(np.log(S[:, band]), axis=1)); am = np.mean(S[:, band], axis=1)
    flat = float(np.median(gm / am))
    # events
    t, pk, dec = onsets(x)
    dur = len(x) / FS
    rate = len(t) / dur
    ioi = np.diff(t)
    ioicv = float(ioi.std() / ioi.mean()) if len(ioi) > 2 else 0.0
    lvlsd = float(pk.std()) if len(pk) > 2 else 0.0
    decay = math.log10(np.median(dec) + 1) if len(dec) else math.log10(1000)
    # envelope: modulation, sparseness, dynamics
    e = np.sqrt(np.mean(frames_db(x, 441, 441) ** 2, axis=1)) + 1e-9          # 10 ms
    ed = e - e.mean()
    M = np.abs(np.fft.rfft(ed * np.hanning(len(ed)))) ** 2
    fm = np.fft.rfftfreq(len(ed), 0.01)
    tot = M[(fm > 0.2) & (fm < 50)].sum() + 1e-18
    modlo = 10 * math.log10(M[(fm >= 0.5) & (fm < 4)].sum() / tot + 1e-9)
    modhi = 10 * math.log10(M[(fm >= 4) & (fm < 32)].sum() / tot + 1e-9)
    kurt = math.log10(np.mean(ed ** 4) / (np.mean(ed ** 2) ** 2 + 1e-18) + 1e-9)
    l100 = 20 * np.log10(np.sqrt(np.mean(frames_db(x, 4410, 4410) ** 2, axis=1)) + 1e-9)
    dyn = float(np.percentile(l100, 90) - np.percentile(l100, 10))
    return {"cent": cent, "slope": slope, "low": low, "flat": flat, "rate": math.log10(1 + rate), "ioicv": ioicv,
            "lvlsd": lvlsd, "decay": decay, "modlo": modlo, "modhi": modhi, "kurt": kurt, "dyn": dyn}


def label_of(path):
    idx = os.path.join(REF, "index.tsv")
    rel = os.path.relpath(path, REF)
    if os.path.exists(idx):
        for line in open(idx):
            p = line.rstrip("\n").split("\t")
            if p and p[0] == rel and len(p) > 1:
                return p[1].replace("candidate: ", "")
    return rel


def refs_of(dirs):
    out = []
    for d in dirs:
        for p in sorted(glob.glob(os.path.join(REF, d, "*.wav")) + glob.glob(os.path.join(REF, d, "*", "ref.wav"))):
            try:
                x = A.load(p)
            except Exception:
                continue
            if len(x) < FS * 3:
                continue
            x = x[: FS * 60]
            # skip a silent lead-in: from the first 100 ms frame within 30 dB of the loudest
            l = 20 * np.log10(np.sqrt(np.mean(frames_db(x, 4410, 4410) ** 2, axis=1)) + 1e-9)
            k0 = int(np.argmax(l > l.max() - 30))
            out.append((label_of(p), d, x[k0 * 4410:]))
    return out


# ---- the renders (the committed engines: a git HEAD export built in build/cover)
def txrender():
    exe = os.path.join(OUT, "txrender")
    head = subprocess.run(["git", "-C", ROOT, "rev-parse", "HEAD"], capture_output=True, text=True).stdout.strip()
    stamp = os.path.join(OUT, "txrender.head")
    if os.path.exists(exe) and os.path.exists(stamp) and open(stamp).read().strip() == head:
        return exe
    src = os.path.join(OUT, "src")
    subprocess.run(["rm", "-rf", src], check=True)
    os.makedirs(src, exist_ok=True)
    tar = subprocess.run(["git", "-C", ROOT, "archive", "HEAD", "engines", "bench"], capture_output=True, check=True).stdout
    subprocess.run(["tar", "-x", "-C", src], input=tar, check=True)
    files = ["bench/txrender.c", "bench/tx_host.c", "bench/tx_script.c"] + sorted(
        os.path.relpath(p, src) for p in glob.glob(os.path.join(src, "engines", "*.c")))
    subprocess.run(["cc", "-std=c99", "-D_DARWIN_C_SOURCE", "-D_DEFAULT_SOURCE", "-O2", "-Iengines", "-Ibench", "-o", exe]
                   + files + ["-lm"], cwd=src, check=True)
    open(stamp, "w").write(head)
    return exe


def render(exe, preset, knobs, seed):
    os.makedirs(os.path.join(OUT, "tmp"), exist_ok=True)
    name = f"p{preset}_" + "_".join(f"{k[:2]}{v}" for k, v in knobs.items()) + f"_s{seed}"
    sc = os.path.join(OUT, "tmp", name + ".txt"); wav = os.path.join(OUT, "tmp", name + ".wav")
    lines = [f"seed {seed}", f"0 param PRESET {preset}"] + [f"0 param {k} {v}" for k, v in knobs.items()]
    lines += ["0 on C4 100", "22000 off C4", "22500 end"]
    open(sc, "w").write("\n".join(lines) + "\n")
    subprocess.run([exe, "source", sc, "-o", wav], capture_output=True, check=True)
    x = A.load(wav)
    os.remove(wav); os.remove(sc)
    return x[FS * 2: FS * 22]


def sweep(hosted):
    """the controls swept: INTENSITY x PROXIMITY x (the character knobs: particle presets only) x LIFE"""
    out = []
    for i in (0, 32, 64, 96, 127):
        for p in (24, 64, 110):
            chars = [{}] if hosted else [{}, {"SIZE": 20}, {"SIZE": 110}, {"TONE": 20, "HARD": 20}, {"TONE": 110, "HARD": 110},
                                         {"SPREAD": 0}, {"SPACING": 0}, {"SPACING": 127}]
            for c in chars:
                k = {"INTENSITY": i, "PROXIMITY": p}; k.update(c)
                out.append(k)
    for i in (32, 96):                                   # LIFE still and alive
        out.append({"INTENSITY": i, "PROXIMITY": 64, "LIFE": 0})
        out.append({"INTENSITY": i, "PROXIMITY": 64, "LIFE": 127, "TIME": 0})
    return out


# ---- coverage
def main():
    args = sys.argv[1:]
    js = None; plots = os.path.join(ROOT, ".private", "research", "plots")
    if "--json" in args: js = args[args.index("--json") + 1]; del args[args.index("--json"):args.index("--json") + 2]
    if "--plots" in args: plots = args[args.index("--plots") + 1]; del args[args.index("--plots"):args.index("--plots") + 2]
    names = args or list(PRESETS)
    exe = txrender()
    # the scale: the spread of every reference of every family (cached)
    cache = os.path.join(OUT, "ref_features.json")
    reff = json.load(open(cache)) if os.path.exists(cache) else {}
    for nm, (_, dirs, _) in PRESETS.items():
        for lab, d, x in refs_of(dirs):
            key = f"{d}|{lab}"
            if key not in reff:
                reff[key] = features(x)
    json.dump(reff, open(cache, "w"), indent=1)
    scale = np.array([TOL[f] for f in FEATS])
    report = {}
    for nm in names:
        pre, dirs, hosted = PRESETS[nm]
        refs = [(k.split("|", 1)[1], k.split("|", 1)[0], np.array([v[f] for f in FEATS])) for k, v in reff.items()
                if k.split("|", 1)[0] in dirs]
        pts, knobs = [], []
        head = open(os.path.join(OUT, "txrender.head")).read().strip()
        sc_path = os.path.join(OUT, f"sweep-{nm}.json")
        cached = json.load(open(sc_path)) if os.path.exists(sc_path) else {}
        if cached.get("head") != head: cached = {"head": head, "pts": {}}
        for kn in sweep(hosted):
            key = json.dumps(kn, sort_keys=True)
            if key not in cached["pts"]:
                cached["pts"][key] = features(render(exe, pre, kn, 3))
            pts.append(np.array([cached["pts"][key][f] for f in FEATS])); knobs.append(kn)
        json.dump(cached, open(sc_path, "w"))
        P = np.array(pts) / scale
        rows = []
        for lab, d, v in refs:
            z = v / scale
            dist = np.sqrt(np.mean((P - z) ** 2, axis=1))
            j = int(np.argmin(dist))
            gap = z - P[j]
            reached = dist[j] <= RMS_IN and np.abs(gap).max() <= MAX_IN
            miss = [(FEATS[q], float(gap[q])) for q in np.argsort(-np.abs(gap)) if abs(gap[q]) > 1.0][:4]
            rows.append({"ref": lab, "folder": d, "reached": bool(reached), "dist": float(dist[j]), "nearest": knobs[j],
                         "miss": miss, "ref_feat": {f: float(v[q]) for q, f in enumerate(FEATS)},
                         "near_feat": {f: float(pts[j][q]) for q, f in enumerate(FEATS)}})
        report[nm] = {"preset": pre, "refs": rows, "sweep": len(pts),
                      "range": {f: [float(min(p[q] for p in pts)), float(max(p[q] for p in pts))] for q, f in enumerate(FEATS)}}
        n_in = sum(r["reached"] for r in rows)
        print(f"{nm:9s} {n_in}/{len(rows)} references reached ({len(pts)} renders)")
        for r in rows:
            m = ", ".join(f"{f} {'+' if g > 0 else ''}{g:.1f}" for f, g in r["miss"])
            print(f"   {'IN ' if r['reached'] else 'OUT'} {r['dist']:4.1f}  {r['ref'][:58]:58s} {m}")
        plot(nm, refs, pts, knobs, scale, plots)
    if js:
        json.dump(report, open(js, "w"), indent=1)


def plot(nm, refs, pts, knobs, scale, plots):
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    os.makedirs(plots, exist_ok=True)
    R = np.array([v / scale for _, _, v in refs]); P = np.array(pts) / scale
    Z = np.vstack([R, P]); mu = Z.mean(0)
    U, s, Vt = np.linalg.svd(Z - mu, full_matrices=False)
    r2 = (R - mu) @ Vt[:2].T; p2 = (P - mu) @ Vt[:2].T
    fig = plt.figure(figsize=(14, 6))
    ax = fig.add_subplot(1, 2, 1)
    sc = ax.scatter(p2[:, 0], p2[:, 1], c=[k["INTENSITY"] for k in knobs], cmap="viridis", s=14, alpha=0.7, label="engine sweep")
    ax.scatter(r2[:, 0], r2[:, 1], c="C3", marker="*", s=160, label="references")
    for (lab, _, _), (a, b) in zip(refs, r2):
        ax.annotate(lab[:28], (a, b), fontsize=7, color="C3", xytext=(3, 3), textcoords="offset points")
    plt.colorbar(sc, ax=ax, label="INTENSITY")
    top = lambda v: ", ".join(f"{FEATS[i]} {v[i]:+.2f}" for i in np.argsort(-np.abs(v))[:3])
    ax.set_xlabel("PC1: " + top(Vt[0])); ax.set_ylabel("PC2: " + top(Vt[1]))
    ax.set_title(f"{nm}: references vs the engine's reach (feature space, PCA)"); ax.legend(fontsize=8)
    ax2 = fig.add_subplot(1, 2, 2)
    for q, f in enumerate(FEATS):
        ax2.scatter(P[:, q], np.full(len(P), q) + 0.15, s=6, c="C0", alpha=0.4)
        ax2.scatter(R[:, q], np.full(len(R), q) - 0.15, s=40, c="C3", marker="*")
    ax2.set_yticks(range(len(FEATS))); ax2.set_yticklabels(FEATS); ax2.set_xlabel("feature (units: its tolerance)")
    ax2.set_title("per feature: engine (dots) vs references (stars)")
    plt.tight_layout(); plt.savefig(os.path.join(plots, f"cover-{nm}.png"), dpi=80); plt.close(fig)


if __name__ == "__main__":
    main()
