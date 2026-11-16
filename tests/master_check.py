#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""The MASTER pass after the app (make fw-test): the audio ISR's own code (platform.c fm1_alnk0_irq, from
`m = master_q12;` to its `#undef TAIL_PRE`, and master_ramp when it is there) cut out of the source and run on the host
over a steady 2 kHz tone while MASTER moves as the main loop moves it (once a block at most): one detent (24 / 1023
of the knob: k 500 -> 524, master_q12 976 -> 1072), a spin (8 detents, one a block) up and down, at low, mid and full
volume, USB before MASTER on and off. Per case the largest |second difference| of the jack's samples against the same
tone at the run's top gain held: a gain that ramps stays at or under 1 (one LSB of rounding over that at low volume),
a gain that steps once a block adds |dg x| at once (Frodhes 0.9.6, BUGHUNT #20). One gain a block reads 1.2 .. 4.1,
master_ramp 1.00 .. 1.04 (an LSB of rounding allowed: x / (held + 1)). Over 1.5 fails.
Exit 1 on a failed check."""
import os
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "build" / "master_test"
CC = os.environ.get("CC", "cc")
LIMIT = 1.5
fails = 0


def check(ok, what):
    global fails
    print(("ok    " if ok else "FAIL  ") + what)
    fails += not ok


src = (ROOT / "firmware" / "src" / "platform.c").read_text(encoding="utf-8")
irq = src[src.index("void fm1_alnk0_irq(void)"):]
a = irq.index("        m = master_q12;\n")
z = irq.index("#undef TAIL_PRE\n", a) + len("#undef TAIL_PRE\n")
section = irq[a:z]
ramp = ""
if "static __attribute__((noinline)) void master_ramp(" in src:
    r0 = src.index("static int32_t master_prev")
    r0 = src.rindex("\n/*", 0, r0) + 1
    r1 = src.index("#undef TAIL_PRE\n", r0) + len("#undef TAIL_PRE\n")
    ramp = src[r0:r1].replace("__attribute__((noinline)) ", "")

HARNESS = r"""
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#define HALF_FRAMES 256u
#define HALF_WORDS (HALF_FRAMES * 2u)
#define APP_OUT_SHIFT 7u
#define HAL_KEEP_REG(v) ((void)(v))
#define UAC_ONLY(x) ((void)0)
static int32_t tap_q[64];
static uint8_t usb_pre_master;
static volatile int32_t master_q12;
@RAMP@
static void pass(int32_t *o)
{
    uint32_t i, b;
    int32_t m;
@SECTION@
    (void)i;
}
/* argv: PRE AMP then the gain a block (master_q12 values); prints the jack's largest |second difference| */
int main(int argc, char **argv)
{
    static int32_t o[HALF_WORDS];
    double ph = 0, d2 = 0;
    int32_t y1 = 0, y2 = 0, n = 0, amp;
    int k;
    usb_pre_master = (uint8_t)atoi(argv[1]); amp = atoi(argv[2]);
    for (k = 3; k < argc; k++) {
        uint32_t j;
        master_q12 = atoi(argv[k]);
        for (j = 0; j < HALF_FRAMES; j++) {
            o[2 * j] = o[2 * j + 1] = (int32_t)lrint(amp * sin(ph));
            ph += 6.283185307179586 * 2000.0 / 44100.0;
        }
        pass(o);
        for (j = 0; j < HALF_FRAMES; j++, n++) {
            int32_t y = o[2 * j];
            if (n >= 2 && k > 3) { double e = fabs((double)y - 2.0 * y1 + y2); if (e > d2) d2 = e; }
            y2 = y1; y1 = y;
        }
    }
    printf("%.1f\n", d2 / (1 << APP_OUT_SHIFT));   /* (in the 16-bit LSB) */
    return 0;
}
"""
OUT.mkdir(parents=True, exist_ok=True)
c = OUT / "master_test.c"
c.write_text(HARNESS.replace("@RAMP@", ramp).replace("@SECTION@", section), encoding="utf-8")
exe = OUT / "master_test"
r = subprocess.run([CC, "-O1", "-w", "-o", str(exe), str(c), "-lm"], capture_output=True, text=True)
check(r.returncode == 0, "the ISR's MASTER pass (platform.c) builds on the host" + ("" if not r.returncode else ": " + r.stderr[-800:]))
if r.returncode:
    sys.exit(1)
check(bool(ramp), "platform.c has master_ramp (MASTER ramps over the block when it moved)")


def q12(k):
    return (k * k) >> 8


def run(pre, amp, gains):
    out = subprocess.run([str(exe), str(pre), str(amp)] + [str(g) for g in gains], capture_output=True, text=True,
                         check=True).stdout
    return float(out)


worst = {}
for lvl, k0 in (("low", 150), ("mid", 500), ("full", 1000)):
    for case, ks in (("detent", [k0] * 8 + [k0 + 24] * 8 + [k0] * 8),
                     ("spin", [k0] * 8 + [k0 + 24 * i for i in range(1, 9)] + [k0 + 24 * (8 - i) for i in range(1, 9)]
                      + [k0] * 4)):
        ks = [min(k, 1023) for k in ks]
        for pre in (0, 1):
            g = [q12(k) for k in ks]
            top = max(g)
            got = run(pre, 30000, g)
            ref = run(pre, 30000, [top] * len(g))
            ratio = got / (ref + 1.0)   # (one LSB of rounding allowed: at low volume d2 is a few LSB)
            worst[(lvl, case, pre)] = ratio
check(max(worst.values()) <= LIMIT,
      "MASTER moved under a steady tone: the jack's largest second difference re the top gain held, at most %.2f "
      "(%s; %.1f at most)" % (max(worst.values()),
                              ", ".join("%s %s%s %.2f" % (l, c_, " pre" if p else "", v)
                                        for (l, c_, p), v in sorted(worst.items())), LIMIT))
sys.exit(1 if fails else 0)
