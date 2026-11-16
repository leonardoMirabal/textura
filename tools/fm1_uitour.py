#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""A heavy UI session on the FM-1 with no hands: the panel driven over USB-MIDI through the app's dev hook
(firmware/src/textura.c tx_inj: channel 16 CC 116 = 85 arms it, CC 117 presses / releases a button, CC 118 turns an
encoder) while a preset plays, then the debug items read (tools/fm1_dbg.py): the stacks' deepest use since boot and the
screen's draw time.

  tools/fm1_uitour.py [--port NAME] [--rounds N] [--no-save] [--host-script FILE]   (make fm1-uitour ARGS="...")

The tour, each round: a preset loads (its toast) and plays, notes on channel 1 throughout; every page of every group
walked with SELECT, the four knobs turned there and back; each group button's pages (EDIT, FX, GLO, LFO, ENV, ARP);
the sound browser on every lane (scrolled through the families, HOME: the lane's sound back); the presets list
(scrolled, LOAD OPTIONS opened and shut); PERFORM (FX held) with its knobs; LFO and ENV learn (held + a card turned);
the COLOR knob turned there and back (DISPLAY, in the walk); then the scene save: SAVE, the name's letters turned, SAVE again (from a factory
preset the app takes the first empty user slot: a new user scene stays on the unit; --no-save leaves the save mode with
HOME, nothing written), and the saved scene loaded back through the presets list. Each round ends on the preset
reloaded (what the learn and the knobs changed goes with it).

--host-script FILE writes the same events as a tests/host.c script (midi lines) instead of sending them: the tour run on
the host twin (build/fw/textura-host OUT FILE), to see it walk the pages.

Before: power-cycle the unit (the stacks are painted at boot), install nothing else. Run it under the DEVICE lock
(make fm1-uitour). Needs mido + python-rtmidi (.local/venv).
"""
import argparse
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))   # (python -I: the tools' own helpers)

# the FM-1's buttons and encoders (firmware/src/platform.c, the FM-1's enums)
OCT_DOWN, OCT_UP, FX, SEL, ENV, LFO, EDIT, GLO, HOME, SAVE, ARP, SEQ, PLAY, REC = range(14)
E_SELECT, E_ALGORITHM, E_K1, E_K2, E_K3, E_K4, E_PRESETS = range(7)
ARM = 85
TAP_HOLD_MS = 600                                    # (ui.c: FX / LFO / ENV held)
# the pages SELECT walks from LANDSCAPE (0): SOUND 1-2, LANES 1-4, PERFORM, COMPRESSOR 1-2, DRIVE, FILTER, GRAIN 1-4,
# REVERB 1-3, LFO 1-3, ENV 1-2, MATRIX, GROOVE 1-2, TEMPO (27), SCALE, MIDI (29), MIDI IN (30), DISPLAY (31: COLOR).
# No knobs on TEMPO (K3 starts / stops the transport) nor on the MIDI pages (the channels the tour sends on)
NPAGES = 32
NOKNOBS = (27, 29, 30)


class Tour:
    """the events, in ms from the start: (t, status, d1, d2)"""

    def __init__(self):
        self.t, self.ev, self.armed = 0, [], -10 ** 9

    def send(self, st, d1, d2):
        if st == 0xBF and d1 in (117, 118) and self.t - self.armed > 4000:   # (armed for 10 s; each event renews)
            self.ev.append((self.t, 0xBF, 116, ARM))
            self.t += 5
        if st == 0xBF and d1 in (117, 118):
            self.armed = self.t
        self.ev.append((self.t, st, d1, d2))

    def wait(self, ms):
        self.t += ms

    def tap(self, b, hold=60, after=250):
        self.send(0xBF, 117, b); self.wait(hold); self.send(0xBF, 117, b + 64); self.wait(after)

    def press(self, b):
        self.send(0xBF, 117, b); self.wait(30)

    def release(self, b, after=250):
        self.send(0xBF, 117, b + 64); self.wait(after)

    def turn(self, e, n, after=120):
        while n:
            d = max(-8, min(7, n))
            self.send(0xBF, 118, e * 16 + 8 + d); self.wait(40)
            n -= d
        self.wait(after)

    def note(self, k, on=True):
        self.send(0x90 if on else 0x80, k, 90 if on else 0)

    def knobs(self, n=3):                            # the four knobs there and back
        for e in (E_K1, E_K2, E_K3, E_K4):
            self.turn(e, n, 60); self.turn(e, -n, 60)

    def notes(self, i):                              # a few notes on channel 1 (the selected lane)
        k = 48 + (i * 7) % 24
        self.note(k); self.wait(40); self.note(k, False)


def build(rounds, save):
    t = Tour()
    t.wait(500)
    for r in range(rounds):
        t.send(0xC0, r % 8, 0); t.wait(2500)          # a preset (its toast), playing
        t.tap(HOME)
        for i in range(1, NPAGES):                   # every page: SELECT through all groups, the knobs at each
            t.turn(E_SELECT, 1, 300)
            if i not in NOKNOBS:
                t.knobs(2)
            t.notes(i)
        for i in range(NPAGES):
            t.turn(E_SELECT, -1, 60)
        t.send(0xC0, r % 8, 0); t.wait(2500)          # (the preset again: what the knobs muted or moved, back)
        for b in (EDIT, FX, GLO, LFO, ENV, ARP):      # each group button's own pages (GLO's: no knobs, as above)
            for i in range(6):
                t.tap(b, after=300)
                if b != GLO:
                    t.knobs(1)
        t.tap(HOME)
        t.send(0xC0, r % 8, 0); t.wait(2500)
        for lane in range(4):                        # the sound browser on every lane
            t.tap(SEQ, after=400)
            t.turn(E_ALGORITHM, 12, 300); t.turn(E_SELECT, 3, 300); t.turn(E_SELECT, -3, 300)
            t.turn(E_ALGORITHM, -12, 300)
            t.tap(HOME, after=300)
            t.tap(OCT_UP, after=300)                 # (the next lane)
        t.turn(E_PRESETS, 1, 400)                    # the presets list
        t.turn(E_PRESETS, 10, 300); t.turn(E_SELECT, 2, 300)
        t.tap(FX, after=400); t.tap(FX, after=300)    # LOAD OPTIONS
        t.turn(E_PRESETS, -10, 300); t.tap(HOME, after=400)
        t.press(FX); t.wait(TAP_HOLD_MS + 300)        # PERFORM (FX held)
        t.knobs(4); t.release(FX, 400); t.tap(HOME)
        t.tap(EDIT, after=300)                       # LFO / ENV learn: held, a card turned
        for b in (LFO, ENV):
            t.press(b); t.wait(200); t.turn(E_K1, 2, 200); t.release(b, 400)
        t.tap(HOME)
        t.tap(SAVE, after=500)                       # the scene save
        t.turn(E_K2, 3, 200); t.turn(E_K3, 2, 200); t.turn(E_K3, -2, 200)
        if save:
            t.tap(SAVE, after=3000)                  # (written: the first empty user slot)
            t.turn(E_PRESETS, 1, 300); t.turn(E_PRESETS, -1, 1500)   # (the saved scene loaded back, the list's way)
            t.tap(HOME, after=300)
        else:
            t.tap(HOME, after=500)
        t.send(0xC0, r % 8, 0); t.wait(2500)          # the preset back
    return t.ev


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port")
    ap.add_argument("--rounds", type=int, default=2)
    ap.add_argument("--no-save", action="store_true")
    ap.add_argument("--host-script")
    a = ap.parse_args()
    ev = build(a.rounds, not a.no_save)
    if a.host_script:
        lines = ["# tools/fm1_uitour.py's tour as a host script (generated)", "1700 btn 13", "4700 dump"]
        lines += [f"{1800 + t} midi {st:#04x} {d1} {d2}" for t, st, d1, d2 in ev]
        end = 1800 + ev[-1][0] + 1000
        lines += [f"{end} expect state dropped 0", f"{end + 50} dump", f"{end + 100} end"]
        Path(a.host_script).write_text("\n".join(lines) + "\n")
        print(f"fm1_uitour: {len(ev)} events, {ev[-1][0] / 1000:.0f} s -> {a.host_script}")
        return 0
    import mido
    import tx_ports
    import fm1_dbg
    ins = tx_ports.pick(mido.get_input_names(), a.port)
    outs = tx_ports.pick(mido.get_output_names(), a.port)
    if not ins or not outs:
        raise SystemExit(f"fm1_uitour: no Textura port: {tx_ports.why_none(mido.get_input_names(), a.port)}")
    i, o = mido.open_input(ins[0]), mido.open_output(outs[0])
    before = fm1_dbg.ask(i, o)
    if not before:
        raise SystemExit("fm1_uitour: no debug answer (not the Textura app?)")
    print(f"{ins[0]}: before the tour: user stack {before.get(43, 0) * 4} B, system {before.get(44, 0) * 4} B, "
          f"core 1 {before.get(41, 0)} B")
    print(f"fm1_uitour: {len(ev)} events, {ev[-1][0] / 1000:.0f} s{'' if not a.no_save else ' (no save)'}", flush=True)
    t0 = time.time()
    for t, st, d1, d2 in ev:
        while time.time() - t0 < t / 1000:
            time.sleep(0.002)
        b = [st, d1] if st & 0xF0 == 0xC0 else [st, d1, d2]
        o.send(mido.Message.from_bytes(b))
    time.sleep(1.0)
    it = fm1_dbg.ask(i, o) or {}
    print(f"after the tour: user stack {it.get(43, 0) * 4} B (deepest since boot), system {it.get(44, 0) * 4} B, "
          f"core 1 {it.get(41, 0)} B")
    print(f"  screen: parts drawn {it.get(58, 0)}, mean {it.get(57, 0)} us, longest {it.get(56, 0)} us "
          f"(since the first ask); audio interrupt longest {it.get(24, 0)} us; core 1 bad reads {it.get(31, 0)}, "
          f"jobs lost {it.get(34, 0)}; crash count {it.get(53, 0)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
