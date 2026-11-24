#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""The audio's CPU on the FM-1, per OUTPUT mode: the Textura app's debug reply (firmware/src/textura.c tx_dbg_reply,
items 59-72; tools/fm1_dbg.py) read over USB-MIDI after a window in each mode. Installs nothing; it changes OUTPUT and
its AMOUNT (NRPN on channel 5: 99 = 4, 98 = 45 OUTPUT 0 / 127, 46 AMOUNT 0..127) and puts OUTPUT back to SPEAKERS at
the end. Play the scene to measure first (a busy 4-lane preset).

  tools/hil_cpu.py [--port NAME] [--sec 10] [--modes spk,hp100,hp0,hp100] [--jack] [--json]

Each mode: set, settle 1 s, ask (the "since the last ask" items restart), wait --sec, ask again: the window's audio
interrupt (mean, longest; those over TX_GOV_ISR_TARGET and over a block: a late block, the jack repeats a half), the
pan on core 0 (mean, longest), core 0's render, core 1's job and its mix end (longest), the binaural pans the
governor's pool carries, the blocks core 1 panned lanes in. --jack also records the jack (MOTU M6 inputs 5 / 6 at
48 kHz, sounddevice) over the window and counts clicks (second differences 12x over their local mean). An app before
items 59-72 (0.3.0) answers with its longest interrupt alone (item 24): the numbers before a change. Exit 3 when a
mode had a late block (or an interrupt longer than a block), 2 on no answer. Needs mido + python-rtmidi (+ numpy, sounddevice for --jack): .local/venv.
"""
import argparse
import json
import sys
import time
from pathlib import Path

import mido

sys.path.insert(0, str(Path(__file__).resolve().parent))   # (python -I: the tools' own helpers)
import fm1_dbg  # noqa: E402
import tx_ports  # noqa: E402

MODES = {"spk": (0, None), "hp100": (127, 127), "hp50": (127, 64), "hp0": (127, 0)}   # OUTPUT, AMOUNT (None: kept)
BLOCK_US = 5805
SR = 48000


def nrpn(o, idx, v, ch=4):
    for n, x in ((99, 4), (98, idx), (6, v)):
        o.send(mido.Message("control_change", channel=ch, control=n, value=x))
    time.sleep(0.05)


def clicks(x):
    """the jack's clicks per channel: |second difference| 12x over its local mean (2048 samples) and over 0.02"""
    import numpy as np
    res = []
    for c in range(x.shape[1]):
        d2 = np.abs(np.diff(x[:, c], 2))
        k = np.convolve(d2, np.ones(2048) / 2048, mode="same") + 1e-6
        res.append(int(np.sum((d2 > 12 * k) & (d2 > 0.02))))
    return res


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port")
    ap.add_argument("--sec", type=float, default=10.0)
    ap.add_argument("--modes", default="spk,hp100,hp0,hp100")
    ap.add_argument("--jack", action="store_true")
    ap.add_argument("--json", action="store_true")
    a = ap.parse_args()
    modes = [m for m in a.modes.split(",") if m]
    bad = [m for m in modes if m not in MODES]
    if bad:
        raise SystemExit(f"hil_cpu: unknown mode {bad[0]} (spk, hp100, hp50, hp0)")
    ins = tx_ports.pick(mido.get_input_names(), a.port)
    outs = tx_ports.pick(mido.get_output_names(), a.port)
    if not ins or not outs:
        raise SystemExit(f"hil_cpu: no Textura port: {tx_ports.why_none(mido.get_input_names(), a.port)}")
    i, o = mido.open_input(ins[0]), mido.open_output(outs[0])
    rows, late = [], False
    try:
        for m in modes:
            out, amt = MODES[m]
            if amt is not None:
                nrpn(o, 46, amt)
            nrpn(o, 45, out)
            time.sleep(1.0)
            first = fm1_dbg.ask(i, o)
            if not first:
                print("hil_cpu: no answer (an app without the debug reply, or not the Textura app)")
                return 2
            rec = None
            if a.jack:
                import sounddevice as sd
                rec = sd.rec(int(a.sec * SR), samplerate=SR, channels=6, device="M6", dtype="float32")
            time.sleep(a.sec)
            if rec is not None:
                sd.wait()
            it = fm1_dbg.ask(i, o) or {}
            if 24 not in it:
                print(f"hil_cpu: {m}: no answer after the window")
                return 2
            if 72 not in it:                         # (an app before these items: its longest interrupt alone)
                print(f"hil_cpu: {m}: the reply ended before item 72 (an older app: the longest interrupt only)")
            r = {"mode": m, "isr_mean": it.get(64), "isr_max": it[24], "over_target": it.get(65), "late": it.get(66),
                 "blocks": it.get(67), "pan0_mean": it.get(60), "pan0_max": it.get(59), "c0_render_max": it.get(61),
                 "c1_job_max": it.get(62), "c1_mixend_max": it.get(63), "charge": it.get(68), "output": it.get(69),
                 "amount": it.get(70), "c1_pan_blocks": (it[72] - first.get(72, 0)) % 16384 if 72 in it else None,
                 "c1_state": it.get(30), "c1_lost": it.get(34), "c1_wait_max": it.get(35), "pool": it.get(42)}
            if rec is not None:
                r["jack_clicks"] = clicks(rec[int(0.3 * SR):, 4:6].astype("float64"))
            late |= (r["late"] or 0) > 0 or r["isr_max"] > BLOCK_US
            rows.append(r)
    finally:
        nrpn(o, 45, 0)                               # (OUTPUT back to SPEAKERS)
    if a.json:
        print(json.dumps(rows))
    else:
        print(f"{ins[0]}: {a.sec:.0f} s a mode, audio interrupt us (a block {BLOCK_US})")
        print("  mode    isr mean  max  >target  late/blocks  pan0 mean  max  c0 render  c1 job  c1 end  charge"
              "  c1 pans" + ("  jack clicks L/R" if a.jack else ""))
        for r in rows:
            v = {k: ("-" if x is None else x) for k, x in r.items()}
            print(f"  {v['mode']:6s}  {v['isr_mean']:>8} {v['isr_max']:>4}  {v['over_target']:>7}  {v['late']:>4}/{v['blocks']!s:<6}"
                  f"  {v['pan0_mean']:>9} {v['pan0_max']:>4}  {v['c0_render_max']:>9}  {v['c1_job_max']:>6}"
                  f"  {v['c1_mixend_max']:>6}  {v['charge']:>6}  {v['c1_pan_blocks']:>7}"
                  + (f"  {v['jack_clicks'][0]}/{v['jack_clicks'][1]}" if a.jack else ""))
        print(f"  core 1 state {rows[-1]['c1_state']} (1 running), jobs lost {rows[-1]['c1_lost']}, "
              f"longest wait for it {rows[-1]['c1_wait_max']} us, governor pool {rows[-1]['pool']} us")
    return 3 if late else 0


if __name__ == "__main__":
    sys.exit(main())
