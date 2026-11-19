# SPDX-License-Identifier: GPL-3.0-only
"""The plugin manual's window pictures (docs/plugin/img/*.png) from the gate's headless window shots.

The gate paints the window and its views from plugin/tests/scripts/gui4.txt and gui16.txt (txp_clap_render --shot);
this keeps the ones the manual shows, as 256-colour PNGs (the window is dark and nearly monochrome: no visible loss,
about a tenth of the size). Needs Pillow.

  make plugin
  python3 -I tools/plugin_gate.py --build build/plugin --only gui --shots build/plugin/shots-manual
  python3 -I tools/plugin_manual_shots.py [build/plugin/shots-manual] [docs/plugin/img]
"""
import os
import sys

from PIL import Image

PICK = {   # the gate's picture -> the manual's
    "gui4": "window-4-lanes",
    "gui16": "window-16-lanes",
    "gui4-space": "space",
    "gui4-sounds": "sound-browser",
    "gui4-presets": "preset-browser",
    "gui4-settings": "settings",
    "gui4-colour": "colour-picker",
}


def main():
    src = sys.argv[1] if len(sys.argv) > 1 else "build/plugin/shots-manual"
    dst = sys.argv[2] if len(sys.argv) > 2 else "docs/plugin/img"
    os.makedirs(dst, exist_ok=True)
    for k, v in PICK.items():
        im = Image.open(os.path.join(src, "textura-window-%s.png" % k)).convert("RGB")
        q = im.quantize(colors=256, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.NONE)
        out = os.path.join(dst, v + ".png")
        q.save(out, optimize=True)
        print("%-40s %7d bytes" % (out, os.path.getsize(out)))


if __name__ == "__main__":
    main()
