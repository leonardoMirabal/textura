#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Hardware-in-the-loop suite for Textura's FM-1: run it after every install.

  hil_suite.py [--only clicks,binaural,ms,stereo,activity] [--presets F007,STEADY RAIN,...] [--seconds 20]
               [--save DIR] [--wav DIR] [--dry-run] [--no-restore]

Rig (fixed): the FM-1's USB MIDI port "Textura" (the only MIDI port ever opened), its USB audio input "Textura FM-1"
(2 ch, 44.1 kHz) and its headphone jack into the MOTU M6 inputs 5/6 (device "M6", 48 kHz, channel indexes 4 and 5).
The M6's gain is never touched. NRPN and program change as in docs/MANUAL.md (12. MIDI): NRPN on channel 5, program
change on channel 1.

Checks (each prints ok / FAIL with its numbers; the exit code is the number of failures):
  clicks    busy presets x OUTPUT SPEAKERS / HEADPHONES: jack and USB recorded at once; clicks, repeated blocks, dropouts
  binaural  all lanes hard left: far ear lag (in the lows, 20 .. 80 Hz: its highs come on time) and level per HEAD SIZE;
            AMOUNT 0 equals SPEAKERS
  ms        M/S section: MID off, SIDE off, WIDTH 0, MONO, default
  stereo    per preset after its load: MID against SIDE, |L-R|, correlation
  activity  per lane soloed, three windows: a lane that should play must not go silent
At the end the unit is restored to its defaults and what was restored is printed.

--wav DIR re-analyses recordings (names `<check>__<case>__usb.wav` / `__jack.wav`, as --save writes them): nothing
touches the hardware. --dry-run prints the plan (the cases, the MIDI that would be sent) and touches nothing.
The analysis (click, repeat, dropout, lag, M/S) is plain numpy in the functions below; tests/hil_suite_check.py
tests it on synthetic signals.
"""
import argparse
import os
import re
import sys
import time
import wave

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

PORT = "Textura"
USB_DEV, USB_SR = "Textura FM-1", 44100
JACK_DEV, JACK_SR, JACK_CH = "M6", 48000, (4, 5)
SETTLE = 0.5                 # s cut from the start of every recording
BLOCK = 256                  # the firmware's block, frames at 44.1 kHz

# NRPN values (0..127 spread over the range; docs/MANUAL.md)
G_OUTPUT, G_AMOUNT, G_HEAD = 45, 46, 47
G_MID, G_SIDE, G_WIDTH, G_MONO = 96, 97, 98, 99
OUT_SPEAKERS, OUT_HEADPHONES = 0, 127
AMOUNT_100, AMOUNT_0 = 127, 0
HEAD = {"SMALL": 0, "MEDIUM": 64, "LARGE": 127}
HEAD_LAG = {"SMALL": 25, "MEDIUM": 29, "LARGE": 33}      # far ear lag at 44.1 kHz
MS_DEFAULT = {G_MID: 115, G_SIDE: 115, G_WIDTH: 64, G_MONO: 0}

# thresholds (the reasons are in the report of the change and in AGENTS.md)
CLICK_K, CLICK_FLOOR = 8.0, 0.003     # second difference over k x its local noise and over an absolute floor
REPEAT_R, REPEAT_TONAL = 0.90, 0.05    # adjacent-block correlation; more than this fraction of windows: tonal, skipped
DROP_WIN, DROP_PEAK, DROP_REF = 96, 0.01, 10 ** (-50 / 20)
LAG_TOL = 2                            # samples
FAR_BINAURAL = (-20.0, -10.0)          # dB re the near ear
FAR_SPEAKERS = -25.0                   # dB re the near ear: hard left in SPEAKERS leaks nothing to the right
MS_GONE = -25.0                        # dB re the default level
STEREO_MS, STEREO_LR, STEREO_CORR = 3.0, 3.0, 0.3
SILENT_DB = -70.0


# ---------------------------------------------------------------- analysis (pure numpy)

def db(v):
    return 20 * np.log10(v + 1e-12)


def rms_db(x):
    x = np.asarray(x, dtype=np.float64)
    return float(db(np.sqrt(np.mean(x * x)))) if x.size else -240.0


def _cols(x):
    x = np.asarray(x, dtype=np.float64)
    return [x] if x.ndim == 1 else [x[:, i] for i in range(x.shape[1])]


def _merge(idx, gap):
    out = []
    for i in idx:
        if not out or i - out[-1] > gap:
            out.append(int(i))
    return out


def find_clicks(x, k=CLICK_K, floor=CLICK_FLOOR, blk=512):
    """sample indexes of spikes of the second difference over k x the local noise (block median of |d2| as a sigma, the loudest
    of the block and its two neighbours) and over an absolute floor; per channel, merged within 16 samples"""
    found = []
    for c in _cols(x):
        if len(c) < 4 * blk:
            continue
        d2 = np.abs(c[2:] - 2 * c[1:-1] + c[:-2])
        n = len(d2) // blk
        med = np.median(d2[:n * blk].reshape(n, blk), axis=1) / 0.6745
        loc = np.maximum(med, np.maximum(np.r_[med[1:], med[-1]], np.r_[med[0], med[:-1]]))
        ref = np.repeat(loc, blk)
        ref = np.r_[ref, np.full(len(d2) - len(ref), loc[-1])]
        hit = np.nonzero((d2 > k * ref) & (d2 > floor))[0]
        found += [i + 1 for i in _merge(hit, 16)]
    return sorted(found)


def find_repeats(x, block=BLOCK, thr=REPEAT_R):
    """sample indexes where a window of `block` samples repeats the one before (normalised correlation at a lag of
    block-1 .. block+1 over thr, window energy over -60 dB). Returns (list, windows_checked, tonal): tonal when so
    many windows correlate (a held tone) that repeats cannot be told from the signal."""
    out, total, tonal = [], 0, False
    step = max(block // 8, 1)
    for c in _cols(x):
        n = len(c)
        if n < 4 * block:
            continue
        starts = np.arange(block + 1, n - block - 1, step)
        w = np.lib.stride_tricks.sliding_window_view(c, block)       # w[i] = c[i:i+block]
        cur = w[starts]
        e = np.sqrt(np.sum(cur * cur, axis=1))
        best = np.zeros(len(starts))
        for lag in (block - 1, block, block + 1):
            prev = w[starts - lag]
            pe = np.sqrt(np.sum(prev * prev, axis=1))
            r = np.sum(cur * prev, axis=1) / (e * pe + 1e-18)
            best = np.maximum(best, r)
        ok = e / np.sqrt(block) > 10 ** (-60 / 20)
        total += int(ok.sum())
        hit = ok & (best > thr)
        if ok.sum() and hit.sum() / ok.sum() > REPEAT_TONAL:
            tonal = True
            continue
        out += [int(s) for s in _merge(starts[hit], block)]
    return sorted(out), total, tonal


def find_dropouts(x, win=DROP_WIN, peak=DROP_PEAK, refmin=DROP_REF, span=2048):
    """sample indexes of windows of `win` samples whose peak is under `peak` x the rms of the span around them
    on both sides (span samples each) while that rms is over refmin: a gap shorter than span in a playing signal"""
    out = []
    for c in _cols(x):
        n = len(c)
        if n < 4 * span:
            continue
        a = np.abs(c)
        cs = np.r_[0.0, np.cumsum(c * c)]
        starts = np.arange(span, n - span - win, win // 2)
        left = np.sqrt((cs[starts] - cs[starts - span]) / span)
        right = np.sqrt((cs[starts + win + span] - cs[starts + win]) / span)
        ref = np.minimum(left, right)          # signal on both sides: a gap, not the end of a sound
        pk = np.array([a[s:s + win].max() for s in starts])
        hit = (ref > refmin) & (pk < peak * ref)
        out += _merge(starts[hit], win)
    return sorted(out)


def lag_of(l, r, maxlag=64, n=1 << 16):
    """samples by which r lags l (peak of the cross-correlation within +-maxlag)"""
    l = np.asarray(l[:n], dtype=np.float64)
    r = np.asarray(r[:n], dtype=np.float64)
    m = min(len(l), len(r))
    l, r = l[:m] - l[:m].mean(), r[:m] - r[:m].mean()
    L, R = np.fft.rfft(l, 2 * m), np.fft.rfft(r, 2 * m)
    c = np.fft.irfft(R * np.conj(L))
    c = np.concatenate([c[-maxlag:], c[:maxlag + 1]])
    return int(np.argmax(c)) - maxlag


def lag_lo(l, r, sr, band=(20.0, 80.0), n=1 << 16):
    """samples by which r lags l in the lows: the slope of the cross-spectrum's phase over band (Hz), weighted by its
    magnitude. HEADPHONES delays the far ear only below its crossover (engines/tx_core_mix.c TX_XO: 380 Hz at a hard
    pan), its highs come on time, so a broadband cross-correlation peaks at 0"""
    l = np.asarray(l[:n], dtype=np.float64)
    r = np.asarray(r[:n], dtype=np.float64)
    m = min(len(l), len(r))
    l, r = l[:m] - l[:m].mean(), r[:m] - r[:m].mean()
    x = np.fft.rfft(r, 2 * m) * np.conj(np.fft.rfft(l, 2 * m))
    f = np.fft.rfftfreq(2 * m, 1.0 / sr)
    k = (f >= band[0]) & (f <= band[1])
    if not k.any():
        return 0.0
    ph = np.unwrap(np.angle(x[1:]))[k[1:]]
    w, om = np.abs(x[1:])[k[1:]], 2 * np.pi * f[1:][k[1:]] / sr
    return float(-np.sum(w * ph * om) / max(np.sum(w * om * om), 1e-30))


def stereo_stats(x):
    """left, right, mid, side levels (dBFS) and the correlation of an (n, 2) block"""
    l, r = np.asarray(x[:, 0], dtype=np.float64), np.asarray(x[:, 1], dtype=np.float64)
    m, s = (l + r) / 2, (l - r) / 2
    den = np.sqrt(np.sum(l * l) * np.sum(r * r)) + 1e-18
    return dict(l=rms_db(l), r=rms_db(r), mid=rms_db(m), side=rms_db(s), corr=float(np.sum(l * r) / den))


def block_for(sr):
    return int(round(BLOCK * sr / USB_SR))


def analyse_clicks(x, sr):
    rep, nwin, tonal = find_repeats(x, block_for(sr), REPEAT_R)
    return dict(clicks=find_clicks(x), repeats=rep, tonal=tonal, drops=find_dropouts(x), level=rms_db(x))


# ---------------------------------------------------------------- WAV files (24-bit PCM, stdlib only)

def write_wav(path, x, sr):
    x = np.clip(np.asarray(x, dtype=np.float64), -1, 1 - 2 ** -23)
    if x.ndim == 1:
        x = x[:, None]
    v = np.round(x * 8388608).astype(np.int32).reshape(-1)
    b = np.stack([v & 255, (v >> 8) & 255, (v >> 16) & 255], axis=1).astype(np.uint8).tobytes()
    with wave.open(path, "wb") as w:
        w.setnchannels(x.shape[1]); w.setsampwidth(3); w.setframerate(sr); w.writeframes(b)


def read_wav(path):
    with wave.open(path, "rb") as w:
        ch, sw, sr, n = w.getnchannels(), w.getsampwidth(), w.getframerate(), w.getnframes()
        raw = w.readframes(n)
    if sw == 3:
        b = np.frombuffer(raw, dtype=np.uint8).reshape(-1, 3).astype(np.int32)
        v = b[:, 0] | (b[:, 1] << 8) | (b[:, 2] << 16)
        v = np.where(v & 0x800000, v - 0x1000000, v) / 8388608.0
    elif sw == 2:
        v = np.frombuffer(raw, dtype="<i2") / 32768.0
    elif sw == 4:
        v = np.frombuffer(raw, dtype="<i4") / 2147483648.0
    else:
        raise ValueError(f"{path}: {8 * sw}-bit PCM not handled")
    return v.reshape(-1, ch).astype(np.float64), sr


# ---------------------------------------------------------------- presets (firmware/presets.txt)

def parse_presets(path=None):
    """[dict(num, name, cat, lanes=[None | dict(n, a, v, muted)])], num 0 = F001"""
    path = path or os.path.join(ROOT, "firmware", "presets.txt")
    out, cat = [], ""
    for line in open(path, encoding="utf-8"):
        line = line.rstrip("\n")
        if not line.strip() or line.startswith("#"):
            continue
        if line.startswith("="):
            cat = line[1:].strip()
            continue
        head, _, rest = line.partition("|")
        name = re.sub(r"\s+fx\d\s*$", "", head).strip()
        lanes = []
        for part in rest.split("|")[:4]:
            part = part.strip()
            m = re.match(r"[A-Z]:.*?\bn(\d+)\s+a(\d+)\b", part)
            if not m:
                lanes.append(None)
                continue
            v = re.search(r"\bv(\d+)\b", part)
            lanes.append(dict(n=int(m.group(1)), a=int(m.group(2)), v=int(v.group(1)) if v else 100,
                              muted=bool(re.search(r"\sm\s*$", part))))
        out.append(dict(num=len(out), name=name, cat=cat, lanes=lanes + [None] * (4 - len(lanes))))
    return out


def preset_tag(p):
    return "F%03d" % (p["num"] + 1)


def density(p):
    return sum(l["n"] * l["a"] for l in p["lanes"] if l and not l["muted"])


BUSY_CATS = ("RAIN", "STORM", "FIRE", "STONE & SAND")


def default_presets(allp, count=10):
    """the busiest presets of the rain, storm, fire and sand families, at most 3 per family"""
    picked = []
    for cat in BUSY_CATS:
        ps = sorted((p for p in allp if p["cat"] == cat), key=lambda p: -density(p))
        picked += ps[:3]
    picked.sort(key=lambda p: -density(p))
    return sorted(picked[:count], key=lambda p: p["num"])


def pick_presets(allp, spec, count=10):
    if not spec:
        return default_presets(allp, count)
    out = []
    for s in (t.strip() for t in spec.split(",") if t.strip()):
        m = re.fullmatch(r"[Ff]?(\d{1,3})", s)
        if m and 1 <= int(m.group(1)) <= len(allp):
            out.append(allp[int(m.group(1)) - 1])
            continue
        hit = [p for p in allp if p["name"].lower() == s.lower()] or [p for p in allp if s.lower() in p["name"].lower()]
        if not hit:
            raise SystemExit(f"no preset {s!r}")
        out.append(hit[0])
    return out


# ---------------------------------------------------------------- the rig

class Report:
    def __init__(self, save=None):
        self.fails, self.lines, self.save = 0, [], save

    def say(self, text):
        print(text, flush=True)
        self.lines.append(text)

    def ok(self, name, good, text=""):
        if not good:
            self.fails += 1
        self.say(f"{'ok  ' if good else 'FAIL'}  {name}  {text}".rstrip())

    def info(self, text):
        self.say(f"      {text}")

    def skip(self, name, why):
        self.say(f"skip  {name}  {why}")


class Live:
    """MIDI out to "Textura" and the two recordings (needs mido, sounddevice)"""

    def __init__(self):
        import mido
        import sounddevice as sd
        import tx_ports
        self.mido, self.sd = mido, sd
        names = tx_ports.pick(mido.get_output_names())
        if not names:
            raise SystemExit("hil: " + tx_ports.why_none(mido.get_output_names()))
        self.out = mido.open_output(names[0])
        self.port = names[0]
        devs = [d["name"] for d in sd.query_devices()]
        self.usb = USB_DEV if USB_DEV in devs else None
        self.jack = JACK_DEV if JACK_DEV in devs else None
        if not self.usb:
            print(f"hil: no USB audio device {USB_DEV!r}: USB analysis skipped")
        if not self.jack:
            print(f"hil: no audio device {JACK_DEV!r}: jack analysis skipped")

    def cc(self, ch, n, v):
        self.out.send(self.mido.Message("control_change", channel=ch, control=n, value=v))

    def nrpn(self, scope, idx, v, ch=4):
        self.cc(ch, 99, scope); self.cc(ch, 98, idx); self.cc(ch, 6, v)
        time.sleep(0.04)

    def pc(self, n):
        self.out.send(self.mido.Message("program_change", channel=0, program=n))

    def record(self, sec):
        sd = self.sd
        total = sec + SETTLE
        bufs = {}
        streams = []

        def mk(key, dev, sr, ch):
            bufs[key] = []

            def cb(data, frames, t, status):
                bufs[key].append(data.copy())
            streams.append((key, sd.InputStream(device=dev, samplerate=sr, channels=ch, dtype="float32", callback=cb)))
        if self.usb:
            mk("usb", self.usb, USB_SR, 2)
        if self.jack:
            mk("jack", self.jack, JACK_SR, 6)
        for _, s in streams:
            s.start()
        time.sleep(total)
        for _, s in streams:
            s.stop(); s.close()
        out = {}
        for key, _ in streams:
            x = np.concatenate(bufs[key]).astype(np.float64)
            sr = USB_SR if key == "usb" else JACK_SR
            if key == "jack":
                x = x[:, JACK_CH[0]:JACK_CH[1] + 1]
            out[key] = (x[int(SETTLE * sr):], sr)
        return out


class Source:
    """live: setup(), record, optionally save; wav: read the files; dry: nothing"""

    def __init__(self, live, wavdir, save, dry, rep):
        self.live, self.wavdir, self.save, self.dry, self.rep = live, wavdir, save, dry, rep

    @staticmethod
    def key(*parts):
        return "__".join(re.sub(r"[^A-Za-z0-9&-]+", "_", p).strip("_") for p in parts)

    def capture(self, key, setup, sec):
        """{'usb': (x, sr), 'jack': (x, sr)} (missing keys: not recorded), None when there is nothing"""
        if self.dry:
            self.rep.info(f"plan  {key}  {sec:.0f} s")
            return None
        if self.wavdir:
            out = {}
            for k in ("usb", "jack"):
                p = os.path.join(self.wavdir, f"{key}__{k}.wav")
                if os.path.exists(p):
                    out[k] = read_wav(p)
            if not out:
                self.rep.skip(key, f"no {key}__usb.wav / __jack.wav in {self.wavdir}")
                return None
            return out
        setup()
        out = self.live.record(sec)
        if self.save:
            for k, (x, sr) in out.items():
                write_wav(os.path.join(self.save, f"{key}__{k}.wav"), x, sr)
        return out


# ---------------------------------------------------------------- checks

def reset_globals(live):
    live.nrpn(4, G_OUTPUT, OUT_SPEAKERS)
    live.nrpn(4, G_AMOUNT, AMOUNT_100)
    live.nrpn(4, G_HEAD, HEAD["MEDIUM"])
    for i, v in MS_DEFAULT.items():
        live.nrpn(4, i, v)


def load(live, p, pans=None, settle=2.0):
    live.pc(p["num"])
    time.sleep(settle)
    if pans is not None:
        for ch, pan in enumerate(pans):
            live.cc(ch, 10, pan)
        time.sleep(0.3)


def check_clicks(src, rep, presets, sec, state):
    rep.say("== clicks: jack and USB, SPEAKERS and HEADPHONES (AMOUNT 100, MEDIUM)")
    for p in presets:
        for mode, ov in (("SPEAKERS", OUT_SPEAKERS), ("HEADPHONES", OUT_HEADPHONES)):
            def setup(p=p, ov=ov):
                state.first(p)
                reset_globals(src.live)
                load(src.live, p)
                src.live.nrpn(4, G_OUTPUT, ov)
                time.sleep(0.5)
            tag = f"{preset_tag(p)} {p['name']} {mode}"
            cap = src.capture(Source.key("clicks", preset_tag(p), p["name"], mode), setup, sec)
            if not cap:
                continue
            res = {k: analyse_clicks(x, sr) for k, (x, sr) in cap.items()}
            fails = []
            for k, r in res.items():
                if r["repeats"]:
                    fails.append(f"{k} repeated block at {r['repeats'][0] / cap[k][1]:.2f} s")
                if r["drops"]:
                    fails.append(f"{k} dropout at {r['drops'][0] / cap[k][1]:.2f} s")
            if "jack" in res:
                if "usb" in res:
                    if res["jack"]["clicks"] and not res["usb"]["clicks"]:
                        fails.append(f"jack {len(res['jack']['clicks'])} clicks, USB clean (first at "
                                     f"{res['jack']['clicks'][0] / cap['jack'][1]:.2f} s)")
                    elif res["jack"]["clicks"] and len(res["jack"]["clicks"]) > len(res["usb"]["clicks"]):
                        fails.append(f"jack {len(res['jack']['clicks'])} clicks > USB {len(res['usb']['clicks'])}")
            desc = "  ".join(f"{k}: {len(r['clicks'])} clicks {len(r['repeats'])} repeats {len(r['drops'])} drops "
                             f"{r['level']:.1f} dBFS" + (" tonal" if r["tonal"] else "") for k, r in res.items())
            rep.ok(f"clicks  {tag}", not fails, desc + ("  <- " + "; ".join(fails) if fails else ""))


def lanes_left(live):
    for ch in range(4):
        live.cc(ch, 10, 0)
    time.sleep(0.4)


def check_binaural(src, rep, presets, sec, state):
    rep.say("== binaural: all lanes hard left")
    p = presets[0]
    cases = [("SPEAKERS", OUT_SPEAKERS, "MEDIUM", AMOUNT_100)]
    cases += [(f"HEADPHONES {h}", OUT_HEADPHONES, h, AMOUNT_100) for h in HEAD]
    cases += [("HEADPHONES AMOUNT 0", OUT_HEADPHONES, "MEDIUM", AMOUNT_0)]
    got = {}
    for name, ov, head, amt in cases:
        def setup(ov=ov, head=head, amt=amt, first=(name == cases[0][0])):
            state.first(p)
            if first:
                reset_globals(src.live)
                load(src.live, p)
                lanes_left(src.live)
            src.live.nrpn(4, G_OUTPUT, ov)
            src.live.nrpn(4, G_AMOUNT, amt)
            src.live.nrpn(4, G_HEAD, HEAD[head])
            time.sleep(1.0)
        cap = src.capture(Source.key("binaural", name), setup, sec)
        if not cap or "usb" not in cap:
            if cap:
                rep.skip(f"binaural {name}", "no USB recording")
            continue
        x, sr = cap["usb"]
        s = stereo_stats(x)
        lag = lag_lo(x[:, 0], x[:, 1], sr) * USB_SR / sr
        far = s["r"] - s["l"]
        got[name] = (lag, far, s)
        if name == "SPEAKERS":
            rep.ok("binaural  SPEAKERS", far < FAR_SPEAKERS, f"left {s['l']:.1f} dBFS  far {far:.1f} dB (< {FAR_SPEAKERS:.0f})")
        elif name.startswith("HEADPHONES AMOUNT"):
            sp = got.get("SPEAKERS")
            good = abs(lag) <= 1 and far < FAR_SPEAKERS
            rep.ok("binaural  AMOUNT 0 = SPEAKERS", good,
                   f"lag {lag:.0f} (|lag| <= 1)  far {far:.1f} dB (< {FAR_SPEAKERS:.0f})"
                   + (f"  SPEAKERS far {sp[1]:.1f}" if sp else ""))
        else:
            h = name.split()[1]
            want = HEAD_LAG[h]
            good = abs(lag - want) <= LAG_TOL and FAR_BINAURAL[0] <= far <= FAR_BINAURAL[1]
            rep.ok(f"binaural  {name}", good,
                   f"lag {lag:.0f} (want {want} +-{LAG_TOL})  far {far:.1f} dB (want {FAR_BINAURAL[0]:.0f}..{FAR_BINAURAL[1]:.0f})")


def check_ms(src, rep, presets, sec, state):
    rep.say("== M/S: lanes spread across the field")
    p = presets[0]
    cases = [("default", {}),
             ("MID off", {G_MID: 0}),
             ("SIDE off", {G_SIDE: 0}),
             ("WIDTH 0", {G_WIDTH: 0}),
             ("MONO", {G_MONO: 127})]
    stats = {}
    for name, ov in cases:
        def setup(ov=ov, first=(name == "default")):
            state.first(p)
            if first:
                reset_globals(src.live)
                load(src.live, p, pans=(8, 40, 88, 120))
            for i, v in MS_DEFAULT.items():
                src.live.nrpn(4, i, ov.get(i, v))
            time.sleep(1.0)
        cap = src.capture(Source.key("ms", name), setup, sec)
        if not cap or "usb" not in cap:
            continue
        stats[name] = stereo_stats(cap["usb"][0])
    d = stats.get("default")
    if not d:
        return
    rep.ok("ms  default", d["mid"] >= d["side"], f"MID {d['mid']:.1f} SIDE {d['side']:.1f} dBFS (MID >= SIDE)")
    for name in ("MID off", "SIDE off", "WIDTH 0", "MONO"):
        s = stats.get(name)
        if not s:
            continue
        if name == "MID off":
            good = s["mid"] < d["mid"] + MS_GONE
            txt = f"MID {s['mid']:.1f} (< default {d['mid']:.1f} {MS_GONE:+.0f})"
        else:
            good = s["side"] < d["mid"] + MS_GONE and s["mid"] > d["mid"] - 3
            txt = (f"SIDE {s['side']:.1f} (< default MID {d['mid']:.1f} {MS_GONE:+.0f})  MID {s['mid']:.1f} "
                   f"(kept: > default MID - 3)")
        rep.ok(f"ms  {name}", good, txt)


def check_stereo(src, rep, presets, sec, state):
    rep.say("== stereo after preset loads")
    for p in presets:
        def setup(p=p):
            state.first(p)
            reset_globals(src.live)
            load(src.live, p)
        cap = src.capture(Source.key("stereo", preset_tag(p), p["name"]), setup, sec)
        if not cap or "usb" not in cap:
            continue
        s = stereo_stats(cap["usb"][0])
        lr = abs(s["l"] - s["r"])
        good = s["mid"] >= s["side"] + STEREO_MS and lr <= STEREO_LR and s["corr"] >= STEREO_CORR
        rep.ok(f"stereo  {preset_tag(p)} {p['name']}", good,
               f"MID {s['mid']:.1f} SIDE {s['side']:.1f} (MID >= SIDE + {STEREO_MS:.0f})  |L-R| {lr:.1f} dB "
               f"(<= {STEREO_LR:.0f})  corr {s['corr']:.2f} (>= {STEREO_CORR})")


def check_activity(src, rep, presets, sec, state):
    rep.say("== activity: each lane soloed, three windows")
    win = max(sec / 3.0, 1.0)
    for p in presets:
        for li in range(4):
            lane = p["lanes"][li]
            if not lane or lane["muted"]:
                continue
            want = lane["n"] > 0 and lane["a"] > 0

            def setup(p=p, li=li):
                state.first(p)
                reset_globals(src.live)
                load(src.live, p)
                for j in range(4):
                    if j != li:
                        src.live.cc(j, 7, 0)
                time.sleep(1.0)
            cap = src.capture(Source.key("activity", preset_tag(p), p["name"], f"lane{li + 1}"), setup, 3 * win)
            if not cap or "usb" not in cap:
                continue
            x, sr = cap["usb"]
            w = int(win * sr)
            levels = [rms_db(x[i * w:(i + 1) * w]) for i in range(3) if len(x[i * w:(i + 1) * w])]
            dead = want and any(v < SILENT_DB for v in levels[1:])
            rep.ok(f"activity  {preset_tag(p)} {p['name']} lane {li + 1}", not dead,
                   "windows " + " / ".join(f"{v:.1f}" for v in levels) + f" dBFS"
                   + (f" (n{lane['n']} a{lane['a']}: must stay over {SILENT_DB:.0f})" if want else " (not meant to play)"))
        if not src.dry and not src.wavdir:
            load(src.live, p, settle=1.0)       # levels back


CHECKS = {"clicks": check_clicks, "binaural": check_binaural, "ms": check_ms, "stereo": check_stereo,
          "activity": check_activity}


class State:
    def __init__(self):
        self.preset = None

    def first(self, p):
        if self.preset is None:
            self.preset = p


def restore(live, state, rep):
    """the unit back at its defaults; prints what was restored"""
    p = state.preset or dict(num=0, name="(F001)")
    live.pc(p["num"])
    time.sleep(2.0)
    reset_globals(live)
    for ch in range(4):
        live.cc(ch, 10, 64)
    time.sleep(0.3)
    rep.say(f"restored: OUTPUT SPEAKERS, AMOUNT 100, HEAD SIZE MEDIUM, M/S defaults (MID, SIDE, WIDTH, MONO), "
            f"preset {preset_tag(p) if 'cat' in p else 'F001'} {p['name']} reloaded, lane pans centre")


# ---------------------------------------------------------------- main

def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0], formatter_class=argparse.RawTextHelpFormatter)
    ap.add_argument("--only", help="comma list of: " + ",".join(CHECKS))
    ap.add_argument("--presets", help="comma list: F007, 7 or a name (default: the busiest rain, storm, fire, sand)")
    ap.add_argument("--seconds", type=float, default=20.0, help="recording per case (default 20; binaural and M/S cap at 6, stereo at 8, activity at 15 = 3 windows)")
    ap.add_argument("--save", help="directory for the WAVs and summary.txt")
    ap.add_argument("--wav", help="analyse recordings from this directory, no hardware")
    ap.add_argument("--dry-run", action="store_true", help="print the plan, touch nothing")
    ap.add_argument("--no-restore", action="store_true", help="leave the unit as the last check set it")
    a = ap.parse_args(argv)

    names = [n.strip() for n in a.only.split(",")] if a.only else list(CHECKS)
    bad = [n for n in names if n not in CHECKS]
    if bad:
        ap.error(f"unknown check {bad}")
    allp = parse_presets()
    presets = pick_presets(allp, a.presets)
    if a.save:
        os.makedirs(a.save, exist_ok=True)
    rep = Report(a.save)
    live = None
    if not a.wav and not a.dry_run:
        live = Live()
        rep.say(f"hil: MIDI port {live.port!r}, USB audio {live.usb!r}, jack {live.jack!r} (channels {JACK_CH[0] + 1}/{JACK_CH[1] + 1})")
    src = Source(live, a.wav, a.save, a.dry_run, rep)
    state = State()
    rep.say("presets: " + ", ".join(f"{preset_tag(p)} {p['name']}" for p in presets))
    try:
        for n in names:
            sec = a.seconds
            if n in ("binaural", "ms"):
                sec = min(sec, 6.0)
            elif n == "stereo":
                sec = min(sec, 8.0)
            elif n == "activity":
                sec = min(sec, 15.0)
            ps = presets
            if n == "activity" and not a.presets:
                ps = presets[:3]
            if n in ("binaural", "ms") and not a.presets:
                ps = [p for p in allp if p["name"] == "STEADY RAIN"] or presets
            CHECKS[n](src, rep, ps, sec, state)
    finally:
        if live and not a.no_restore:
            restore(live, state, rep)
    rep.say(f"{'FAIL' if rep.fails else 'ok'}: {rep.fails} failure(s)")
    if a.save:
        with open(os.path.join(a.save, "summary.txt"), "w") as f:
            f.write("\n".join(rep.lines) + "\n")
    return min(rep.fails, 125)


if __name__ == "__main__":
    sys.exit(main())
