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

sys.path.insert(0, str(Path(__file__).resolve().parent))   # (python -I: the tools' own helper)
import tx_ports  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
FM1 = ROOT / "build" / "fm1"
ALSO = r"fm-1|txfm1|felucca|dinghy"                  # after "Textura" (tx_ports: "Frodhes" only by --port)
TPB_MS = 256 * 1000 / 44100
PAR = []                                         # (labels(): the LANES rows on two cores)


def labels():
    txt = (FM1 / "gen" / "fm1_cfg.h").read_text()
    kinds = [int(m.group(1)) for m in re.finditer(r"^  \{(\d+),\d+,\d+,\d+,\d+,\d+,(\d+),", txt, re.M)]
    nblk = [int(m.group(2)) for m in re.finditer(r"^  \{(\d+),\d+,\d+,\d+,\d+,\d+,(\d+),", txt, re.M)]
    lab = re.findall(r'^  "(.*)",$', txt, re.M)
    global PAR
    PAR = [(int(m.group(1)) & 32) != 0 for m in re.finditer(r"^  \{\d+,\d+,(\d+),", txt, re.M)]   # LANES FM_PAR
    return lab, kinds, nblk


class Dev:
    def __init__(self, name):
        ins = tx_ports.pick(mido.get_input_names(), name, ALSO)
        outs = tx_ports.pick(mido.get_output_names(), name, ALSO)
        if not ins or not outs:
            raise SystemExit(f"fm1_timing: no FM-1 port: {tx_ports.why_none(mido.get_input_names(), name)}")
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


C1_STATES = ["off", "on", "held: refusals too often", "held: late job", "held: no answer", "off: the supply not raised"]


def c1_state(st):
    """txfm1's core-1 items (CC 24: 80..89, 36..39, 58, 59)"""
    if not st or 80 not in st:
        return "no core-1 items"
    k = st[80]
    return (f"{C1_STATES[k] if k < len(C1_STATES) else k}, bad reads {st.get(81)}, jobs {st.get(82, 0) | st.get(83, 0) << 14}"
            f", wait max {st.get(84)} us, last bad job {st.get(85)}, stack {st.get(86)} B; recovered core 1 {st.get(87)}"
            f" core 0 {st.get(88)}, refused {st.get(89)}, late {st.get(36)}, holds {st.get(37)}, retries {st.get(38)}"
            f"; SYS_DIV {st.get(39, 0):#x}{' (C1_HSB2 wrote it)' if st.get(58) else ''}, reset reason {st.get(59)}")


def crash(st):
    """the last crash record (CC 24: 60..78; it outlives the reset)"""
    if not st or not st.get(60):
        return "no crash record"
    v = [st.get(63 + 3 * k, 0) | st.get(64 + 3 * k, 0) << 14 | st.get(65 + 3 * k, 0) << 28 for k in range(5)]
    x = [st.get(40 + 3 * k, 0) | st.get(41 + 3 * k, 0) << 14 | st.get(42 + 3 * k, 0) << 28 for k in range(6)]
    return (f"crash record: count {st.get(61)}, vector {st.get(62)}, pc {v[0]:08x}, rets {v[1]:08x}, sp {v[2]:08x}, "
            f"DBG_MSG {v[3]:08x}, EMU_MSG {v[4]:08x}, ICFG {x[0]:08x}, PSR {x[1]:08x}, at {st.get(78, 0) / 10:.1f} s "
            f"after boot; core 1 started with sp {x[2]:08x} usp {x[3]:08x} ssp {x[4]:08x} psr {x[5]:08x}"
            f"{'; core 1 faulted (stopped)' if st.get(79) else ''}")


FRAME = [f"r{k}" for k in range(16)] + ["reti", "rete", "retx", "rets", "psr", "icfg", "usp", "ssp", "sp", "rets0"]


def frame(d):
    """core 1's fault frame (CC 25)"""
    d.cc(25, 0)
    it = d.items(99)
    if not it:
        return "core 1 frame: no answer"
    v = [it.get(1 + 3 * k, 0) | it.get(2 + 3 * k, 0) << 14 | it.get(3 + 3 * k, 0) << 28 for k in range(26)]
    if not it.get(90):
        return "core 1 frame: none for the last crash record"
    return "core 1 frame: " + " ".join(f"{n} {x:08x}" for n, x in zip(FRAME, v))


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
    ap.add_argument("--soft-reset", action="store_true", help="reset the FM-1 first (CC 26: core 1 held, then a chip "
                    "reset) and wait for it to answer again")
    a = ap.parse_args()
    lab, kinds, nblk = labels()
    n = len(lab)
    host = {}
    if (FM1 / "host.csv").exists():
        for r in csv.DictReader(open(FM1 / "host.csv")):
            host[int(r["idx"])] = r
    todo = parse_only(a.only, n) if a.only else list(range(n))
    spread = set(sorted((i for i in host if i < n and kinds[i] != 3), key=lambda i: -float(host[i]["w8_us"]))[:a.spread])
    out = FM1 / "device.csv"
    done = set()
    if a.resume and out.exists():
        done = {int(r["idx"]) for r in csv.DictReader(open(out))}
    fields = ["idx", "label", "run", "worst_us", "w8_us", "mean_us", "raw_worst_us", "raw_w8_us", "hash",
              "lane_worst_us", "lane_w8_us", "isr_max_us", "sstack_b", "ustack_b", "host_w8_us", "factor_w8",
              "hash_match", "c1"]
    new = not (a.resume and out.exists())
    f = open(out, "w" if new else "a", newline="")
    w = csv.DictWriter(f, fields)
    if new:
        w.writeheader()
    d = Dev(a.port)
    if a.soft_reset:
        d.cc(26, 0x55)
        d.i.close()
        d.o.close()
        t0 = time.time()
        while True:
            time.sleep(2.0)
            try:
                d = Dev(a.port)
                if d.state():
                    break
                d.i.close()
                d.o.close()
            except (SystemExit, OSError, IOError):
                pass
            if time.time() - t0 > 60:
                raise SystemExit("fm1_timing: the FM-1 did not come back within 60 s of the soft reset")
        print(f"fm1_timing: soft reset done ({time.time() - t0:.0f} s)")
    st = d.state()
    if not st or st.get(93) != n:
        raise SystemExit(f"fm1_timing: {d.name} is not txfm1 with this table (state {st}, want {n} configurations)")
    print(f"fm1_timing: {d.name}, {n} configurations; run state {st.get(98)} KB, pool free {st.get(97, 0) * 32} B")
    print(f"  core 1: {c1_state(st)}")
    if st and 30 in st:
        print(f"  supply: VDC14 {st.get(32)} ({1.25 + 0.05 * st.get(32, 0):.2f} V), SYSVDD {st.get(33)} "
              f"({0.93 + 0.03 * st.get(33, 0):.2f} V); at boot {st.get(30)} / {st.get(31)}; this image "
              f"{'raises them' if st.get(35) else 'reads only'}; SYS PLL multiplier {st.get(34)} "
              f"({st.get(34, 0) * 4 // 3} MHz)")
    print(f"  {crash(st)}")
    print(f"  {frame(d)}")
    stream = None
    for i in todo:
        if i in done:
            continue
        runs = max(a.runs, 3 if i in spread else 1)
        if kinds[i] == 2 and not a.no_usb48 and stream is None:
            try:
                import sounddevice as sd
                devs = sd.query_devices()
                ok = tx_ports.pick([x["name"] for x in devs if x["max_input_channels"] >= 2], a.port, ALSO)
                dev = [k for n in ok[:1] for k, x in enumerate(devs) if x["name"] == n and x["max_input_channels"] >= 2]
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
            if s and s.get(91) != i:    # the configuration is gone: the FM-1 restarted (a crash?)
                f.close()
                raise SystemExit(f"fm1_timing: configuration {i} ({lab[i]}): the FM-1 restarted (state {s})\n"
                                 f"  {crash(s)}\n  core 1: {c1_state(s)}\n  {frame(d)}")
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
                   "hash_match": (h["hash"] == f"{hsh:08x}") if h else "", "c1": ""}
            if kinds[i] == 3:           # FM_K_C1: core 1 against core 0, each block
                v = [it.get(20 + 2 * k, 0) | it.get(21 + 2 * k, 0) << 14 for k in range(4)]
                row["c1"] = f"jobs {v[0]} differ {v[1]} first {v[2]} solo {v[3]}; {c1_state(s)}"
            elif kinds[i] == 2 and PAR[i]:     # LANES on two cores: core 1's blocks, jobs lost, first loss, core 0 alone
                v = [it.get(20 + 2 * k, 0) | it.get(21 + 2 * k, 0) << 14 for k in range(4)]
                row["c1"] = (f"two cores {v[0]} lost {v[1]} at {v[2]} one core {v[3]}; busy worst core 0 "
                             f"{it.get(30, 0) / 2} core 1 {it.get(31, 0) / 2}, mean {it.get(32, 0) / 2} / "
                             f"{it.get(33, 0) / 2} us; {c1_state(s)}")
            w.writerow(row)
            f.flush()
            print(f"  {i:3d} {lab[i][:44]:44s} r{r} worst {row['worst_us']:7.1f} w8 {row['w8_us']:7.1f} "
                  f"mean {row['mean_us']:7.1f} x{row['factor_w8']:>5s} hash {'ok' if row['hash_match'] is True else row['hash_match']}"
                  f" isr {isr} stack {row['sstack_b']}")
            if row["c1"]:
                print(f"      {row['c1']}")
                if s.get(79):
                    print(f"      {crash(s)}")
                    print(f"      {frame(d)}")
    f.close()
    if stream:
        stream.stop()
        stream.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
