#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""The FM-1's headphone jack against its USB audio, per landscape: peak, short-term level, the floor, SNR.

From Frodhes' tools/hil_jack.py (the jack and USB recorded at once while MIDI plays the FM-1), with the persistent
recorder (Ring, Capture, to_fs) from its tools/hil_scan.py; adapted to Textura: the landscapes are the factory scenes
(program change on channel 1: F01 .. F10, each landscape at its start), the lanes are channels 1 .. 4 (CC 22 HOW
ACTIVE starts a lane), the mix's COMPRESSOR is switched by CC 95 on channel 5 (COMP: 0 off, 127 on; params.c).

  hil_jack.py [--dev M6] [--ch 5] [--only NAME[,NAME]] [--secs 15] [--comp 0,127] [--port NAME] [--dry-run]

The rig: the FM-1's USB audio (44.1 kHz) and USB MIDI (the port named "Textura": the only MIDI port opened;
"Frodhes" only by --port Frodhes, a unit still on the old firmware, never beside a Textura port: tools/tx_ports.py), its
headphone jack into the interface --dev input --ch (the MOTU M6 input 5, 48 kHz). The interface's input gain is never
touched: every jack figure is re its full scale, so the differences are what matter.

First a preflight: F01's four lanes playing must read >= 20 dB over the input's silence, else "jack not connected"
(exit 1, nothing measured). Per landscape: a silence (all lanes off, USB exact zero) then, for each COMP: the landscape's default start (its scene)
and all four lanes at HOW ACTIVE 64 (CC 22 = 64 on channels 1 .. 4), MASTER as the user left it (fully up):
  USB / jack peak        dBFS / dB re the interface's full scale
  p95 short-term RMS     the 95th percentile of 400 ms RMS windows (100 ms hop)
  floor                  the jack's RMS at silence (the codec as the FM-1 leaves it at USB exact zero) and USB's
                         nonzero samples there (0: the digital silence gate)
  SNR                    the jack's p95 short-term RMS over its floor
  jack re USB            p95 RMS jack - USB: with MASTER (VOLUME) at max it reads Frodhes' calibration, -12.65 dB
                         (the same rig); lower means the knob is not at max (USB is before the knob only if the
                         reading moves with it: if it does not move, whether MASTER is at max needs the user)
Results to build/hil/jack-landscapes.json; a table on stdout. --dry-run: the plan only (no MIDI, no audio).
The scan leaves the FM-1 on F01 with the lanes stopped and COMP back at its default (the scene load restores it).
"""
import argparse
import json
import os
import sys
import time

import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FS = 48000
# the FM-1's MIDI port and USB audio, never IAC or any other port (tools/tx_ports.py; python -I: the tools' own helper)
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import tx_ports  # noqa: E402
LANDSCAPES = ("WATER", "STONES & SAND", "ICE & GLASS", "FIRE", "WOOD", "WIND", "STORM", "INSECTS", "SONGBIRDS",
              "WILD BIRDS")                     # (landscapes.h order: factory scene F01 .. F10)
CH_GLOBAL = 4                                  # (0-based: channel 5)
CC_ACTIVE, CC_COMP = 22, 95
CAL_JACK_RE_USB = -12.65                       # Frodhes' rig: jack = USB - 12.65 dB at VOLUME max


# ---- the persistent recorder (Frodhes tools/hil_scan.py: Ring, Capture, to_fs)
class Ring:
    """one input stream's recent history (secs at the device's own rate, float32; only the channels kept): every
    callback block pushed with its ADC time (PortAudio's stream clock), so a take is cut by TIME, not by stream start"""

    def __init__(self, sr, ch, secs=120.0):
        import threading
        self.sr, self.ch = int(sr), int(ch)
        self.N = int(secs * sr)
        self.buf = np.zeros((self.N, ch), np.float32)
        self.w = 0
        self.lock = threading.Lock()
        self.M = max(64, self.N // 32)
        self.lf = np.zeros(self.M, np.int64)
        self.la = np.zeros(self.M, np.float64)
        self.nlog = 0
        self.gaps = []

    def push(self, block, adc):
        n = len(block)
        if not n:
            return
        if n > self.N:
            block, adc = block[-self.N:], adc + (n - self.N) / self.sr
            self.w += n - self.N
            n = self.N
        i = self.w % self.N
        k = min(n, self.N - i)
        self.buf[i:i + k] = block[:k]
        if k < n:
            self.buf[:n - k] = block[k:]
        with self.lock:
            j = self.nlog % self.M
            self.lf[j], self.la[j] = self.w, float(adc)
            self.nlog += 1
            self.w += n

    def _snap(self, t0, back=64):
        with self.lock:
            c, w = self.nlog, self.w
            lo = max(0, c - self.M)
            if c == lo:
                return None, w
            a, b = lo, c
            while a < b:
                m = (a + b) // 2
                if self.la[m % self.M] <= t0:
                    a = m + 1
                else:
                    b = m
            idx = np.arange(max(lo, a - 1 - back), c) % self.M
            L = np.empty((len(idx), 2), float)
            L[:, 0], L[:, 1] = self.lf[idx], self.la[idx]
        return L, w

    def end_time(self):
        with self.lock:
            if not self.nlog:
                return None
            j, w = (self.nlog - 1) % self.M, self.w
            f, a = int(self.lf[j]), float(self.la[j])
        return a + (w - 1 - f) / self.sr

    def cut(self, t0, dur, tol=0.006):
        """the frames from stream time t0 for dur s, placed by the blocks' ADC times (a lost block stays zeros, so
        what follows is still on the event clock); None while they are not all in yet"""
        L, w = self._snap(t0)
        n = int(round(dur * self.sr))
        if L is None:
            return None
        if L[-1, 1] + (w - 1 - L[-1, 0]) / self.sr < t0 + (n - 1) / self.sr:
            return None
        ends = np.append(L[1:, 0], w)
        k0 = max(0, int(np.searchsorted(L[:, 1], t0, "right")) - 1)
        out = np.zeros((n, self.ch), np.float32)
        self.gaps = []
        pos = None
        for k in range(k0, len(L)):
            a, f0, f1 = L[k, 1], int(L[k, 0]), int(ends[k])
            if pos is None:
                pos = int(round((a - t0) * self.sr))
            else:
                step = a - L[k - 1, 1] - (f0 - L[k - 1, 0]) / self.sr
                if abs(step) > tol:
                    self.gaps.append((float(a), float(step)))
                    pos += int(round(step * self.sr))
            if pos >= n:
                break
            lo, hi = max(f0, w - self.N, f0 - pos), min(f1, f0 + n - pos)
            if hi > lo:
                out[pos + lo - f0:pos + hi - f0] = self.buf[np.arange(lo, hi) % self.N]
            pos += f1 - f0
        return out


class Capture:
    """one InputStream per device, opened once for the whole run (the FM-1 keeps its flash saves back while a
    computer reads its USB audio); each take a slice by the streams' ADC times"""

    def __init__(self, sd):
        self.sd, self.rings, self.streams = sd, {}, {}

    def add(self, key, dev, ch_keep, n_in, secs=120.0):
        sd = self.sd
        sr = int(sd.query_devices(dev)["default_samplerate"])
        ring = Ring(sr, len(ch_keep), secs)
        sel = list(ch_keep)

        def cb(indata, frames, ti, status):
            adc = ti.inputBufferAdcTime or (ti.currentTime - frames / sr)
            ring.push(indata[:, sel], adc)
        s = sd.InputStream(device=dev, samplerate=sr, channels=n_in, dtype="float32", callback=cb)
        s.start()
        self.rings[key], self.streams[key] = ring, s
        t0 = time.monotonic()
        while ring.end_time() is None and time.monotonic() - t0 < 5:
            time.sleep(0.01)

    def now(self):
        return {k: s.time for k, s in self.streams.items()}

    def cut(self, key, t0, dur, timeout=5.0):
        r = self.rings[key]
        lim = time.monotonic() + dur + timeout
        while True:
            x = r.cut(t0, dur)
            if x is not None:
                return x
            if time.monotonic() > lim:
                raise RuntimeError(f"{key}: the stream stopped delivering ({r.w} frames)")
            time.sleep(0.01)

    def close(self):
        for s in self.streams.values():
            try:
                s.stop()
                s.close()
            except Exception:
                pass
        self.streams = {}


def to_fs(x, sr, dur):
    """a capture at a device's own rate -> FS (polyphase), dur s long; (n,) or (n, ch)"""
    if sr != FS:
        from math import gcd
        from scipy.signal import resample_poly
        g = gcd(FS, sr)
        x = resample_poly(x, FS // g, sr // g, axis=0)
    n = int(dur * FS)
    return np.pad(x, [(0, max(0, n - len(x)))] + [(0, 0)] * (np.ndim(x) - 1))[:n]


# ---- the rig
class Rig:
    def __init__(self, devname, ch, port=None):
        import atexit
        import mido
        import sounddevice as sd
        self.mido = mido
        names = tx_ports.pick(mido.get_output_names(), port)
        if not names:
            sys.exit(f"no FM-1 MIDI port: {tx_ports.why_none(mido.get_output_names(), port)}")
        self.out = mido.open_output(names[0])
        devs = sd.query_devices()
        ok = tx_ports.pick([d["name"] for d in devs if d["max_input_channels"] >= 2], port)
        usb = [i for n in ok[:1] for i, d in enumerate(devs) if d["name"] == n and d["max_input_channels"] >= 2]
        jack = [i for i, d in enumerate(devs) if d["name"] == devname and d["max_input_channels"] >= ch]
        if not usb or not jack:
            sys.exit(f"no FM-1 USB audio input or no '{devname}' input {ch}")
        print(f"MIDI '{names[0]}', USB '{devs[usb[0]]['name']}', jack '{devname}' input {ch}", flush=True)
        self.cap = Capture(sd)
        self.cap.add("usb", usb[0], (0, 1), 2)
        self.cap.add("jack", jack[0], (ch - 1,), int(devs[jack[0]]["max_input_channels"]))
        atexit.register(self.cap.close)

    def send(self, kind, ch, a, b=0):
        M = self.mido.Message
        self.out.send(M("program_change", channel=ch, program=a) if kind == "pc" else
                      M("control_change", channel=ch, control=a, value=b))

    def both(self, dur):
        st = self.cap.now()
        return {k: to_fs(self.cap.cut(k, st[k], dur).astype(float), self.cap.rings[k].sr, dur) for k in ("usb", "jack")}

    def wait_zero(self, limit=20.0):
        """until USB is exact zero for 0.5 s (the lanes' tails rung out): True, or False at the limit"""
        t0 = time.monotonic()
        while time.monotonic() - t0 < limit:
            if not np.any(self.both(0.5)["usb"]):
                return True
        return False


def db(x):
    return float(20 * np.log10(np.sqrt(np.mean(np.asarray(x, float) ** 2)) + 1e-15))


def peak_db(x):
    return float(20 * np.log10(np.abs(np.asarray(x)).max() + 1e-15))


def p95_st(x, win=0.4, hop=0.1):
    """the 95th percentile of short-term RMS (dB): win s windows every hop s; x (n,) or (n, ch) (power summed over
    the channels' mean)"""
    x = np.asarray(x, float)
    p = (x ** 2).mean(axis=1) if x.ndim == 2 else x ** 2
    n, h = int(win * FS), int(hop * FS)
    c = np.concatenate([[0.0], np.cumsum(p)])
    s = np.arange(0, len(p) - n + 1, h)
    ms = (c[s + n] - c[s]) / n
    return float(10 * np.log10(np.percentile(ms, 95) + 1e-30))


def lanes_off(r):
    for k in range(4):
        r.send("cc", k, CC_ACTIVE, 0)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dev", default="M6")
    ap.add_argument("--ch", type=int, default=5)
    ap.add_argument("--only", default="", help="landscapes by name, comma separated")
    ap.add_argument("--secs", type=float, default=15.0, help="each take's length (after 4 s to settle)")
    ap.add_argument("--comp", default="0,127", help="COMP off / on (CC 95 on channel 5: 0 off, 127 on, at its defaults)")
    ap.add_argument("--port", help="the FM-1's MIDI port and USB audio (part of the name; default: Textura)")
    ap.add_argument("--dry-run", action="store_true")
    a = ap.parse_args()
    only = [s.strip().upper() for s in a.only.split(",") if s.strip()]
    todo = [(i, n) for i, n in enumerate(LANDSCAPES) if not only or n in only]
    comps = [int(v) for v in a.comp.split(",")]
    if a.dry_run:
        for i, n in todo:
            print(f"{n}: PC {i} ch 1; silence; " + "; ".join(
                f"COMP {c} (CC {CC_COMP} ch 5): default, then CC {CC_ACTIVE}=64 ch 1-4" for c in comps))
        print(f"~{len(todo) * (len(comps) * 2 * (a.secs + 6) + 20) / 60:.0f} min")
        return
    r = Rig(a.dev, a.ch, a.port)
    # the jack really there: F01 with its four lanes playing must read well over the input's silence (else the input
    # is not wired: stop before measuring a floor as music)
    r.send("pc", 0, 0)
    time.sleep(1.5)
    lanes_off(r)
    r.wait_zero()
    q = r.both(2.0)["jack"][:, 0]
    for k in range(4):
        r.send("cc", k, CC_ACTIVE, 64)
    time.sleep(4.0)
    d = r.both(4.0)
    lanes_off(r)
    on, off = p95_st(d["jack"][:, 0]), db(q)
    print(f"preflight: USB p95 {p95_st(d['usb']):.1f} dBFS; input {a.ch} p95 {on:.1f} dB, silent {off:.1f} dB", flush=True)
    if on - off < 20.0 or p95_st(d["usb"]) < -60:
        sys.exit(f"jack not connected (input {a.ch} reads {on - off:.1f} dB over its silence with F01 playing)")
    r.wait_zero()
    rep = dict(calibration_jack_re_usb_db=CAL_JACK_RE_USB, secs=a.secs, rows=[])
    for i, name in todo:
        r.send("pc", 0, i)
        time.sleep(1.5)
        lanes_off(r)
        zero = r.wait_zero()
        d = r.both(3.0)
        sil = dict(usb_zero=bool(zero and not np.any(d["usb"])), usb_nonzero=int(np.count_nonzero(d["usb"])),
                   jack_db=db(d["jack"]))
        for c in comps:
            for mode in ("default", "all4 ACT64"):
                r.send("pc", 0, i)                     # (the scene: the landscape's start; COMP back to its default)
                time.sleep(1.5)
                r.send("cc", CH_GLOBAL, CC_COMP, c)
                if mode != "default":
                    for k in range(4):
                        r.send("cc", k, CC_ACTIVE, 64)
                time.sleep(4.0)
                d = r.both(a.secs)
                u, j = d["usb"], d["jack"][:, 0]
                row = dict(landscape=name, comp=c, mode=mode, usb_peak=peak_db(u), jack_peak=peak_db(j),
                           usb_p95=p95_st(u), jack_p95=p95_st(j), jack_floor=sil["jack_db"],
                           usb_silence_nonzero=sil["usb_nonzero"], usb_full_scale=int(np.sum(np.abs(u) >= 32767 / 32768)),
                           gaps=len(r.cap.rings["usb"].gaps) + len(r.cap.rings["jack"].gaps))
                row["snr"] = row["jack_p95"] - row["jack_floor"]
                row["jack_re_usb"] = row["jack_p95"] - row["usb_p95"]
                rep["rows"].append(row)
                print(f"{name:10s} COMP {c:3d} {mode:10s}  USB pk {row['usb_peak']:6.1f} p95 {row['usb_p95']:6.1f} | jack "
                      f"pk {row['jack_peak']:6.1f} p95 {row['jack_p95']:6.1f} floor {row['jack_floor']:6.1f} SNR "
                      f"{row['snr']:5.1f} | jack-USB {row['jack_re_usb']:+6.2f} (silence USB nonzero "
                      f"{row['usb_silence_nonzero']})", flush=True)
                lanes_off(r)
                r.wait_zero()
    r.send("pc", 0, 0)                                 # F01, lanes stopped, COMP at its default
    time.sleep(1.5)
    lanes_off(r)
    od = os.path.join(ROOT, "build", "hil")
    os.makedirs(od, exist_ok=True)
    json.dump(rep, open(os.path.join(od, "jack-landscapes.json"), "w"), indent=1)
    print(f"-> {os.path.join(od, 'jack-landscapes.json')}")


if __name__ == "__main__":
    main()
