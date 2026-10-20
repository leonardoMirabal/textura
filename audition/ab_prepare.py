# SPDX-License-Identifier: GPL-3.0-only
"""A/B data for the player: for every engine, a render of ours at its defaults and the local reference recordings,
loudness-matched, with spectrograms of both on the same scale.

    make tx-ab          (= .local/venv/bin/python -I audition/ab_prepare.py; needs build/bench/txrender)
    TX_REF=/path/to/.local/reference python -I audition/ab_prepare.py     (from a checkout without .local)

Reads the reference recordings from .local/reference/<dir>/ (*.wav, or */ref.wav with a SOURCE.txt; labels, URLs
and licences from .local/reference/index.tsv when present). Writes build/audition/ab/: ours/<engine>.wav,
ref/<engine>/<name>.wav (a 10 s excerpt: the loudest 10 s), png/*.png and manifest.json. Everything stays under
build/ and .local/: references are never committed, never pushed and never played by an engine (the player only
plays these files back for listening, on localhost).

Ours: a key (C4, v100) held 8 s if the engine sustains (a texture), else four presses 2.5 s apart (an event engine),
10 s in all, a fixed seed. Loudness: BS.1770 integrated (gated); each reference gets the gain that brings it to ours
(at most so far that its peak stays under 0 dBFS; then the gap is shown). Spectrograms: 10 s, 20 Hz-20 kHz on a log
axis, power in dB on one fixed scale (-120 .. -20 dBFS per bin), the reference after its gain: what differs in level
between the two images differs in the sound."""
import glob
import json
import os
import re
import shutil
import subprocess
import sys
import wave

import numpy as np
from scipy.signal import lfilter, resample_poly

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REF = os.environ.get("TX_REF", os.path.join(ROOT, ".local", "reference"))   # (another checkout: TX_REF=...)
OUT = os.path.join(ROOT, "build", "audition", "ab")
TXR = os.path.join(ROOT, "build", "bench", "txrender")
FS = 44100
DUR = 10.0

# which reference folders belong to which engine (folder names under .local/reference)
REFDIRS = {
    "bees": ["bees"], "candle": ["candle"], "mineral": ["mineral", "krint"], "pebble": ["pebble"], "sandsong": ["sandsong"],
    "aeolian": ["aeolian"], "sand": ["sand"], "rainsurf": ["rainsurf"], "rainwater": ["rainwater"],
    "stream": ["stream"], "surf": ["surf"], "storm": ["storm"], "crickets": ["crickets"], "cicadas": ["cicadas"],
    "katydids": ["katydids"], "crumple": ["crumple"], "fire": ["fire"], "stonedrum": ["stonedrum", "stone"],
    "stonecloud": ["stone"], "minerals": ["minerals", "stone"], "glass": ["glass"], "wood": ["wood"], "ice": ["ice"],
    # step 4: candidates (cand_*.wav) for the user to choose from by ear
    "drip": ["drip"], "sparks": ["sparks"], "gravel": ["gravel"], "rainstick": ["rainstick"], "thunder": ["thunder"],
    "lakeice": ["lakeice"], "spring": ["spring"], "chimes": ["chimes"], "rockfall": ["rockfall"], "plate": ["plate"],
}


def engines():
    src = open(os.path.join(ROOT, "engines", "tx_engines.c")).read()
    return re.findall(r"&tx_eng_(\w+),", src)


def load(path):
    w = wave.open(path, "rb")
    n, ch, sw, fs = w.getnframes(), w.getnchannels(), w.getsampwidth(), w.getframerate()
    b = w.readframes(n)
    if sw == 2:
        x = np.frombuffer(b, "<i2").astype(np.float64) / 32768.0
    elif sw == 3:
        a = np.frombuffer(b, np.uint8).reshape(-1, 3).astype(np.int32)
        x = ((a[:, 0] | (a[:, 1] << 8) | (a[:, 2] << 16)) << 8 >> 8) / 8388608.0
    elif sw == 4:
        x = np.frombuffer(b, "<i4").astype(np.float64) / 2147483648.0
    else:
        x = (np.frombuffer(b, np.uint8).astype(np.float64) - 128) / 128.0
    x = x.reshape(-1, ch).mean(axis=1)
    if fs != FS:
        g = np.gcd(FS, fs)
        x = resample_poly(x, FS // g, fs // g)
    return x


def save(path, x):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    y = np.clip(np.round(x * 32767), -32768, 32767).astype("<i2")
    w = wave.open(path, "wb")
    w.setnchannels(1); w.setsampwidth(2); w.setframerate(FS); w.writeframes(y.tobytes()); w.close()


def kweight(x):
    # BS.1770's K-weighting at 44.1 kHz (its analog prototypes: a +4 dB shelf above ~1.7 kHz, a ~38 Hz high-pass)
    G, f0, Q = 3.999843853973347, 1681.974450955533, 0.7071752369554196
    K = np.tan(np.pi * f0 / FS); Vh = 10 ** (G / 20); Vb = Vh ** 0.4996667741545416
    a0 = 1 + K / Q + K * K
    b1 = [(Vh + Vb * K / Q + K * K) / a0, 2 * (K * K - Vh) / a0, (Vh - Vb * K / Q + K * K) / a0]
    a1 = [1, 2 * (K * K - 1) / a0, (1 - K / Q + K * K) / a0]
    f0, Q = 38.13547087602444, 0.5003270373238773
    K = np.tan(np.pi * f0 / FS)
    a2 = [1, 2 * (K * K - 1) / (1 + K / Q + K * K), (1 - K / Q + K * K) / (1 + K / Q + K * K)]
    return lfilter([1, -2, 1], a2, lfilter(b1, a1, x))


def lufs(x):
    y = kweight(x); n = int(0.4 * FS); h = int(0.1 * FS)
    if len(y) < n:
        return -120.0
    z = np.array([np.mean(y[i:i + n] ** 2) for i in range(0, len(y) - n + 1, h)])
    z1 = z[-0.691 + 10 * np.log10(z + 1e-20) > -70]
    if not len(z1):
        return -120.0
    g = -0.691 + 10 * np.log10(z1.mean()) - 10
    z2 = z1[-0.691 + 10 * np.log10(z1 + 1e-20) > g]
    return float(-0.691 + 10 * np.log10(z2.mean() + 1e-20))


def loudest(x, n):
    # the loudest n samples (1 s steps), padded with silence when shorter
    if len(x) <= n:
        return np.concatenate([x, np.zeros(n - len(x))])
    s = np.cumsum(np.concatenate([[0.0], x ** 2]))
    starts = np.arange(0, len(x) - n + 1, FS)
    e = s[starts + n] - s[starts]
    i = int(starts[int(np.argmax(e))])
    return x[i:i + n]


def spectrogram(x, path, title):
    nf, hop = 4096, 512
    win = np.hanning(nf)
    frames = 1 + (len(x) - nf) // hop
    S = np.empty((nf // 2 + 1, frames))
    for k in range(frames):
        S[:, k] = np.abs(np.fft.rfft(x[k * hop:k * hop + nf] * win)) ** 2
    S = 10 * np.log10(S / (np.sum(win) ** 2 / 4) + 1e-20)      # dBFS per bin (a full-scale sine = 0)
    f = np.fft.rfftfreq(nf, 1 / FS)
    fl = np.geomspace(20, 20000, 300)                          # log frequency rows
    rows = np.empty((len(fl), frames))
    for k in range(frames):
        rows[:, k] = np.interp(fl, f, S[:, k])
    fig = plt.figure(figsize=(6.4, 2.7), dpi=100)
    ax = fig.add_axes([0.09, 0.17, 0.80, 0.72])
    im = ax.imshow(rows, origin="lower", aspect="auto", cmap="magma", vmin=-120, vmax=-20,
                   extent=[0, len(x) / FS, np.log10(20), np.log10(20000)], interpolation="nearest")
    ticks = [20, 50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000]
    ax.set_yticks(np.log10(ticks))
    ax.set_yticklabels(["20", "50", "100", "200", "500", "1k", "2k", "5k", "10k", "20k"], fontsize=7)
    ax.tick_params(axis="x", labelsize=7)
    ax.set_xlabel("s", fontsize=7, labelpad=1)
    ax.set_title(title, fontsize=8, loc="left")
    cax = fig.add_axes([0.905, 0.17, 0.02, 0.72])
    cb = fig.colorbar(im, cax=cax)
    cb.ax.tick_params(labelsize=6)
    cb.set_label("dBFS", fontsize=7)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    fig.savefig(path)
    plt.close(fig)


def render_ours(name, idx):
    tmp = os.path.join(OUT, "tmp")
    os.makedirs(tmp, exist_ok=True)
    held = os.path.join(tmp, "held.txt")
    open(held, "w").write("0 on 60 100\n8000 off 60\n10000 end\n")
    wav = os.path.join(tmp, name + "-held.wav")
    subprocess.run([TXR, name, held, "-o", wav, "-s", "1"], check=True, capture_output=True)
    x = load(wav)[:int(DUR * FS)]
    r = lambda a, b: 10 * np.log10(np.mean(x[int(a * FS):int(b * FS)] ** 2) + 1e-20)
    if r(6, 8) > -60 and r(6, 8) > r(0.5, 2.5) - 20:
        return x, "texture: a key held 8 s, then released"
    taps = os.path.join(tmp, "taps.txt")
    open(taps, "w").write("".join(f"{t} on 60 100\n{t + 150} off 60\n" for t in (0, 2500, 5000, 7500)) + "10000 end\n")
    wav = os.path.join(tmp, name + "-taps.wav")
    subprocess.run([TXR, name, taps, "-o", wav, "-s", "1"], check=True, capture_output=True)
    return load(wav)[:int(DUR * FS)], "event: four presses 2.5 s apart"


def meta_for(path):
    rel = os.path.relpath(path, REF)
    d = os.path.dirname(path)
    label, url, lic = os.path.splitext(os.path.basename(path))[0], "", ""
    if os.path.basename(path) == "ref.wav":
        label = os.path.basename(d)
    idx = os.path.join(REF, "index.tsv")
    if os.path.exists(idx):
        for line in open(idx):
            p = line.rstrip("\n").split("\t")
            if len(p) >= 4 and p[0] == rel:
                label, url, lic = p[1] or label, p[2], p[3]
    src = os.path.join(d, "SOURCE.txt")
    if os.path.basename(path) == "ref.wav" and os.path.exists(src):
        s = [l.strip() for l in open(src) if l.strip()]
        if s:
            url = s[0]
        if len(s) > 1 and label == os.path.basename(d):
            label = os.path.basename(d) + " (" + s[1].replace("Freesound - ", "")[:60] + ")"
        if len(s) > 2:
            lic = s[2]
    return label, url, lic


def main():
    if not os.path.exists(TXR):
        sys.exit("no build/bench/txrender: make build/bench/txrender first")
    if os.path.isdir(OUT):
        shutil.rmtree(OUT)
    man = {"duration": DUR, "scale_db": [-120, -20], "engines": {}}
    names = engines()
    for i, name in enumerate(names):
        x, kind = render_ours(name, i)
        lo = lufs(x)
        save(os.path.join(OUT, "ours", name + ".wav"), x)
        spectrogram(x, os.path.join(OUT, "png", name + "-ours.png"), f"#{i + 1} {name}: ours ({lo:.1f} LUFS)")
        e = {"num": i + 1, "ours": {"wav": f"ab/ours/{name}.wav", "png": f"ab/png/{name}-ours.png", "lufs": round(lo, 1),
                                    "kind": kind}, "refs": []}
        files = []
        for d in REFDIRS.get(name, []):
            files += sorted(glob.glob(os.path.join(REF, d, "*.wav"))) + sorted(glob.glob(os.path.join(REF, d, "*", "ref.wav")))
        for k, f in enumerate(files):
            try:
                r = loudest(load(f), int(DUR * FS))
            except Exception as ex:                          # a file the wave module cannot read: skipped
                print(f"skip {f}: {ex}")
                continue
            lr = lufs(r)
            if lr <= -100:
                continue
            g = lo - lr
            pk = 20 * np.log10(np.abs(r).max() + 1e-12)
            gap = 0.0
            if pk + g > -0.5:                                  # the peak would clip: as loud as it can go
                gap = round(g - (-0.5 - pk), 1)
                g = -0.5 - pk
            label, url, lic = meta_for(f)
            tag = f"r{k + 1}"
            save(os.path.join(OUT, "ref", name, tag + ".wav"), r)
            spectrogram(r * 10 ** (g / 20), os.path.join(OUT, "png", f"{name}-{tag}.png"),
                        f"reference {k + 1}: {label[:48]} (matched{'' if not gap else f', {gap} dB short'})")
            e["refs"].append({"name": label, "wav": f"ab/ref/{name}/{tag}.wav", "png": f"ab/png/{name}-{tag}.png",
                              "lufs": round(lr, 1), "gain_db": round(g, 1), "short_db": gap, "url": url, "licence": lic})
        man["engines"][name] = e
        print(f"#{i + 1:2d} {name:10s} ours {lo:6.1f} LUFS ({kind.split(':')[0]}), {len(e['refs'])} references")
    shutil.rmtree(os.path.join(OUT, "tmp"), ignore_errors=True)
    json.dump(man, open(os.path.join(OUT, "manifest.json"), "w"), indent=1)
    print(f"wrote {OUT}/manifest.json")


if __name__ == "__main__":
    main()
