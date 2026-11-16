#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""The device-only platform paths no host build runs (make fw-test), checked in their source: what each guards
against is said with its check. Exit 1 on a failed check."""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
fails = 0


def check(ok, what):
    global fails
    print(("ok    " if ok else "FAIL  ") + what)
    fails += not ok


def src(name):
    return (ROOT / "firmware" / "src" / name).read_text(encoding="utf-8")


def body(text, head):
    """a function's body: from its head's line to the first column-0 closing brace"""
    m = re.search(r"^" + re.escape(head) + r"\n\{(.*?)^\}\n", text, re.S | re.M)
    return m.group(1) if m else ""


# USB RESCUE (recovery.c) drops the MIDI ring every step: nothing plays there, and a full ring makes usb.c hold EP1 OUT
# (NAK, ep1_take: fewer than EP1_ROOM slots free), so the installer's SysEx never arrives (Frodhes 1004e7bdc)
step = body(src("recovery.c"), "static void recovery_step(void)")
check(bool(re.search(r"while \(midi_get\(&st, &d1, &d2\)\)\s*(/\*.*?\*/\s*)?;", step, re.S)),
      "recovery: recovery_step drains the MIDI ring (a full ring held EP1 OUT: USB RESCUE took no installer)")
check(bool(re.search(r"if \(MQ - \(mi_w - mi_r\) < EP1_ROOM\)\n\s+return 0;", src("usb.c"))),
      "recovery: ... the ring's back-pressure on EP1 (what the drain is for) still there")

sys.exit(1 if fails else 0)
