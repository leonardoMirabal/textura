#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Drive txfm1 (bench/fm1/txfm1.c) on the FM-1 over USB-MIDI: every configuration of its table run on the device,
the results written to build/fm1/device.csv and set against the host twin's (build/fm1/host.csv, make fm1-host).

  tools/fm1_timing.py [--only 0-34,219] [--runs 1] [--spread 20] [--no-usb48] [--resume] [--port NAME]

Installs nothing: the bench must already run on the FM-1 (identity FM-1_959). Run it under the DEVICE lock (make
fm1-timing). Each configuration: its index (CC 20 / 21), run (CC 22 n), the state polled (CC 24) until done, then
each run's results (CC 23 r). A configuration that does not finish in time stops the whole run (exit 3): nothing
is retried on the device.

--spread N: the N configurations with the highest host w8 also get 3 runs (the device's own spread).
--no-usb48: the four-lane cases without the USB audio stream (by default it is opened at 48 kHz, as Frodhes' CPU
checks do: the 48 kHz resampler is part of the worst interrupt). Needs mido + python-rtmidi (+ sounddevice).
"""
import argparse
import csv
import re
import sys
import time
from pathlib import Path

import mido

ROOT = Path(__file__).resolve().parents[1]
FM1 = ROOT / "build" / "fm1"
PORT_RE = re.compile(r"fm-1|txfm1|textura|felucca|dinghy|frodhes", re.I)   # the bench keeps Frodhes' USB name
TPB_MS = 256 * 1000 / 44100


def labels():
    txt = (FM1 / "gen" / "fm1_cfg.h").read_text()
    kinds = [int(m.group(1)) for m in re.finditer(r"^  \{(\d+),\d+,\d+,\d+,\d+,\d+,(\d+),", txt, re.M)]
    nblk = [int(m.group(2)) for m in re.finditer(r"^  \{(\d+),\d+,\d+,\d+,\d+,\d+,(\d+),", txt, re.M)]
    lab = re.findall(r'^  "(.*)",$', txt, re.M)
    return lab, kinds, nblk


class Dev:
    def __init__(self, name):
        ins = [n for n in mido.get_input_names() if (name and name in n) or (not name and PORT_RE.search(n))]
        outs = [n for n in mido.get_output_names() if (name and name in n) or (not name and PORT_RE.search(n))]
        if not ins or not outs:
            raise SystemExit(f"fm1_timing: no FM-1 port (in {mido.get_input_names()}, out {mido.get_output_names()})")
        self.i, self.o = mido.open_input(ins[0]), mido.open_output(outs[0])
        self.name = outs[0]

    def cc(self, c, v):
        self.o.send(mido.Message("control_change", channel=0, control=c, value=v))

    def items(self, until, timeout=2.0):
        """the CC 100 / 101 / 102 items until item `until` arrives: {id: value}"""
        got, cur, msb, t0 = {}, None, 0, time.time()
        while time.time() - t0 < timeout:
            m = self.i.poll()
            if m is None:
                time.sleep(0.002)
                continue
            if m.type != "control_change":
                continue
            if m.control == 100:
                cur = m.value
            elif m.control == 101:
                msb = m.value
            elif m.control == 102 and cur is not None:
                got[cur] = msb << 7 | m.value
                if cur == until:
                    return got
        return None

    def state(self):
        self.cc(24, 0)
        return self.items(98)


def parse_only(s, n):
    out = []
    for part in s.split(","):
        a, _, b = part.partition("-")
        out += list(range(int(a), (int(b) if b else int(a)) + 1))
    return [i for i in out if 0 <= i < n]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--only")
    ap.add_argument("--runs", type=int, default=1)
    ap.add_argument("--spread", type=int, default=20)
    ap.add_argument("--no-usb48", action="store_true")
    ap.add_argument("--resume", action="store_true")
    ap.add_argument("--port")
    a = ap.parse_args()
    lab, kinds, nblk = labels()
    n = len(lab)
    host = {}
    if (FM1 / "host.csv").exists():
        for r in csv.DictReader(open(FM1 / "host.csv")):
            host[int(r["idx"])] = r
    todo = parse_only(a.only, n) if a.only else list(range(n))
    spread = set(sorted(host, key=lambda i: -float(host[i]["w8_us"]))[:a.spread]) if host else set()
    out = FM1 / "device.csv"
    done = set()
    if a.resume and out.exists():
        done = {int(r["idx"]) for r in csv.DictReader(open(out))}
    fields = ["idx", "label", "run", "worst_us", "w8_us", "mean_us", "raw_worst_us", "raw_w8_us", "hash",
              "lane_worst_us", "lane_w8_us", "isr_max_us", "sstack_b", "ustack_b", "host_w8_us", "factor_w8",
              "hash_match"]
    new = not (a.resume and out.exists())
    f = open(out, "w" if new else "a", newline="")
    w = csv.DictWriter(f, fields)
    if new:
        w.writeheader()
    d = Dev(a.port)
    st = d.state()
    if not st or st.get(93) != n:
        raise SystemExit(f"fm1_timing: {d.name} is not txfm1 with this table (state {st}, want {n} configurations)")
    print(f"fm1_timing: {d.name}, {n} configurations; run state {st.get(98)} KB, pool free {st.get(97, 0) * 32} B")
    stream = None
    for i in todo:
        if i in done:
            continue
        runs = max(a.runs, 3 if i in spread else 1)
        if kinds[i] == 2 and not a.no_usb48 and stream is None:
            try:
                import sounddevice as sd
                dev = [k for k, x in enumerate(sd.query_devices()) if PORT_RE.search(x["name"]) and x["max_input_channels"] >= 2]
                stream = sd.InputStream(device=dev[0], samplerate=48000, channels=2, blocksize=128, dtype="int16")
                stream.start()
                print("  USB audio open at 48 kHz (the four-lane cases)")
            except Exception as e:      # noqa: BLE001 (reported, the run goes on without the stream)
                print(f"  USB audio not opened: {e}")
                stream = False
        d.state()                       # (resets the interrupt maximum)
        d.cc(20, i >> 7)
        d.cc(21, i & 127)
        d.cc(22, runs)
        limit = time.time() + runs * (nblk[i] + 344) * TPB_MS / 1000 * 1.5 + 10
        isr, s = 0, None
        time.sleep(0.5)
        while time.time() < limit:
            s = d.state()
            if s:
                isr = max(isr, s.get(96, 0))
                if s.get(90) == 2 and s.get(91) == i and s.get(92) == runs:
                    break
            time.sleep(1.0)
        else:
            f.close()
            raise SystemExit(f"fm1_timing: configuration {i} ({lab[i]}) did not finish (last state {s}): stopped")
        for r in range(runs):
            d.cc(23, r)
            it = d.items(99)
            if not it:
                f.close()
                raise SystemExit(f"fm1_timing: no results for configuration {i} run {r}: stopped")
            hsh = it[6] | it[7] << 14 | it[8] << 28
            h = host.get(i)
            row = {"idx": i, "label": lab[i], "run": r, "worst_us": it[1] / 2, "w8_us": it[2] / 2, "mean_us": it[3] / 2,
                   "raw_worst_us": it[4] / 2, "raw_w8_us": it[5] / 2, "hash": f"{hsh:08x}",
                   "lane_worst_us": " ".join(str(it.get(10 + 2 * k, 0) / 2) for k in range(4)) if kinds[i] == 2 else "",
                   "lane_w8_us": " ".join(str(it.get(11 + 2 * k, 0) / 2) for k in range(4)) if kinds[i] == 2 else "",
                   "isr_max_us": isr, "sstack_b": s.get(94), "ustack_b": s.get(95),
                   "host_w8_us": h["w8_us"] if h else "",
                   "factor_w8": f"{it[2] / 2 / float(h['w8_us']):.1f}" if h and float(h["w8_us"]) > 0 else "",
                   "hash_match": (h["hash"] == f"{hsh:08x}") if h else ""}
            w.writerow(row)
            f.flush()
            print(f"  {i:3d} {lab[i][:44]:44s} r{r} worst {row['worst_us']:7.1f} w8 {row['w8_us']:7.1f} "
                  f"mean {row['mean_us']:7.1f} x{row['factor_w8']:>5s} hash {'ok' if row['hash_match'] is True else row['hash_match']}"
                  f" isr {isr} stack {row['sstack_b']}")
    f.close()
    if stream:
        stream.stop()
        stream.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
