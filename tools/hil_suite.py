#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Hardware-in-the-loop suite for Textura's FM-1: run it after every install.

  hil_suite.py [--only midi,clicks,binaural,ms,stereo,activity] [--presets all | F007,STEADY RAIN,...]
               [--midi usb|trs|both] [--seconds 20] [--save DIR] [--wav DIR] [--dry-run] [--no-restore]

Rig (fixed): the FM-1's USB MIDI port "Textura", its USB audio input "Textura FM-1" (2 ch, 44.1 kHz: the device's
rate is set to 44.1 kHz so the recording is the unit's 16-bit samples, bit for bit, never resampled by the computer)
and its headphone jack into the MOTU M6 inputs 5/6 (device "M6", 48 kHz, channel indexes 4 and 5: which input
carries which side, and its polarity, is found from the signal). The M6's MIDI OUT is cabled into the FM-1's TRS
MIDI IN: --midi trs sends everything there, --midi both splits each preset's commands over the two (the program
change over one, the OUTPUT NRPN over the other, alternating) and runs the `midi` check. Debug replies (CC 116, the
CPU) always come back over USB. The M6's gain is never touched. NRPN and program change as in docs/MANUAL.md
(12. MIDI): NRPN on channel 5, program change on channel 1 (F001 .. F100: 0 .. 99, F101 ..: 116 ..).

Checks (each prints ok / FAIL with its numbers; the exit code is the number of failures):
  midi      each MIDI path: the debug reply asked over it, an NRPN read back (OUTPUT), the audio's answer to an NRPN
            (MID off) and to program changes (the sound matches the host's render of the preset loaded)
  clicks    per preset, OUTPUT SPEAKERS then HEADPHONES, jack and USB recorded at once; judged on digital artefacts
            only (crackle is the sound): USB bit-exact repeated or zeroed half / whole blocks, spikes clustered on
            the 256-frame block grid, the jack against the USB (aligned, the analog path fitted: what the fit cannot
            explain is a glitch on one path), the audio interrupt (late blocks must be 0: the debug reply), the
            crackle rate against the host's render of the preset (a note; a FAIL only when far over), the sound
            against the host's render (the preset loaded)
  binaural  all lanes hard left: far ear lag (in the lows, 20 .. 80 Hz: its highs come on time) and level per HEAD
            SIZE; AMOUNT 0 equals SPEAKERS (no lag measured where nothing coherent reaches the far ear)
  ms        lanes spread: each M/S case alternated with the default every 1.5 s in one recording (the sound varies
            more than a case changes it over separate windows): MID off, SIDE off, WIDTH 0, MONO; MID must stay
  stereo    per preset (from the clicks' SPEAKERS recording when there is one): MID against SIDE, L-R against the
            preset's own pans (each lane's place level through the constant-power pan), correlation
  activity  per lane soloed, three windows: a window under -70 dBFS fails only when the host's render of the same
            lane never goes that quiet over as long a window (a sparse lane rests: so does its render)
At the end the unit is restored to its defaults and what was restored is printed.

--wav DIR re-analyses recordings (names `<check>__<case>__usb.wav` / `__jack.wav`, as --save writes them): nothing
touches the hardware. --dry-run prints the plan and touches nothing. The analysis is plain numpy in the functions
below; tests/hil_suite_check.py tests it on synthetic signals (and on the host's render of FULL STORM, when built).
"""
import argparse
import json
import math
import os
import re
import subprocess
import sys
import tempfile
import time
import wave

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

PORT = "Textura"
TRS_PORT = "M6"              # the M6's MIDI OUT, cabled into the FM-1's TRS MIDI IN
USB_DEV, USB_SR = "Textura FM-1", 44100
JACK_DEV, JACK_SR, JACK_CH = "M6", 48000, (4, 5)
SETTLE = 0.5                 # s cut from the start of every recording
BLOCK = 256                  # the firmware's block, frames at 44.1 kHz
BLOCK_US = 5805
HOST = os.path.join(ROOT, "build", "fw", "textura-host")
RIG_DRIFT = -541e-6          # the jack's clock against the unit's, as measured on this rig (-540 .. -543 ppm)
DRIFT_TOL = 30e-6            # a fitted drift further than this from it: a wrong lock (the offset alone is fitted)

# NRPN values (0..127 spread over the range; docs/MANUAL.md)
G_SCNFX = 38                 # EFFECTS ON LOAD: 0 ALWAYS (the host's renders: each preset brings its effects), 127 NEVER
FX_ALWAYS, FX_NEVER = 0, 127
G_OUTPUT, G_AMOUNT, G_HEAD = 45, 46, 47
G_MID, G_SIDE, G_WIDTH, G_MONO = 96, 97, 98, 99
OUT_SPEAKERS, OUT_HEADPHONES = 0, 127
AMOUNT_100, AMOUNT_0 = 127, 0
HEAD = {"SMALL": 0, "MEDIUM": 64, "LARGE": 127}
HEAD_LAG = {"SMALL": 25, "MEDIUM": 29, "LARGE": 33}      # far ear lag at 44.1 kHz
MS_DEFAULT = {G_MID: 115, G_SIDE: 115, G_WIDTH: 64, G_MONO: 0}

# thresholds
CLICK_K, CLICK_FLOOR = 8.0, 0.003     # (crackle counter) second difference over k x its local noise and an absolute floor
REPEAT_R, REPEAT_TONAL = 0.90, 0.05    # (find_repeats, an analog signal) adjacent-block correlation; tonal fraction
DROP_WIN, DROP_PEAK, DROP_REF = 96, 0.01, 10 ** (-50 / 20)
EXACT_RUN = 64                         # USB: samples in a row equal to those a half / whole block before (16-bit, both
                                       # sides, not constant): a repeated block; natural audio never does it
ZERO_LENS = ((120, 136), (248, 264))   # USB: an exact-zero gap of a half or a whole block inside sound
ZERO_SIDE = 10 ** (-65 / 20)           # the sound on both sides of such a gap (rms of 512 samples)
GRID_P = 1e-4                          # spikes on one phase of the 256-frame grid: chance under this (of 256 phases)
GLITCH_DB = -8.0                       # jack vs USB: what the fitted analog path leaves in 32 samples, over the signal
                                       # around (dB); the rig's own at worst: -12 (STEADY RAIN, loud), -15 (BONFIRE),
                                       # -26 (FULL STORM); a zeroed / repeated half block, a dropped block, a 0.02
                                       # click on one path: -7 .. +22 (real recordings, injected)
GLITCH_LOC_DB = 10.0                   # ... and over the residual's median around it (+-0.1 s) by this
JACK_FLOOR_DB = -70.0                  # jack vs USB: the band's level (jack dBFS, ~ -57 at the USB) under which nothing
                                       # counts (the jack's own hum, noise and small pops come and go at -75 .. -100)
RES_FLOOR_K = 2.0                      # jack vs USB: the residual's floor, this many times its 10th percentile (a
                                       # loose fit is left to GLITCH_LOC_DB)
STEP_SEARCH, STRONG_PK = 400, 0.15     # jack vs USB: each chunk's lag searched +-400 around the line; a chunk with a
                                       # whitened peak this high is sure of its lag (noise alone: ~0.03)
ALIGN_FIT = 0.5                        # jack vs USB: at least this share of the chunks on the fitted lag line, else
                                       # nothing judged (a sparse scene, drips in silence: nothing to align on)
MODEL_MIN_DB = 12.0                    # the fit's band SNR under this: the two recordings disagree (a sample slip)
CRACKLE_FAIL = (3.0, 100.0)            # crackle over the host's: FAIL beyond 3x its busiest window and +100 / 10 s
ID_OK = 3.0                            # a load heard: its sound's shape this close to a window of its host render
                                       # (octave bands, rms dB), or closer to it than to the previous preset's
LAG_TOL = 2                            # samples
LAG_COH = 0.5                          # coherence in the lag's band under this: no lag to measure
FAR_BINAURAL = (-20.0, -10.0)          # dB re the near ear
FAR_SPEAKERS = -25.0                   # dB re the near ear: hard left in SPEAKERS leaks nothing to the right
AMT0_FAR_TOL = 3.0                     # AMOUNT 0's far ear within this of SPEAKERS'
MS_GONE = -25.0                        # dB re the default level
MS_SEG, MS_PAIRS = 1.5, 8              # the M/S alternation: segment s, default / case pairs
MS_KEEP = 1.5                          # MID's paired change allowed (dB; widened to 2.5 standard errors, at most 4)
STEADY_PLACES = {"F": 0.0, "M": -6.0, "B": -12.0}
STEREO_MS, STEREO_LR, STEREO_CORR = 3.0, 3.0, 0.3       # STEREO_LR: |(L-R) - the preset's expected L-R|
SILENT_DB = -70.0
HOST_LR_SEC, LR_MARGIN = 60.0, 1.5     # stereo: the host's render this long; the device's L-R within its span +- this
HOST_ACT_SEC = 60.0                    # the host's render of a soloed lane for the activity check


# ---------------------------------------------------------------- analysis (pure numpy)

def db(v):
    return 20 * np.log10(v + 1e-12)


def rms_db(x):
    x = np.asarray(x, dtype=np.float64)
    return float(db(np.sqrt(np.mean(x * x)))) if x.size else -240.0


def _cols(x):
    x = np.asarray(x, dtype=np.float64)
    return [x] if x.ndim == 1 else [x[:, i] for i in range(x.shape[1])]


def _two(x):
    x = np.asarray(x, dtype=np.float64)
    return np.stack([x, x], axis=1) if x.ndim == 1 else x


def _merge(idx, gap):
    out = []
    for i in idx:
        if not out or i - out[-1] > gap:
            out.append(int(i))
    return out


def _runs(mask):
    """[(start, length)] of the True runs of a boolean array"""
    m = np.r_[False, np.asarray(mask, bool), False].astype(np.int8)
    d = np.diff(m)
    s, e = np.nonzero(d == 1)[0], np.nonzero(d == -1)[0]
    return list(zip(s.tolist(), (e - s).tolist()))


def find_clicks(x, k=CLICK_K, floor=CLICK_FLOOR, blk=512):
    """sample indexes of spikes of the second difference over k x the local noise (block median of |d2| as a sigma, the loudest
    of the block and its two neighbours) and over an absolute floor; per channel, merged within 16 samples. Rain, fire
    and thunder crackle are such spikes: this counts crackle (compared with the host's render), it judges nothing alone"""
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
    """(an analog recording) sample indexes where a window of `block` samples repeats the one before (normalised
    correlation at a lag of block-1 .. block+1 over thr, window energy over -60 dB). Returns (list, windows_checked,
    tonal): tonal when so many windows correlate (a held tone) that repeats cannot be told from the signal."""
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


def _poisson_sf(k, lam):
    """P(X >= k), X ~ Poisson(lam)"""
    if k <= 0:
        return 1.0
    term, cdf = math.exp(-lam), 0.0
    for i in range(int(k)):
        cdf += term
        term *= lam / (i + 1)
    return max(0.0, 1.0 - cdf)


def usb_digital(x, block=BLOCK):
    """digital artefacts in the USB recording (the unit's own samples, 44.1 kHz, never resampled):
      repeats  [(index, lag)]: EXACT_RUN+ samples in a row equal (both sides) to those a half or a whole block before,
               the run not constant (a block sent twice)
      zeros    [(index, length)]: an exact-zero gap (both sides) of a half or a whole block with sound on both sides
               (a block lost; a sound that stops and starts is no such gap)
      grid     (count, expected, phase, p): the loudest 0.1 % of second-difference spikes counted per phase of the
               256-frame grid; p the chance that the busiest phase holds so many (a step at a block edge: low p)"""
    x = _two(x)
    n = len(x)
    reps = []
    for lag in (block // 2, block):
        if n <= lag + EXACT_RUN:
            continue
        a, b = x[lag:], x[:-lag]
        eq = np.all(a == b, axis=1) & np.any(a != 0, axis=1)
        for s, ln in _runs(eq):
            if ln >= EXACT_RUN and np.ptp(a[s:s + ln, 0]) + np.ptp(a[s:s + ln, 1]) > 4.0 / 32768:
                reps.append((s + lag, lag))
    reps.sort()
    zeros = []
    z = np.all(x == 0, axis=1)
    cs = np.r_[0.0, np.cumsum(np.sum(x * x, axis=1) / 2)]
    for s, ln in _runs(z):
        if not any(lo <= ln <= hi for lo, hi in ZERO_LENS):
            continue
        if s < 512 or s + ln + 512 > n:
            continue
        left = math.sqrt((cs[s] - cs[s - 512]) / 512)
        right = math.sqrt((cs[s + ln + 512] - cs[s + ln]) / 512)
        if min(left, right) > ZERO_SIDE:
            zeros.append((s, ln))
    grid = (0, 0.0, 0, 1.0)
    if n > 16 * block:
        hits = []
        for c in range(x.shape[1]):
            d2 = np.abs(np.diff(x[:, c], 2))
            thr = np.percentile(d2, 99.9)
            if thr <= 0:
                continue
            hits += _merge(np.nonzero(d2 > thr)[0] + 1, 16)
        hits = sorted(set(hits))
        if hits:
            ph = np.bincount(np.asarray(hits) % block, minlength=block)
            lam = len(hits) / block
            k = int(ph.max())
            p = min(1.0, block * _poisson_sf(k, lam))
            grid = (k, lam, int(ph.argmax()), p)
    return dict(repeats=reps, zeros=zeros, grid=grid)


def _resample(x, n):
    """x (1-D) to n samples, band-limited (FFT)"""
    X = np.fft.rfft(x)
    m = n // 2 + 1
    Y = np.zeros(m, complex)
    k = min(m, len(X))
    Y[:k] = X[:k]
    return np.fft.irfft(Y, n) * (n / len(x))


def gcc_lag(a, b, maxlag):
    """samples by which b lags a: the phase transform's peak (whitened: periodic content cannot pick a wrong period),
    sign-blind (an inverting analog path), with a parabolic fraction; returns (lag, peak height)"""
    n = len(a)
    X = np.fft.rfft(b, 2 * n) * np.conj(np.fft.rfft(a, 2 * n))
    X /= np.abs(X) + 1e-20
    c = np.fft.irfft(X)
    c = np.concatenate([c[-maxlag:], c[:maxlag + 1]])
    i = int(np.argmax(np.abs(c)))
    if c[i] < 0:
        c = -c
    d = 0.0
    if 0 < i < len(c) - 1:
        den = c[i - 1] - 2 * c[i] + c[i + 1]
        d = 0.5 * (c[i - 1] - c[i + 1]) / den if den else 0.0
    return i - maxlag + d, float(c[i])


def _interp(x, q, taps=16):
    """x at the fractional positions q (Hann-windowed sinc, 2 x taps points)"""
    i0 = np.floor(q).astype(np.int64)
    f = q - i0
    out = np.zeros(len(q))
    for t in range(-taps + 1, taps + 1):
        d = t - f
        out += np.sinc(d) * (0.5 + 0.5 * np.cos(np.pi * d / taps)) * x[np.clip(i0 + t, 0, len(x) - 1)]
    return out


def align_jack(u, su, j, sj, W=16384, iters=3, drift0=RIG_DRIFT, start=None):
    """the jack's recording on the USB's samples: resampled, its clock drift (the unit's against the M6's; this rig's
    as the start) and offset fitted from whitened chunk lags, each chunk searched around the one before (the drift
    walks the lag hundreds of samples over a recording). Returns (usb part, jack part aligned, first USB index, info:
    drift_ppm, offset, jitter, fit = the chunks within a sample of the line), None when too short"""
    u, j = _two(u), _two(j)
    m, jm = u.sum(1), j.sum(1)
    drift, a, o, k = drift0, None, None, None
    for _ in range(iters):
        n = int(round(len(jm) * su / sj * (1 - drift)))
        jr = _resample(jm, n)
        head = min(len(m), len(jr), 4 * su)
        if head < 2 * W:
            return None
        L = start if start is not None else gcc_lag(m[:head], jr[:head], min(6000, head // 3))[0]
        o = []
        for s in range(64, len(m) - W - 64, W // 2):
            c = int(round(L))
            if s + c - 64 < 0 or s + c + W + 64 > len(jr):
                continue
            l, pk = gcc_lag(m[s:s + W], jr[s + c:s + c + W], 64)
            L = c + l
            o.append((s, L, pk))
        if len(o) < 4:
            return None
        o = np.array(o)
        k = o[:, 2] >= np.median(o[:, 2])
        a = np.polyfit(o[k, 0], o[k, 1], 1)
        for _ in range(3):
            res = o[:, 1] - np.polyval(a, o[:, 0])
            k = np.abs(res) < 1.0
            if k.sum() < 3:
                k = np.abs(res) <= np.median(np.abs(res))
            a = np.polyfit(o[k, 0], o[k, 1], 1)
        drift += a[0] * (1 - drift)
        if abs(drift - drift0) > DRIFT_TOL:  # (tonal or sparse scenes can lock onto a wrong period: the clocks are
            drift, fixed = drift0, True      # known to a few ppm, so keep the rig's drift and fit the offset alone)
            n = int(round(len(jm) * su / sj * (1 - drift)))
            jr = _resample(jm, n)
            o = o[:0]
            L = start if start is not None else gcc_lag(m[:head], jr[:head], min(6000, head // 3))[0]
            rows = []
            for s in range(64, len(m) - W - 64, W // 2):
                c = int(round(L))
                if s + c - 64 < 0 or s + c + W + 64 > len(jr):
                    continue
                l, pk = gcc_lag(m[s:s + W], jr[s + c:s + c + W], 64)
                rows.append((s, c + l, pk))
            if len(rows) < 4:
                return None
            o = np.array(rows)
            a = np.array([0.0, float(np.median(o[o[:, 2] >= np.median(o[:, 2]), 1]))])
            k = np.abs(o[:, 1] - a[1]) < 1.0
            break
        if abs(a[0]) < 2e-7:
            break
    jr = np.stack([_resample(j[:, c], n) for c in range(j.shape[1])], 1)   # (the last pass's n: a and o are its)
    pos = np.arange(len(u)) + np.polyval(a, np.arange(len(u)))           # jr's position of each USB sample
    ok = np.nonzero((pos >= 16) & (pos < len(jr) - 17))[0]
    if len(ok) < 4 * W:
        return None
    lo, hi = int(ok[0]), int(ok[-1]) + 1
    ja = np.stack([_interp(jr[:, c], pos[lo:hi]) for c in range(jr.shape[1])], 1)
    # every chunk again, searched wide around the line: the strong ones (a clear peak) must sit on it; a run of
    # strong chunks off it by the same amount is a step (samples lost or added on one path)
    jm2 = _resample(jm, n)
    w2, V = [], W // 2
    for s in range(64, len(m) - V - 64, V // 2):
        c = int(round(np.polyval(a, s)))
        if s + c - STEP_SEARCH < 0 or s + c + V + STEP_SEARCH > len(jm2):
            continue
        l, pk = gcc_lag(m[s:s + V], jm2[s + c:s + c + V], STEP_SEARCH)
        w2.append((s, c + l - np.polyval(a, s), pk))
    w2 = np.array(w2) if w2 else np.zeros((0, 3))
    strong = w2[w2[:, 2] >= STRONG_PK] if len(w2) else w2
    fit = float(np.mean(np.abs(strong[:, 1]) < 1.0)) if len(strong) >= 4 else 0.0
    steps = []
    for i in range(len(strong) - 2):
        r0, r1, r2 = strong[i:i + 3, 1]
        if abs(r0) > 2 and abs(r1 - r0) < 1 and abs(r2 - r0) < 1 and not any(abs(t - strong[i, 0]) < 3 * V for t, _ in steps):
            steps.append((float(strong[i, 0]), float(r0)))
    res = o[:, 1] - np.polyval(a, o[:, 0])
    return u[lo:hi], ja, lo, dict(drift_ppm=drift * 1e6, offset=float(a[1]), jitter=float(np.std(res[np.abs(res) < 1.0]))
                                  if np.any(np.abs(res) < 1.0) else 99.0, fit=fit, strong=len(strong),
                                  steps=[(t / su, d) for t, d in steps])


def _band(x, sr, lo=200.0, hi=16000.0):
    X = np.fft.rfft(x, axis=0)
    f = np.fft.rfftfreq(len(x), 1.0 / sr)
    X[(f < lo) | (f > hi)] = 0
    return np.fft.irfft(X, len(x), axis=0)


def gcc_peaks(a, b, maxlag, k=3, sep=32):
    """the k highest whitened cross-correlation peaks (lags), at least sep apart"""
    n = len(a)
    X = np.fft.rfft(b, 2 * n) * np.conj(np.fft.rfft(a, 2 * n))
    X /= np.abs(X) + 1e-20
    c = np.abs(np.fft.irfft(X))
    c = np.concatenate([c[-maxlag:], c[:maxlag + 1]])
    out = []
    for i in np.argsort(c)[::-1]:
        if all(abs(i - q) >= sep for q in out):
            out.append(int(i))
        if len(out) == k:
            break
    return [q - maxlag for q in out]


def jack_vs_usb(u, su, j, sj, nfft=4096, w=32, edge=4410):
    """jack_one at the whitened correlation's best offset; when its fit fails, at the next two (echoes and tones
    give a delay several peaks): the best fit wins"""
    r = jack_one(u, su, j, sj, nfft, w, edge)
    if r is None or r["ok"] is not False or r["steps"]:
        return r
    uu, jj = _two(u), _two(j)
    m, jm = uu.sum(1), jj.sum(1)
    n = int(round(len(jm) * su / sj * (1 - RIG_DRIFT)))
    jr = _resample(jm, n)
    head = min(len(m), len(jr), 4 * su)
    for L in gcc_peaks(m[:head], jr[:head], min(6000, head // 3))[1:]:
        r2 = jack_one(u, su, j, sj, nfft, w, edge, start=L)
        if r2 is not None and r2["model_db"] == r2["model_db"] and (r["model_db"] != r["model_db"] or r2["model_db"] > r["model_db"]):
            r = r2
    return r


def jack_one(u, su, j, sj, nfft=4096, w=32, edge=4410, start=None):
    """the jack against the USB: aligned (align_jack), the sides matched (the rig may swap them), the analog path
    fitted per side (a Welch transfer function: the DAC, the amp, the M6, the polarity), and what the fit leaves
    scored in windows of w samples in 200 .. 16000 Hz against the fitted signal around them (and the residual's
    floor). Returns dict(model_db, swap, drift_ppm, glitches=[(time s, score dB)], worst, ok) or None (too short /
    nothing to align)."""
    al = align_jack(u, su, j, sj, start=start)
    if al is None:
        return None
    uu, ja, lo, info = al
    if info["steps"]:                        # (segments sure of their lags, apart: one path lost or added samples)
        return dict(model_db=float("nan"), swap=False, drift_ppm=info["drift_ppm"], glitches=[], worst=float("nan"),
                    ok=False, fit=info["fit"], steps=info["steps"])
    if info["fit"] < ALIGN_FIT and info["strong"] >= 8:   # (lags sure of themselves, off the line: no one offset)
        return dict(model_db=float("nan"), swap=False, drift_ppm=info["drift_ppm"], glitches=[], worst=float("nan"),
                    ok=False, fit=info["fit"], steps=[(float("nan"), float("nan"))])
    if info["fit"] < ALIGN_FIT:              # (a sparse or quiet scene: too little in common to align on)
        return dict(model_db=float("nan"), swap=False, drift_ppm=info["drift_ppm"], glitches=[], worst=float("nan"),
                    ok=None, fit=info["fit"], steps=[])
    cm = [[abs(np.sum(uu[:, a] * ja[:, b])) / math.sqrt(np.sum(uu[:, a] ** 2) * np.sum(ja[:, b] ** 2) + 1e-30)
           for b in range(2)] for a in range(2)]
    swap = cm[0][1] + cm[1][0] > cm[0][0] + cm[1][1]
    if swap:
        ja = ja[:, ::-1]
    e, y = np.zeros_like(ja), np.zeros_like(ja)
    win = np.hanning(nfft)
    for c in range(2):
        Suu, Suj = np.zeros(nfft // 2 + 1), np.zeros(nfft // 2 + 1, complex)
        for s in range(0, len(uu) - nfft, nfft // 2):
            U, J = np.fft.rfft(uu[s:s + nfft, c] * win), np.fft.rfft(ja[s:s + nfft, c] * win)
            Suu += np.abs(U) ** 2
            Suj += J * np.conj(U)
        h = np.roll(np.fft.irfft(Suj / (Suu + 1e-12 * Suu.max() + 1e-30), nfft), nfft // 2) * np.hanning(nfft)
        N = len(uu) + nfft
        y[:, c] = np.fft.irfft(np.fft.rfft(uu[:, c], N) * np.fft.rfft(h, N), N)[nfft // 2:nfft // 2 + len(uu)]
        e[:, c] = ja[:, c] - y[:, c]
    eb, yb = _band(e, su)[edge:-edge], _band(y, su)[edge:-edge]
    model = rms_db(yb) - rms_db(eb)
    nw = len(eb) // w
    er = (eb[:nw * w] ** 2).reshape(nw, w, -1).mean(1)
    yr = (yb[:nw * w] ** 2).reshape(nw, w, -1).mean(1)
    nf = np.percentile(er, 10, axis=0)
    jb = _band(ja, su)[edge:-edge]
    jq = np.percentile((jb[:nw * w] ** 2).reshape(nw, w, -1).mean(1), 1, axis=0)   # the jack's quietest: its own
    nf = np.maximum(np.maximum(RES_FLOOR_K * nf, 2 * jq), 10 ** (JACK_FLOOR_DB / 10))  # noise floor and the 16-bit steps;
                                                                                    # nothing under JACK_FLOOR_DB counts
    yn = yr.copy()                           # the signal around: the loudest of the window and 3 each side (an
    for d in (1, 2, 3):                      # onset's leading edge leaves a little the fit cannot place)
        yn = np.maximum(yn, np.maximum(np.r_[yr[d:], np.repeat(yr[-1:], d, 0)], np.r_[np.repeat(yr[:1], d, 0), yr[:-d]]))
    sc = (10 * np.log10(er / (yn + nf + 1e-30) + 1e-30)).max(1)
    # and over the fit's own residual around (0.1 s each side): where the fit is loose (quiet highs, tones) the
    # residual is high all along; a glitch stands out of it
    B = max(int(0.1 * su / w), 1)
    nb = (nw + B - 1) // B
    erm = er.max(1)
    med = np.array([np.median(erm[i * B:(i + 1) * B]) for i in range(nb)])
    med = np.maximum(med, np.maximum(np.r_[med[1:], med[-1:]], np.r_[med[:1], med[:-1]]))
    loc = 10 * np.log10(erm / (np.repeat(med, B)[:nw] + 1e-30) + 1e-30)
    hit = np.nonzero((sc > GLITCH_DB) & (loc > GLITCH_LOC_DB))[0]
    gl = [(round((i * w + edge + lo) / su, 3), round(float(sc[i]), 1)) for i in _merge(hit, 8)]
    return dict(model_db=float(model), swap=bool(swap), drift_ppm=info["drift_ppm"], glitches=gl,
                worst=float(sc.max()) if len(sc) else -99.0, ok=model >= MODEL_MIN_DB and not gl, fit=info["fit"], steps=[])


def crackle_rate(x, sr):
    """crackle spikes (find_clicks) per 10 s"""
    return len(find_clicks(x)) * 10.0 * sr / max(len(x), 1)


def window_rates(x, sr, sec):
    """crackle per 10 s in each window of sec seconds"""
    w = int(sec * sr)
    return [crackle_rate(x[i:i + w], sr) for i in range(0, len(x) - w + 1, w)] if w > 0 else []


BANDS = [(31.5 * 2 ** i / math.sqrt(2), 31.5 * 2 ** i * math.sqrt(2)) for i in range(10)]


def band_levels(x, sr):
    """dB per octave band (31.5 Hz .. 16 kHz) of the pair's mid"""
    m = _two(x).mean(1)
    P = np.abs(np.fft.rfft(m)) ** 2
    f = np.fft.rfftfreq(len(m), 1.0 / sr)
    return np.array([10 * np.log10(P[(f >= a) & (f < b)].sum() / max(len(m), 1) ** 2 + 1e-24) for a, b in BANDS])


def band_dist(a, b):
    return float(np.sqrt(np.mean((np.asarray(a) - np.asarray(b)) ** 2)))


def best_shape(x, sr, ref):
    """the spectrum's shape distance (shape_dist) of x to the closest window of as long in the host render ref"""
    hx, hsr = ref
    w = int(len(x) * hsr / sr)
    bl = band_levels(x, sr)
    d = [shape_dist(bl, band_levels(hx[i:i + w], hsr)) for i in range(0, max(len(hx) - w, 0) + 1, max(w // 2, 1))]
    return min(d) if d else float("nan")


def shape_dist(a, b):
    """band_dist of the spectra's shapes (each less its mean: the level apart)"""
    a, b = np.asarray(a), np.asarray(b)
    return band_dist(a - a.mean(), b - b.mean())


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


def lag_lo(l, r, sr, band=(20.0, 80.0), n=1 << 16, maxlag=64):
    """samples by which r lags l in the lows: the slope of the cross-spectrum's phase over band (Hz), weighted by its
    magnitude. HEADPHONES delays the far ear only below its crossover (engines/tx_core_mix.c TX_XO: 380 Hz at a hard
    pan), its highs come on time, so a broadband cross-correlation peaks at 0. NaN when the slope is beyond +-maxlag
    (nothing coherent: a far ear of reverb and noise only; see coherence_lo)"""
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
    v = float(-np.sum(w * ph * om) / max(np.sum(w * om * om), 1e-30))
    return v if abs(v) <= maxlag else float("nan")


def coherence_lo(l, r, sr, band=(20.0, 80.0), nfft=8192):
    """mean magnitude-squared coherence of l and r over band (Welch, Hann, half overlap): 1 the far ear is the near
    ear filtered and delayed, ~0 nothing in common"""
    l, r = np.asarray(l, dtype=np.float64), np.asarray(r, dtype=np.float64)
    m = min(len(l), len(r))
    if m < 2 * nfft:
        nfft = max(256, 1 << int(math.log2(max(m // 2, 256))))
    win = np.hanning(nfft)
    Sll = Srr = 0.0
    Slr = 0.0
    for s in range(0, m - nfft + 1, nfft // 2):
        L, R = np.fft.rfft(l[s:s + nfft] * win), np.fft.rfft(r[s:s + nfft] * win)
        Sll = Sll + np.abs(L) ** 2
        Srr = Srr + np.abs(R) ** 2
        Slr = Slr + R * np.conj(L)
    if np.isscalar(Sll):
        return 0.0
    f = np.fft.rfftfreq(nfft, 1.0 / sr)
    k = (f >= band[0]) & (f <= band[1])
    c = np.abs(Slr[k]) ** 2 / (Sll[k] * Srr[k] + 1e-30)
    return float(np.mean(c)) if c.size else 0.0


def stereo_stats(x):
    """left, right, mid, side levels (dBFS) and the correlation of an (n, 2) block"""
    l, r = np.asarray(x[:, 0], dtype=np.float64), np.asarray(x[:, 1], dtype=np.float64)
    m, s = (l + r) / 2, (l - r) / 2
    den = np.sqrt(np.sum(l * l) * np.sum(r * r)) + 1e-18
    return dict(l=rms_db(l), r=rms_db(r), mid=rms_db(m), side=rms_db(s), corr=float(np.sum(l * r) / den))


def lane_place_db(lane):
    """a preset lane's place level, dB (F 0, M -6, B -12, r-N -N)"""
    r = lane.get("r", "F")
    if r in STEADY_PLACES:
        return STEADY_PLACES[r]
    try:
        return float(r)
    except ValueError:
        return 0.0


def expected_lr(p):
    """the preset's L-R (dB) from its playing lanes: each lane's place level through the constant-power pan
    (engines/tx_core_mix.c tx_mix_pan: sqrt 2 cos / sin of pan / 127 x 90 degrees). The reverb's even tail pulls a
    measurement towards 0. None when nothing plays"""
    pl = pr = 0.0
    for lane in p["lanes"]:
        if not lane or lane["muted"] or lane["n"] <= 0:
            continue
        P = 10 ** (lane_place_db(lane) / 10)
        th = min(max(lane.get("p", 64), 0), 127) / 127 * math.pi / 2
        pl += P * math.cos(th) ** 2
        pr += P * math.sin(th) ** 2
    if pl <= 0 or pr <= 0:
        return None
    return 10 * math.log10(pl / pr)


def host_lr_span(h, sec):
    """(lowest, highest) L-R (dB) over the host render's windows of sec s (half overlap): what the preset's own
    sounds give over as long a window (a sparse loud lane, thunder on one side, moves it)"""
    hx, hsr = h
    w = int(sec * hsr)
    v = []
    for i in range(0, len(hx) - w + 1, max(w // 2, 1)):
        st = stereo_stats(hx[i:i + w])
        if st["mid"] > -90:
            v.append(st["l"] - st["r"])
    return (min(v), max(v)) if v else None


def lr_verdict(lr, exp, span):
    """(good, text): L-R within the host's span (+-LR_MARGIN), or without a host within STEREO_LR of the pans'"""
    if span is not None:
        good = span[0] - LR_MARGIN <= lr <= span[1] + LR_MARGIN
        return good, f"L-R {lr:+.1f} dB (the host's {span[0]:+.1f} .. {span[1]:+.1f} +-{LR_MARGIN:.1f}; pans {exp:+.1f})" if exp is not None \
            else f"L-R {lr:+.1f} dB (the host's {span[0]:+.1f} .. {span[1]:+.1f} +-{LR_MARGIN:.1f})"
    if exp is None:
        return abs(lr) <= STEREO_LR, f"L-R {lr:+.1f} dB (no lane plays by the file: |L-R| <= {STEREO_LR:.0f})"
    return abs(lr - exp) <= STEREO_LR, f"L-R {lr:+.1f} dB (its pans {exp:+.1f}, within {STEREO_LR:.0f}; no host render)"


def ms_segments(x, sr, seg=MS_SEG, pairs=MS_PAIRS, skip=0.3, tail=0.05):
    """the M/S alternation: (default segments' (mid, side) dB, case segments' (mid, side) dB); segment i (from 0)
    starts at i x seg, odd ones the case; each measured past its first `skip` s (the NRPN and the glide)"""
    x = _two(x)
    m, s = (x[:, 0] + x[:, 1]) / 2, (x[:, 0] - x[:, 1]) / 2
    off, on = [], []
    for i in range(2 * pairs):
        a, b = int((i * seg + skip) * sr), int(((i + 1) * seg - tail) * sr)
        if b > len(x) or b <= a:
            break
        (on if i % 2 else off).append((rms_db(m[a:b]), rms_db(s[a:b])))
    k = min(len(on), len(off))
    return off[:k], on[:k]


def ms_verdict(name, off, on):
    """(good, text) for one M/S case from its paired segments"""
    if not off:
        return False, "no segments"
    dm = np.array([b[0] - a[0] for a, b in zip(off, on)])
    mid_d = float(np.median([a[0] for a in off]))
    mean, se = float(dm.mean()), float(dm.std(ddof=1) / math.sqrt(len(dm))) if len(dm) > 1 else 0.0
    side_c = float(np.max([b[1] for b in on]))
    if name == "MID off":
        good = float(np.max([b[0] for b in on])) < mid_d + MS_GONE
        return good, (f"MID {mean:+.1f} dB (each case segment < default MID {mid_d:.1f} {MS_GONE:+.0f}), "
                      f"{len(dm)} pairs")
    lim = min(max(MS_KEEP, 2.5 * se), 4.0)
    good = side_c < mid_d + MS_GONE and abs(mean) <= lim
    return good, (f"MID {mean:+.2f} dB +-{se:.2f} (kept: |change| <= {lim:.1f})  SIDE <= {side_c:.1f} "
                  f"(< default MID {mid_d:.1f} {MS_GONE:+.0f})  {len(dm)} pairs")


def activity_verdict(levels, host_quiet, want):
    """(dead, text): a device window under SILENT_DB is a failure only when the host's render of the lane has no
    window as quiet (host_quiet: that fraction of its windows, None: no render)"""
    quiet = [v < SILENT_DB for v in levels[1:]]
    if not want or not any(quiet):
        return False, ""
    if host_quiet is None:
        return True, "silent window, no host render to compare"
    if host_quiet > 0:
        return False, f"silent window; the host's render rests too ({host_quiet * 100:.0f} % of its windows)"
    return True, "silent window; the host's render never rests so long"


def block_for(sr):
    return int(round(BLOCK * sr / USB_SR))


def analyse_clicks(x, sr):
    """the analog-style analysis of one recording (crackle count, correlated repeats, dropouts); kept for --wav of
    old recordings and the tests; the clicks check judges with usb_digital and jack_vs_usb"""
    rep, nwin, tonal = find_repeats(x, block_for(sr), REPEAT_R)
    return dict(clicks=find_clicks(x), repeats=rep, tonal=tonal, drops=find_dropouts(x), level=rms_db(x))


def judge_clicks(cap, host=None, ref=None):
    """the verdict on one capture {'usb': (x, sr), 'jack': (x, sr)}, the host's render (x, sr) of the same preset as
    long (or None: no crackle or sound comparison) and a long one (ref, or None: host) for the block grid (an engine
    that starts its events on a block's first sample shows there over a minute): (fails [str], notes [str], numbers)"""
    fails, notes, num = [], [], {}
    if "usb" in cap:
        x, sr = cap["usb"]
        d = usb_digital(x)
        num["usb"] = d
        if d["repeats"]:
            fails.append(f"USB block repeated ({len(d['repeats'])}, first at {d['repeats'][0][0] / sr:.3f} s, "
                         f"lag {d['repeats'][0][1]})")
        if d["zeros"]:
            fails.append(f"USB block zeroed ({len(d['zeros'])}, first at {d['zeros'][0][0] / sr:.3f} s, "
                         f"{d['zeros'][0][1]} samples)")
        k, lam, ph, p = d["grid"]
        r_ = ref if ref is not None else host
        hgrid = usb_digital(r_[0])["grid"] if r_ is not None else None
        if p < GRID_P:
            txt = f"spikes on the block grid: {k} at phase {ph} (expected {lam:.1f}, p {p:.1e})"
            if hgrid and hgrid[3] < GRID_P:      # (an engine starting its events at a block's first sample: the
                notes.append(txt + ", the host's render too (an engine's onsets, not the unit's)")   # sound's, host too)
            else:
                fails.append(txt)
        num["host_grid"] = hgrid
        dr = find_dropouts(x)
        if dr:                                   # (a sudden fall of the level: the sound's own gaps do it too, a
            notes.append(f"USB level falls away at {dr[0] / sr:.2f} s")   # bird's song; lost blocks are the zeros above,
                                                                          # a late block the CPU check)
        num["level"] = rms_db(x)
        num["crackle"] = crackle_rate(x, sr)
        if host is not None:
            hx, hsr = host
            num["host_level"] = rms_db(hx)
            g = 10 ** ((num["host_level"] - num["level"]) / 20)      # (crackle counted at the host's level)
            num["crackle_n"] = crackle_rate(x * g, sr)
            sec = len(x) / sr
            hr = window_rates(hx, hsr, sec) or [crackle_rate(hx, hsr)]
            num["host_crackle"] = (float(np.median(hr)), float(np.max(hr)))
            bl, hb = band_levels(x, sr), band_levels(hx, hsr)
            num["id_db"] = band_dist(bl - bl.mean(), hb - hb.mean())     # the spectrum's shape
            if num["crackle_n"] > CRACKLE_FAIL[0] * max(hr) and num["crackle_n"] > max(hr) + CRACKLE_FAIL[1]:
                fails.append(f"crackle {num['crackle_n']:.0f} / 10 s at the host's level, host {max(hr):.0f} at most")
            elif num["crackle_n"] > 1.5 * max(hr) + 20:
                notes.append(f"crackle {num['crackle_n']:.0f} / 10 s at the host's level, over its {max(hr):.0f}")
            if abs(num["level"] - num["host_level"]) > 3:
                notes.append(f"level {num['level'] - num['host_level']:+.1f} dB against the host's render")
    if "usb" in cap and "jack" in cap:
        (u, su), (j, sj) = cap["usb"], cap["jack"]
        jv = jack_vs_usb(u, su, j, sj)
        num["jack"] = jv
        if jv is None or jv["ok"] is None:
            notes.append("jack: too sparse or short to align on the USB" + (f" ({jv['fit'] * 100:.0f} % of the chunks)" if jv else ""))
        elif jv["steps"]:
            t, d = jv["steps"][0]
            fails.append(f"jack and USB slip apart by {d:+.0f} samples near {t:.2f} s (samples lost or added on one path)"
                         if t == t else f"jack and USB keep no one offset ({jv['fit'] * 100:.0f} % of the sure chunks on it)")
        else:
            if jv["model_db"] < MODEL_MIN_DB:
                fails.append(f"jack and USB disagree (fit {jv['model_db']:.1f} dB < {MODEL_MIN_DB:.0f}: a sample slip "
                             f"on one path?)")
            for t, s in jv["glitches"][:3]:
                fails.append(f"jack vs USB glitch at {t:.3f} s ({s:+.1f} dB)")
    elif "jack" in cap:
        x, sr = cap["jack"]
        rep, _, tonal = find_repeats(x, block_for(sr))
        if rep and not tonal:
            fails.append(f"jack repeated block at {rep[0] / sr:.2f} s (no USB to compare)")
    return fails, notes, num


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
    """[dict(num, name, cat, lanes=[None | dict(n, a, v, p, r, muted)])], num 0 = F001"""
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
        name = re.sub(r"\s+fx\d+\s*$", "", head).strip()
        lanes = []
        for part in rest.split("|")[:4]:
            part = part.strip()
            m = re.match(r"[A-Z]:.*?\bn(\d+)\s+a(\d+)\b", part)
            if not m:
                lanes.append(None)
                continue
            v = re.search(r"\bv(\d+)\b", part)
            pn = re.search(r"\bp(\d+)\b", part[m.end():])
            r = re.search(r"\br(F|M|B|-?\d+)\b", part[m.end():])
            lanes.append(dict(n=int(m.group(1)), a=int(m.group(2)), v=int(v.group(1)) if v else 100,
                              p=int(pn.group(1)) if pn else 64, r=r.group(1) if r else "F",
                              muted=bool(re.search(r"\sm\s*$", part))))
        out.append(dict(num=len(out), name=name, cat=cat, lanes=lanes + [None] * (4 - len(lanes))))
    return out


def preset_tag(p):
    return "F%03d" % (p["num"] + 1)


def prog(p):
    """the preset's MIDI program (F001 .. F100: 0 .. 99, F101 ..: 116 ..; tests/scripts/presets.txt)"""
    return p["num"] if p["num"] < 100 else p["num"] + 16


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
    if spec.strip().lower() == "all":
        return list(allp)
    out = []
    for s in (t.strip() for t in spec.split(",") if t.strip()):
        m = re.fullmatch(r"[Ff]?(\d{1,3})", s)
        if m and 1 <= int(m.group(1)) <= len(allp):
            out.append(allp[int(m.group(1)) - 1])
            continue
        m = re.fullmatch(r"[Ff]?(\d{1,3})-[Ff]?(\d{1,3})", s)
        if m:
            out += allp[int(m.group(1)) - 1:int(m.group(2))]
            continue
        hit = [p for p in allp if p["name"].lower() == s.lower()] or [p for p in allp if s.lower() in p["name"].lower()]
        if not hit:
            raise SystemExit(f"no preset {s!r}")
        out.append(hit[0])
    return out


# ---------------------------------------------------------------- the host's renders (build/fw/textura-host)

class Host:
    """renders of the firmware on this computer (the same presets and engines; EFFECTS ON LOAD ALWAYS), cached"""

    def __init__(self, binary=HOST):
        self.bin = binary if os.path.exists(binary) else None
        self.cache = {}

    def render(self, p, sec, solo=None, settle=2.0):
        """(x, sr) of preset p after its load and `settle` s, `sec` s long (solo: only that lane, 0 .. 3, the
        others' CC 7 at 0); None without the host binary"""
        if not self.bin:
            return None
        key = (p["num"], round(sec, 2), solo, settle)
        if key in self.cache:
            return self.cache[key]
        t0 = 1500
        lines = ["1000 midi 0xB4 99 4", "1000 midi 0xB4 98 38", f"1000 midi 0xB4 6 {FX_ALWAYS}",
                 f"{t0} midi 0xC0 {prog(p)} 0"]
        if solo is not None:
            for ch in range(4):
                if ch != solo:
                    lines.append(f"{t0 + 100} midi 0x{0xB0 + ch:X} 7 0")
        start = t0 + int(settle * 1000)
        lines.append(f"{start + int(sec * 1000) + 100} end")
        with tempfile.TemporaryDirectory() as d:
            sp = os.path.join(d, "s.txt")
            with open(sp, "w") as f:
                f.write("\n".join(lines) + "\n")
            out = os.path.join(d, "r")
            r = subprocess.run([self.bin, out, sp], capture_output=True, text=True)
            if r.returncode != 0 or not os.path.exists(out + ".wav"):
                self.cache[key] = None
                return None
            x, sr = read_wav(out + ".wav")
        x = x[int(start / 1000 * sr):int((start / 1000 + sec) * sr)]
        self.cache[key] = (x, sr)
        return self.cache[key]


# ---------------------------------------------------------------- the rig

class Report:
    def __init__(self, save=None):
        self.fails, self.lines, self.save = 0, [], save
        self.rows = []

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
    """MIDI out to "Textura" (and the M6's MIDI OUT into the TRS jack), the debug reply in, the two recordings
    (needs mido, sounddevice)"""

    def __init__(self, midi="usb"):
        import mido
        import sounddevice as sd
        import tx_ports
        self.mido, self.sd = mido, sd
        names = tx_ports.pick(mido.get_output_names())
        if not names:
            raise SystemExit("hil: " + tx_ports.why_none(mido.get_output_names()))
        self.out = mido.open_output(names[0])
        self.port = names[0]
        ins = tx_ports.pick(mido.get_input_names())
        self.inp = mido.open_input(ins[0]) if ins else None
        self.trs = None
        if midi in ("trs", "both"):
            if TRS_PORT not in mido.get_output_names():
                raise SystemExit(f"hil: --midi {midi}: no MIDI port {TRS_PORT!r} (the M6's MIDI OUT into the TRS jack)")
            self.trs = mido.open_output(TRS_PORT)
        self.via = "trs" if midi == "trs" else "usb"
        devs = [d["name"] for d in sd.query_devices()]
        self.usb = USB_DEV if USB_DEV in devs else None
        self.jack = JACK_DEV if JACK_DEV in devs else None
        self.overflows = 0
        if not self.usb:
            print(f"hil: no USB audio device {USB_DEV!r}: USB analysis skipped")
        if not self.jack:
            print(f"hil: no audio device {JACK_DEV!r}: jack analysis skipped")

    def _o(self, via):
        via = via or self.via
        return self.trs if via == "trs" and self.trs else self.out

    def cc(self, ch, n, v, via=None):
        self._o(via).send(self.mido.Message("control_change", channel=ch, control=n, value=v))

    def nrpn(self, scope, idx, v, ch=4, via=None):
        self.cc(ch, 99, scope, via); self.cc(ch, 98, idx, via); self.cc(ch, 6, v, via)
        time.sleep(0.04)

    def pc(self, n, via=None):
        self._o(via).send(self.mido.Message("program_change", channel=0, program=n))

    def ask(self, via=None):
        """the debug reply (fm1_dbg.ask: CC 116 over `via`, the answer over USB), {} on none"""
        if not self.inp:
            return {}
        import fm1_dbg
        return fm1_dbg.ask(self.inp, self._o(via)) or {}

    def record(self, sec, sched=None):
        """sec s of both recordings after SETTLE; sched [(t, fn)]: fn() at t s after the settle"""
        sd = self.sd
        total = sec + SETTLE
        bufs, streams = {}, []
        lost = [0]

        def mk(key, dev, sr, ch, extra=None):
            bufs[key] = []

            def cb(data, frames, t, status):
                if status.input_overflow:
                    lost[0] += 1
                bufs[key].append(data.copy())
            streams.append((key, sd.InputStream(device=dev, samplerate=sr, channels=ch, dtype="float32", callback=cb,
                                                extra_settings=extra)))
        if self.usb:
            extra = None
            if sys.platform == "darwin":     # the device at 44.1 kHz: its own samples, never resampled by CoreAudio
                try:
                    extra = sd.CoreAudioSettings(change_device_parameters=True, fail_if_conversion_required=True)
                except Exception:
                    extra = None
            mk("usb", self.usb, USB_SR, 2, extra)
        if self.jack:
            mk("jack", self.jack, JACK_SR, 6)
        for _, s in streams:
            s.start()
        t0 = time.time()
        if sched:
            for t, fn in sorted(sched, key=lambda e: e[0]):
                while time.time() - t0 < SETTLE + t:
                    time.sleep(0.002)
                fn()
        while time.time() - t0 < total:
            time.sleep(0.005)
        for _, s in streams:
            s.stop(); s.close()
        self.overflows += lost[0]
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

    def __init__(self, live, wavdir, save, dry, rep, host=None):
        self.live, self.wavdir, self.save, self.dry, self.rep = live, wavdir, save, dry, rep
        self.host = host or Host()
        self.cache = {}

    @staticmethod
    def key(*parts):
        return "__".join(re.sub(r"[^A-Za-z0-9&-]+", "_", p).strip("_") for p in parts)

    def capture(self, key, setup, sec, sched=None, after=None):
        """{'usb': (x, sr), 'jack': (x, sr)} (missing keys: not recorded), None when there is nothing. after(cap):
        live only, right after the recording (the debug reply); its dict is saved as key__info.json"""
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
            ip = os.path.join(self.wavdir, f"{key}__info.json")
            if os.path.exists(ip):
                out["info"] = json.load(open(ip))
            self.cache[key] = out
            return out
        setup()
        out = self.live.record(sec, sched)
        if after:
            out["info"] = after()
        if self.save:
            for k, v in out.items():
                if k == "info":
                    with open(os.path.join(self.save, f"{key}__info.json"), "w") as f:
                        json.dump(v, f)
                else:
                    write_wav(os.path.join(self.save, f"{key}__{k}.wav"), v[0], v[1])
        self.cache[key] = out
        return out


def audio(cap):
    return {k: v for k, v in cap.items() if k in ("usb", "jack")}


# ---------------------------------------------------------------- checks

def reset_globals(live, via=None):
    live.nrpn(4, G_OUTPUT, OUT_SPEAKERS, via=via)
    live.nrpn(4, G_AMOUNT, AMOUNT_100, via=via)
    live.nrpn(4, G_HEAD, HEAD["MEDIUM"], via=via)
    for i, v in MS_DEFAULT.items():
        live.nrpn(4, i, v, via=via)


def load(live, p, pans=None, settle=2.0, via=None):
    live.pc(prog(p), via=via)
    time.sleep(settle)
    if pans is not None:
        for ch, pan in enumerate(pans):
            live.cc(ch, 10, pan, via=via)
        time.sleep(0.3)


def paths(state, i):
    """(the program change's path, the OUTPUT NRPN's path) for the i-th preset"""
    if state.midi == "both":
        return ("usb", "trs") if i % 2 == 0 else ("trs", "usb")
    return (state.midi, state.midi)


def cpu_of(it):
    """the debug reply's audio CPU since the last ask (fm1_dbg: 24 longest interrupt, 64 mean, 66 late, 67 counted)"""
    if not it or 24 not in it:
        return None
    return dict(isr_max=it.get(24), isr_mean=it.get(64), late=it.get(66), blocks=it.get(67), output=it.get(69),
                c1_lost=it.get(34), pool=it.get(42))


def cpu_verdict(c):
    if not c:
        return None, "no debug reply"
    late = c.get("late")
    good = (late == 0 or late is None) and c["isr_max"] <= BLOCK_US
    return good, (f"late {late} / {c.get('blocks')} blocks, interrupt longest {c['isr_max']} us mean "
                  f"{c.get('isr_mean')} (a block {BLOCK_US})" + (f", core 1 jobs lost {c['c1_lost']}" if c.get("c1_lost") else ""))


def check_clicks(src, rep, presets, sec, state):
    rep.say("== clicks: SPEAKERS and HEADPHONES, jack and USB at once (digital artefacts, CPU, host comparison)")
    prev = None
    for i, p in enumerate(presets):
        via_pc, via_out = paths(state, i)
        host = src.host.render(p, sec, settle=2.5) if not src.dry else None
        ref = src.host.render(p, HOST_LR_SEC, settle=2.5) if not src.dry else None
        for mode, ov in (("SPEAKERS", OUT_SPEAKERS), ("HEADPHONES", OUT_HEADPHONES)):
            def setup(p=p, ov=ov, mode=mode):
                state.first(p)
                if mode == "SPEAKERS":
                    src.live.nrpn(4, G_SCNFX, FX_ALWAYS, via=via_pc)
                    state.fx_set = True
                    reset_globals(src.live, via=via_pc)
                    load(src.live, p, settle=2.5, via=via_pc)
                else:
                    src.live.nrpn(4, G_OUTPUT, ov, via=via_out)
                    time.sleep(0.6)
                state.pre = src.live.ask(via=via_out)          # (restarts the "since the last ask" items)

            def after():
                return dict(cpu=cpu_of(src.live.ask()), pre_output=(state.pre or {}).get(69), via_pc=via_pc,
                            via_out=via_out)
            tag = f"{preset_tag(p)} {p['name']} {mode}"
            cap = src.capture(Source.key("clicks", preset_tag(p), p["name"], mode), setup, sec, after=after)
            if not cap:
                continue
            fails, notes, num = judge_clicks(audio(cap), host if mode == "SPEAKERS" else None, ref)
            info = cap.get("info") or {}
            parts = []
            if "usb" in num:
                k, lam, ph, pp = num["usb"]["grid"]
                parts.append(f"usb {num['level']:.1f} dBFS" + (f" (host {num['host_level']:.1f})" if "host_level" in num else "")
                             + f", crackle {num['crackle']:.0f}/10 s"
                             + (f" ({num['crackle_n']:.0f} at the host's level; host {num['host_crackle'][0]:.0f}, at most {num['host_crackle'][1]:.0f})" if "host_crackle" in num else "")
                             + f", grid {k}/{lam:.1f} p {pp:.0e}")
            if num.get("jack") and num["jack"]["ok"] is not None and not num["jack"]["steps"]:
                jv = num["jack"]
                parts.append(f"jack fit {jv['model_db']:.0f} dB, worst {jv['worst']:+.1f} dB, drift {jv['drift_ppm']:.0f} ppm"
                             + (", sides swapped" if jv["swap"] else ""))
            if mode == "SPEAKERS" and "usb" in cap and ref is not None:
                x_, sr_ = cap["usb"]
                d_this = best_shape(x_, sr_, ref)
                good_id, txt = True, f"sound vs its host render {d_this:.1f} dB (the closest of its windows)"
                if prev is not None and prev.get("ref") is not None:
                    d_prev = best_shape(x_, sr_, prev["ref"])
                    good_id = d_this <= max(d_prev, ID_OK)
                    txt += f", vs the previous preset's {d_prev:.1f}"
                rep.ok(f"load    {preset_tag(p)} {p['name']} (program change over {via_pc.upper()})", good_id, txt)
                state.paths[via_pc][0 if good_id else 1] += 1
            rep.ok(f"clicks  {tag}", not fails, "; ".join(parts) + ("  <- " + "; ".join(fails) if fails else "")
                   + ("  (note: " + "; ".join(notes) + ")" if notes else ""))
            c = info.get("cpu")
            good, txt = cpu_verdict(c)
            if good is not None:
                rep.ok(f"cpu     {tag}", good, txt)
            elif src.live:
                rep.skip(f"cpu     {tag}", txt)
            if mode == "HEADPHONES" and info:
                got = info.get("pre_output")
                if got is not None:
                    good = bool(got & 1)
                    rep.ok(f"nrpn    {preset_tag(p)} OUTPUT HEADPHONES over {via_out.upper()}", good,
                           f"read back OUTPUT {got} (1: HEADPHONES, +2 binaural on)")
                    state.paths[via_out][0 if good else 1] += 1
            state.rows.append(dict(preset=preset_tag(p), name=p["name"], mode=mode, fails=fails, notes=notes,
                                   level=num.get("level"), host_level=num.get("host_level"), crackle=num.get("crackle"),
                                   crackle_n=num.get("crackle_n"), host=num.get("host_crackle"), grid=(num.get("usb") or {}).get("grid"),
                                   id_db=num.get("id_db"), cpu=c, jack=(num.get("jack") or {}).get("worst")))
            if mode == "SPEAKERS":
                src.cache[("speakers", p["num"])] = cap
        if src.live:
            src.live.nrpn(4, G_OUTPUT, OUT_SPEAKERS, via=via_out)
        prev = dict(p=p, host=host, ref=ref)


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
        coh = coherence_lo(x[:, 0], x[:, 1], sr)
        lag = lag_lo(x[:, 0], x[:, 1], sr) * USB_SR / sr if coh >= LAG_COH else float("nan")
        far = s["r"] - s["l"]
        got[name] = (lag, far, s)
        lag_t = f"lag {lag:.0f}" if lag == lag else f"lag n/a (coherence {coh:.2f} < {LAG_COH})"
        if name == "SPEAKERS":
            rep.ok("binaural  SPEAKERS", far < FAR_SPEAKERS, f"left {s['l']:.1f} dBFS  far {far:.1f} dB (< {FAR_SPEAKERS:.0f})")
        elif name.startswith("HEADPHONES AMOUNT"):
            sp = got.get("SPEAKERS")
            good = far < FAR_SPEAKERS and (lag != lag or abs(lag) <= 1)
            if sp:
                good = good and abs(far - sp[1]) <= AMT0_FAR_TOL
            rep.ok("binaural  AMOUNT 0 = SPEAKERS", good,
                   f"{lag_t} (|lag| <= 1 where measurable)  far {far:.1f} dB (< {FAR_SPEAKERS:.0f}"
                   + (f", SPEAKERS' {sp[1]:.1f} +-{AMT0_FAR_TOL:.0f})" if sp else ")"))
        else:
            h = name.split()[1]
            want = HEAD_LAG[h]
            good = lag == lag and abs(lag - want) <= LAG_TOL and FAR_BINAURAL[0] <= far <= FAR_BINAURAL[1]
            rep.ok(f"binaural  {name}", good,
                   f"{lag_t} (want {want} +-{LAG_TOL})  far {far:.1f} dB (want {FAR_BINAURAL[0]:.0f}..{FAR_BINAURAL[1]:.0f})")


def check_ms(src, rep, presets, sec, state):
    rep.say(f"== M/S: lanes spread; each case alternated with the default every {MS_SEG} s ({MS_PAIRS} pairs)")
    p = presets[0]
    cases = [("MID off", {G_MID: 0}), ("SIDE off", {G_SIDE: 0}), ("WIDTH 0", {G_WIDTH: 0}), ("MONO", {G_MONO: 127})]
    first = [True]
    dflt = []
    for name, ov in cases:
        def setup():
            state.first(p)
            if first[0]:
                reset_globals(src.live)
                load(src.live, p, pans=(8, 40, 88, 120))
                time.sleep(1.0)
                first[0] = False
            for i, v in MS_DEFAULT.items():
                src.live.nrpn(4, i, v)
        sched = []
        if src.live:
            for i in range(2 * MS_PAIRS):
                for k, v in ov.items():
                    sched.append((i * MS_SEG, lambda k=k, v=(v if i % 2 else MS_DEFAULT[k]): src.live.nrpn(4, k, v)))
        cap = src.capture(Source.key("ms", name), setup, 2 * MS_PAIRS * MS_SEG + 0.2, sched=sched)
        if src.live and not src.dry:
            for i, v in MS_DEFAULT.items():
                src.live.nrpn(4, i, v)
        if not cap or "usb" not in cap:
            continue
        off, on = ms_segments(*cap["usb"])
        dflt += off
        good, txt = ms_verdict(name, off, on)
        rep.ok(f"ms  {name}", good, txt)
    if dflt:
        mid, side = float(np.median([a[0] for a in dflt])), float(np.median([a[1] for a in dflt]))
        rep.ok("ms  default", mid >= side, f"MID {mid:.1f} SIDE {side:.1f} dBFS (median of the default segments; MID >= SIDE)")


def check_stereo(src, rep, presets, sec, state):
    rep.say("== stereo after preset loads (L-R within the host render's span, or the preset's pans without a host)")
    for p in presets:
        cap = src.cache.get(("speakers", p["num"]))
        if cap is None and src.wavdir:
            k = Source.key("clicks", preset_tag(p), p["name"], "SPEAKERS")
            if os.path.exists(os.path.join(src.wavdir, f"{k}__usb.wav")):
                cap = src.capture(k, None, sec)
        if cap is None:
            def setup(p=p):
                state.first(p)
                src.live.nrpn(4, G_SCNFX, FX_ALWAYS)
                state.fx_set = True
                reset_globals(src.live)
                load(src.live, p, settle=2.5)
            cap = src.capture(Source.key("stereo", preset_tag(p), p["name"]), setup, sec)
        if not cap or "usb" not in cap:
            continue
        x, sr = cap["usb"]
        s = stereo_stats(x)
        lr = s["l"] - s["r"]
        exp = expected_lr(p)
        h = src.host.render(p, HOST_LR_SEC, settle=2.5)
        span = host_lr_span(h, len(x) / sr) if h is not None else None
        good_lr, lr_t = lr_verdict(lr, exp, span)
        good = s["mid"] >= s["side"] + STEREO_MS and good_lr and s["corr"] >= STEREO_CORR
        rep.ok(f"stereo  {preset_tag(p)} {p['name']}", good,
               f"MID {s['mid']:.1f} SIDE {s['side']:.1f} (MID >= SIDE + {STEREO_MS:.0f})  {lr_t}  corr {s['corr']:.2f} "
               f"(>= {STEREO_CORR})")


def check_activity(src, rep, presets, sec, state):
    rep.say("== activity: each lane soloed, three windows (against the host's render of the lane)")
    win = max(sec / 3.0, 1.0)
    for p in presets:
        for li in range(4):
            lane = p["lanes"][li]
            if not lane or lane["muted"]:
                continue
            want = lane["n"] > 0            # (HOW ACTIVE 0 plays too: the sparsest step)

            def setup(p=p, li=li):
                state.first(p)
                src.live.nrpn(4, G_SCNFX, FX_ALWAYS)
                state.fx_set = True
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
            hq = None
            h = src.host.render(p, HOST_ACT_SEC, solo=li, settle=3.0) if want else None
            if h is not None:
                hx, hsr = h
                hw = int(win * hsr)
                hl = [rms_db(hx[i:i + hw]) for i in range(0, len(hx) - hw + 1, hw // 2)]
                hq = float(np.mean([v < SILENT_DB for v in hl])) if hl else None
            dead, why = activity_verdict(levels, hq, want)
            rep.ok(f"activity  {preset_tag(p)} {p['name']} lane {li + 1}", not dead,
                   "windows " + " / ".join(f"{v:.1f}" for v in levels) + " dBFS"
                   + (f" (n{lane['n']} a{lane['a']})" if want else " (n0: not meant to play)")
                   + (f"  host quiet windows {hq * 100:.0f} %" if hq is not None else "") + (f"  {why}" if why else ""))
        if not src.dry and not src.wavdir:
            load(src.live, p, settle=1.0)       # levels back


def check_midi(src, rep, presets, sec, state):
    """each MIDI path on its own: the debug reply, an NRPN read back, the audio's answer to an NRPN and to program
    changes"""
    rep.say("== midi: each path (" + ", ".join(v.upper() for v in state.vias()) + "); replies over USB")
    if src.dry or src.wavdir:
        rep.skip("midi", "live only")
        return
    live = src.live
    allp = parse_presets()
    loud = [p for p in allp if p["name"] == "STEADY RAIN"] or presets[:1]     # (steady textures: a 6 s window is
                                                                                  # the preset; thunder's would not be)
    quiet = [p for p in allp if p["name"] == "DESERT WIND"] or presets[-1:]
    for via in state.vias():
        it = live.ask(via=via)
        rep.ok(f"midi  {via.upper()} debug reply (CC 116)", bool(it), f"{len(it)} items")
        live.nrpn(4, G_OUTPUT, OUT_HEADPHONES, via=via)
        time.sleep(0.3)
        hp = live.ask(via=via).get(69)
        live.nrpn(4, G_OUTPUT, OUT_SPEAKERS, via=via)
        time.sleep(0.3)
        sp = live.ask(via=via).get(69)
        rep.ok(f"midi  {via.upper()} NRPN OUTPUT read back", hp is not None and bool(hp & 1) and sp is not None and not sp & 1,
               f"HEADPHONES -> {hp}, SPEAKERS -> {sp}")
        for p in (quiet[0], loud[0]):
            state.first(p)
            live.nrpn(4, G_SCNFX, FX_ALWAYS, via=via)
            state.fx_set = True
            live.pc(prog(p), via=via)
            time.sleep(2.5)
            cap = live.record(6.0)
            h = src.host.render(p, 6.0, settle=2.5)
            if h is None or "usb" not in cap:
                rep.skip(f"midi  {via.upper()} program change", "no host render or no USB audio")
                continue
            other = quiet[0] if p is loud[0] else loud[0]
            ho = src.host.render(other, 6.0, settle=2.5)
            dl = shape_dist(band_levels(*cap["usb"]), band_levels(*h))
            do = shape_dist(band_levels(*cap["usb"]), band_levels(*ho))
            rep.ok(f"midi  {via.upper()} program change {preset_tag(p)} {p['name']}", dl < do,
                   f"sound vs its host render {dl:.1f} dB, vs {other['name']}'s {do:.1f} dB")
        base = live.record(3.0)
        live.nrpn(4, G_MID, 0, via=via)
        time.sleep(0.5)
        off = live.record(3.0)
        live.nrpn(4, G_MID, MS_DEFAULT[G_MID], via=via)
        if "usb" in base:
            a, b = stereo_stats(base["usb"][0])["mid"], stereo_stats(off["usb"][0])["mid"]
            rep.ok(f"midi  {via.upper()} NRPN MID off heard", b < a + MS_GONE, f"MID {a:.1f} -> {b:.1f} dBFS")


CHECKS = {"midi": check_midi, "clicks": check_clicks, "binaural": check_binaural, "ms": check_ms,
          "stereo": check_stereo, "activity": check_activity}


class State:
    def __init__(self, midi="usb"):
        self.preset = None
        self.midi = midi
        self.fx_set = False
        self.pre = None
        self.rows = []
        self.paths = {"usb": [0, 0], "trs": [0, 0]}

    def vias(self):
        return ["usb", "trs"] if self.midi == "both" else [self.midi]

    def first(self, p):
        if self.preset is None:
            self.preset = p


def restore(live, state, rep):
    """the unit back at its defaults; prints what was restored"""
    p = state.preset or dict(num=0, name="(F001)")
    live.pc(prog(p) if "cat" in p else 0)
    time.sleep(2.0)
    reset_globals(live)
    for ch in range(4):
        live.cc(ch, 10, 64)
    if state.fx_set:
        live.nrpn(4, G_SCNFX, FX_NEVER)
    time.sleep(0.3)
    rep.say(f"restored: OUTPUT SPEAKERS, AMOUNT 100, HEAD SIZE MEDIUM, M/S defaults (MID, SIDE, WIDTH, MONO), "
            f"preset {preset_tag(p) if 'cat' in p else 'F001'} {p['name']} reloaded, lane pans centre"
            + (", EFFECTS ON LOAD NEVER (the FM-1's default)" if state.fx_set else ""))


# ---------------------------------------------------------------- main

def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0], formatter_class=argparse.RawTextHelpFormatter)
    ap.add_argument("--only", help="comma list of: " + ",".join(CHECKS) + " (default: all but midi; --midi both adds it)")
    ap.add_argument("--presets", help="all, or a comma list: F007, 7, 3-12 or a name (default: the busiest rain, storm, fire, sand)")
    ap.add_argument("--midi", choices=("usb", "trs", "both"), default="usb",
                    help="the commands' path: USB, TRS (the M6's MIDI OUT) or both (split, and the midi check)")
    ap.add_argument("--seconds", type=float, default=20.0, help="recording per case (default 20; binaural caps at 6, stereo at 8, activity at 15 = 3 windows; ms is its own)")
    ap.add_argument("--save", help="directory for the WAVs and summary.txt")
    ap.add_argument("--wav", help="analyse recordings from this directory, no hardware")
    ap.add_argument("--dry-run", action="store_true", help="print the plan, touch nothing")
    ap.add_argument("--no-restore", action="store_true", help="leave the unit as the last check set it")
    ap.add_argument("--no-host", action="store_true", help="no comparisons with the host's renders")
    a = ap.parse_args(argv)

    if a.only:
        names = [n.strip() for n in a.only.split(",")]
    else:
        names = [n for n in CHECKS if n != "midi" or a.midi == "both"]
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
        live = Live(a.midi)
        rep.say(f"hil: MIDI port {live.port!r}" + (f" and {TRS_PORT!r} (TRS)" if live.trs else "")
                + f", USB audio {live.usb!r}, jack {live.jack!r} (channels {JACK_CH[0] + 1}/{JACK_CH[1] + 1})")
    host = Host(binary="" if a.no_host else HOST)
    if not host.bin and not a.no_host:
        rep.say(f"hil: no {os.path.relpath(HOST, ROOT)} (make fw-host): no host comparisons")
    src = Source(live, a.wav, a.save, a.dry_run, rep, host)
    state = State(a.midi)
    rep.say("presets: " + ", ".join(f"{preset_tag(p)} {p['name']}" for p in presets))
    t_start = time.time()
    try:
        for n in [c for c in CHECKS if c in names]:
            sec = a.seconds
            if n == "binaural":
                sec = min(sec, 6.0)
            elif n == "stereo":
                sec = min(sec, 8.0)
            elif n == "activity":
                sec = min(sec, 15.0)
            ps = presets
            if n == "activity" and not a.presets:
                ps = presets[:3]
            if n in ("binaural", "ms") and (not a.presets or len(presets) > 10):
                ps = [p for p in allp if p["name"] == "STEADY RAIN"] or presets
            CHECKS[n](src, rep, ps, sec, state)
    finally:
        if live and not a.no_restore:
            restore(live, state, rep)
    if a.midi == "both" and "clicks" in names:
        for via in ("usb", "trs"):
            g, b = state.paths[via]
            rep.say(f"midi path {via.upper()}: {g} commands answered, {b} not (program changes heard, NRPNs read back)")
    if live and live.overflows:
        rep.say(f"note: {live.overflows} audio input overflow(s) on this computer during the recordings (the rig's, not the unit's)")
    rep.say(f"{'FAIL' if rep.fails else 'ok'}: {rep.fails} failure(s)  ({(time.time() - t_start) / 60:.1f} min)")
    if a.save:
        with open(os.path.join(a.save, "summary.txt"), "w") as f:
            f.write("\n".join(rep.lines) + "\n")
        with open(os.path.join(a.save, "rows.json"), "w") as f:
            json.dump(state.rows, f, default=str)
    return min(rep.fails, 125)


if __name__ == "__main__":
    sys.exit(main())
