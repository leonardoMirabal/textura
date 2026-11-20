#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""The desktop plugin's card hints (plugin/card_hints.h) from the manual's section 5 (docs/MANUAL.md: "## 5. The
sounds"): each family's table names its sounds' engines, each engine's table its cards and what they do. The window's
drawer shows a lane's cards with these lines (gui_core.c card_hint), and the sound browser a sound's engine's first
sentence (gui_core.c snd_desc).

    python3 tools/plugin_card_hints.py            write plugin/card_hints.h
    python3 tools/plugin_card_hints.py --check    exit 1 if plugin/card_hints.h is not what the manual gives
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MANUAL = os.path.join(ROOT, "docs", "MANUAL.md")
OUT = os.path.join(ROOT, "plugin", "card_hints.h")


def cstr(s):
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def parse():
    text = open(MANUAL, encoding="utf-8").read()
    a = text.index("\n## 5. The sounds")
    b = text.index("\n## 6.", a)
    sec = text[a:b].splitlines()
    snd, cards, desc, eng, in_snd, want = [], [], [], None, False, False
    for ln in sec:
        if ln.startswith("#### "):
            eng, in_snd, want = ln[5:].strip(), False, True
            continue
        if want and ln.strip():          # the engine's first paragraph: its first sentence
            want = False
            if not ln.startswith(("|", "!", "#")) and eng not in [e for e, _ in desc]:
                m = re.match(r"^(.+?[.!?])(\s|$)", ln.strip())
                desc.append((eng, (m.group(1) if m else ln.strip()).replace("**", "")))
        if ln.startswith("### "):
            eng, in_snd = None, False
            continue
        if re.match(r"^\| *Sound *\| *Engine *\|", ln):
            in_snd = True
            continue
        cells = [c.strip() for c in ln.strip().strip("|").split("|")] if ln.startswith("|") else None
        if not cells or len(cells) < 2 or set(cells[0]) <= set("- "):
            if not ln.startswith("|"):
                in_snd = False
            continue
        if in_snd and eng is None:
            snd.append((cells[0].lower(), cells[1]))
            continue
        m = re.match(r"^\*\*(.+?)\*\*$", cells[0])
        if eng and m:
            hint = cells[1]
            names = {s for s, e in snd if e == eng}
            p = re.search(r"\s*\(([^()]*)\)$", hint)   # a trailing list of the engine's sounds: not a hint's part
            if p and all(x.strip().lower() in names for x in p.group(1).split(",")):
                hint = hint[: p.start()]
            cards.append((eng, m.group(1), hint))
    return snd, cards, desc


def render(snd, cards, desc):
    o = ["/* SPDX-License-Identifier: GPL-3.0-only */",
         "/* The cards' hints for the plugin window's drawer (gui_core.c card_hint): generated from docs/MANUAL.md",
         " * section 5 by tools/plugin_card_hints.py; do not edit (make test-plugin checks it is current). The engines'",
         " * first sentences: the sound browser's line for a sound (gui_core.c snd_desc). */",
         "#ifndef TXP_CARD_HINTS_H", "#define TXP_CARD_HINTS_H",
         "static const struct { const char *snd, *eng; } TXH_SND[] = {"]
    o += ["    { %s, %s }," % (cstr(s), cstr(e)) for s, e in snd]
    o += ["};", "static const struct { const char *eng, *card, *hint; } TXH_CARD[] = {"]
    o += ["    { %s, %s, %s }," % (cstr(e), cstr(c), cstr(h)) for e, c, h in cards]
    o += ["};", "static const struct { const char *eng, *desc; } TXH_ENG[] = {"]
    o += ["    { %s, %s }," % (cstr(e), cstr(d)) for e, d in desc]
    o += ["};", "#endif", ""]
    return "\n".join(o)


def main():
    snd, cards, desc = parse()
    if not snd or not cards or not desc:
        sys.exit("plugin_card_hints: nothing found in docs/MANUAL.md section 5")
    out = render(snd, cards, desc)
    if "--check" in sys.argv:
        cur = open(OUT, encoding="utf-8").read() if os.path.exists(OUT) else ""
        if cur != out:
            sys.exit("plugin/card_hints.h is out of date: python3 tools/plugin_card_hints.py")
        print("card hints current: %d sounds, %d cards" % (len(snd), len(cards)))
        return
    open(OUT, "w", encoding="utf-8").write(out)
    print("wrote %s: %d sounds, %d cards" % (os.path.relpath(OUT, ROOT), len(snd), len(cards)))


if __name__ == "__main__":
    main()
