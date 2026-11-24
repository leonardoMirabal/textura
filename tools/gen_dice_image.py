#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""The stereo image after a roll (firmware/src/dice.c): the host scripts that roll many times and judge each result.

  tests/scripts/diceimage.txt           the FM-1's four lanes (make fw-test runs it with every host script)
  plugin/tests/scripts/diceimage16.txt  the plugin's sixteen (tools/plugin_gate.py, on the 16-lane host build)

Each roll starts from a factory preset (every 7th of them in turn), then OCT- and OCT+ together roll everything (the
host build's dice run on from roll to roll: every roll differs). On sixteen lanes, lane 16 is given a sound first
(LANES 16): the roll fills lanes 5 .. 15 and places them. Every other roll the kept lanes' PAN is set to the centre
first, so what the roll set alone decides the left and the right. 8 s after the roll's first 1.5 s, "expect image"
(tests/host.c) judges the output: the middle at least 3 dB over the sides, the L / R correlation at least 0.3 (what
MONO keeps: at least ~1.8 dB short of all of it), and, the kept lanes centred, the placement and the left and right.
The placement is what the roll decides: the sum of the playing lanes' PAN - 64 within +- PS (four lanes: 16, a lone
lane filled sits at most 16 off the centre; sixteen: 48, eleven lanes filled, ~3 each). The left and the right then
within 6 dB: their gap also follows the sounds' own levels, which a roll does not set (a sparse filled lane can sit
25 dB under its neighbour: a placement summing to 0 then still leans ~4 dB); 6 dB catches a placement that leans, not
a quiet sound. Each build plays to its own OUTPUT: the FM-1's SPEAKERS, the plugin's HEADPHONES (binaural: the far ear late only in the lows, so it holds in mono too).

  gen_dice_image.py           write both
  gen_dice_image.py --check   fail (exit 1) if either is not what this writes (make fw-test)"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PERIOD, WIN = 11000, 8000
with open(os.path.join(ROOT, "firmware/src/presets.h")) as _f:   # (the factory presets: every 7th in turn)
    PRESETS = int(re.search(r"#define PR_N (\d+)u", _f.read()).group(1))


def script(lanes, n, bal, ps):
    head = ("# the stereo image after a roll, %d lanes, %d rolls: written by tools/gen_dice_image.py (see there), do not\n"
            "# edit. Each: a factory preset,%s OCT- and OCT+ together; every other roll the kept\n"
            "# lanes' PAN at the centre first; then the image of the 8 s after the roll's first 1.5 s: M - S >= 3 dB,\n"
            "# correlation >= 0.3, the kept lanes centred |L - R| <= %d dB and the PANs' sum off the centre <= %d\n"
            % (lanes, n, " lane 16 given a sound (LANES 16)," if lanes > 4 else "", bal, ps))
    out = []
    t = 1500
    for i in range(n):
        out.append("%d midi 0xC0 %d 0" % (t, (i * 7) % PRESETS))
        if lanes > 4:
            out.append("%d midi 0xBF 20 %d" % (t + 600, (i * 13) % 120))
        centred = i % 2 == 1
        if centred:
            for ch in [0, 1, 2, 3] + ([15] if lanes > 4 else []):
                out.append("%d midi 0x%X 10 64" % (t + 700, 0xB0 + ch))
        out.append("%d btn 0" % (t + 1500))
        out.append("%d btn 1" % (t + 1500))
        out.append("%d expect image 3 0.3 %d %d%s" % (t + PERIOD - 1, bal if centred else 99, WIN,
                                                       " %d" % ps if centred else ""))
        t += PERIOD
    out.append("%d end" % t)
    return head + "\n".join(out) + "\n"


FILES = [("tests/scripts/diceimage.txt", 4, 200, 6, 16), ("plugin/tests/scripts/diceimage16.txt", 16, 120, 6, 48)]


def main():
    check = "--check" in sys.argv[1:]
    bad = 0
    for path, lanes, n, bal, ps in FILES:
        want, full = script(lanes, n, bal, ps), os.path.join(ROOT, path)
        if check:
            try:
                with open(full) as f:
                    have = f.read()
            except OSError:
                have = None
            if have != want:
                print("%s: not what tools/gen_dice_image.py writes" % path)
                bad = 1
        else:
            with open(full, "w") as f:
                f.write(want)
    return bad


if __name__ == "__main__":
    sys.exit(main())
