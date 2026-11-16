#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Read the Textura app's debug items on the FM-1 over USB-MIDI (firmware/src/textura.c tx_dbg_reply): CC 116 on
channel 16 asks, the app answers CC 100 id / 101 - 102 value (14 bits) on channel 1. Installs and changes nothing on
the unit: the ask only restarts the held "since the last ask" items (21, 23, 24, 35, 56-58).

  tools/fm1_dbg.py [--port NAME] [--watch S] [--json]      (make fm1-dbg ARGS="...": under the DEVICE lock)

--watch S: ask again every S seconds and print the stacks' deepest and the UI draw's longest so far, and each
interval's mean draw (^C stops; items 56-58: each part of the screen ui_draw draws, timed less an audio interrupt).
The stacks (43, 44, 41) are the deepest use since boot: the stacks are painted at boot and the reply counts the
words never written, so the number only grows until the next reset. Needs mido + python-rtmidi (.local/venv).
"""
import argparse
import json
import sys
import time
from pathlib import Path

import mido

sys.path.insert(0, str(Path(__file__).resolve().parent))   # (python -I: the tools' own helper)
import tx_ports  # noqa: E402

LAST = 58                                            # the reply's last item
ITEMS = {
    20: "grain cap now", 21: "grain cap lowest", 22: "grain room now (us)", 23: "grain room least (us)",
    24: "audio interrupt longest (us)", 25: "reverb (us, held)", 26: "lanes before the bus (us, held)",
    27: "rest (us, held)", 30: "core 1 state", 31: "core 1 bad reads", 32: "core 1 blocks (lo)", 33: "core 1 blocks (hi)",
    34: "core 1 jobs lost", 35: "core 1 longest wait (us)", 36: "VDC14 at boot", 37: "SYSVDD at boot", 38: "VDC14 now",
    39: "SYSVDD now", 40: "core 1 lanes (mask)", 41: "core 1 stack deepest (B)", 42: "governor pool (us)",
    43: "user stack deepest (words)", 44: "system stack deepest (words)", 45: "core 1 recoveries", 46: "core 0 recoveries",
    47: "core 1 jobs refused", 48: "late jobs", 49: "holds for good", 50: "retries", 51: "SYS_DIV (lo 14 bits)",
    52: "C1_HSB2 wrote SYS_DIV", 53: "crash count", 54: "crash vector", 55: "reset reason (P33)",
    56: "UI part drawn longest (us)", 57: "UI part drawn mean (us)", 58: "UI parts drawn",
}


def ask(i, o, timeout=2.0):
    """one reply: {id: value}, or None (no answer)"""
    while i.poll() is not None:                       # (stale events)
        pass
    o.send(mido.Message("control_change", channel=15, control=116, value=0))
    got, cur, msb, t0 = {}, None, 0, time.time()
    while time.time() - t0 < timeout:
        m = i.poll()
        if m is None:
            time.sleep(0.002)
            continue
        if m.type != "control_change" or m.channel != 0:
            continue
        if m.control == 100:
            cur = m.value
        elif m.control == 101:
            msb = m.value
        elif m.control == 102 and cur is not None:
            got[cur] = msb << 7 | m.value
            if cur == LAST:
                return got
    return got or None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port")
    ap.add_argument("--watch", type=float)
    ap.add_argument("--json", action="store_true")
    a = ap.parse_args()
    ins = tx_ports.pick(mido.get_input_names(), a.port)
    outs = tx_ports.pick(mido.get_output_names(), a.port)
    if not ins or not outs:
        raise SystemExit(f"fm1_dbg: no Textura port: {tx_ports.why_none(mido.get_input_names(), a.port)}")
    i, o = mido.open_input(ins[0]), mido.open_output(outs[0])
    it = ask(i, o)
    if not it:
        raise SystemExit("fm1_dbg: no answer (an app built without LN_BUS_TIMED, or not the Textura app)")
    if a.json:
        print(json.dumps(it, sort_keys=True))
    else:
        print(f"{ins[0]}:")
        for k in sorted(it):
            v = it[k]
            extra = f"  = {v * 4} B" if k in (43, 44) else ""
            print(f"  {k:3d} {ITEMS.get(k, '?'):42s} {v}{extra}")
        if LAST not in it:
            print(f"  (the reply ended before item {LAST}: an older app, or events lost)")
    if a.watch:
        u, s, c, d = it.get(43, 0), it.get(44, 0), it.get(41, 0), it.get(56, 0)
        try:
            while True:
                time.sleep(a.watch)
                n = ask(i, o) or {}
                u, s, c = max(u, n.get(43, 0)), max(s, n.get(44, 0)), max(c, n.get(41, 0))
                d = max(d, n.get(56, 0))
                print(f"{time.strftime('%H:%M:%S')}  user stack {u * 4} B  system stack {s * 4} B  core 1 stack {c} B"
                      f"  UI draw longest {d} us, this interval mean {n.get(57, 0)} us over {n.get(58, 0)} parts",
                      flush=True)
        except KeyboardInterrupt:
            pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
