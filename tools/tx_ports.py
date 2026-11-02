# SPDX-License-Identifier: GPL-3.0-only
"""Which MIDI port or USB audio device is Textura's FM-1 (tools/fm1_timing.py, tools/hil_jack.py, tools/tx_install.py).

Textura's firmware names itself "Textura FM-1" on USB: the device, its MIDI port and its audio interface (the product
string; tools/fm1_bench.py writes it into the copied usb.c). Firmware built before that read "Frodhes" (Frodhes'
platform files, unchanged), and so does a Frodhes FM-1, which may be on the same computer and is not ours to drive:

  - a name with "Textura" comes first;
  - a "Frodhes" name is taken only when the caller names it (--port Frodhes: a unit still on the old firmware, for
    its first update), and never while a "Textura" name is present;
  - other names (`also`, a regular expression: the bench's own names, the update loader) come after the Textura ones.
"""
import re

TEXTURA = "textura"
OLD = "frodhes"


def pick(names, port=None, also=None):
    """the names to try, best first (an empty list when none may be used)"""
    names = list(names)
    tex = [n for n in names if TEXTURA in n.lower()]
    if port:
        got = [n for n in names if port.lower() in n.lower()]
        got.sort(key=lambda n: TEXTURA not in n.lower())
    else:
        rx = re.compile(also, re.I) if also else None
        got = tex + [n for n in names if n not in tex and rx and rx.search(n) and OLD not in n.lower()]
    if tex:
        got = [n for n in got if OLD not in n.lower()]
    return got


def why_none(names, port=None):
    """the reason pick() gave nothing, for the caller's error"""
    if port and OLD in port.lower() and any(TEXTURA in n.lower() for n in names):
        return "a Textura port is present: a Frodhes port is never used beside it"
    if not port and any(OLD in n.lower() for n in names):
        return ("only a Frodhes port: if it is Textura's FM-1 on its old firmware, name it with --port Frodhes "
                "(a Frodhes FM-1 may be on this computer too)")
    return f"no Textura port among {names}"
