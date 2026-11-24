# SPDX-License-Identifier: GPL-3.0-only
"""The plugin manual's window pictures (docs/plugin/img/*.png) from the gate's headless window shots.

The gate paints the window and its views from plugin/tests/scripts/gui4.txt and gui16.txt (txp_clap_render --shot);
this keeps the ones the manual shows, as 256-colour PNGs (the window is dark and nearly monochrome: no visible loss,
about a tenth of the size); a crop where the manual shows one part (fractions of the window: left, top, right, bottom).
Needs Pillow.

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
    "gui4-ms-tab": "ms-view",
    "gui4-sounds": "engine-browser",
    "gui4-presets": "preset-browser",
    "gui4-settings": "settings",
    "gui4-colour": "colour-picker",
    "gui4-fx": "effects",
    "gui4-lfo": "lfo",
    "gui4-env": "env-follow",
    "gui4-matrix": "matrix",
    "gui4-comp": "compressor",
    "gui4-filter": "filter",
    "gui4-reverb": "reverb",
    "gui4-ms": "mid-side",
    "gui4-preview": "engine-preview",
    "gui4-stopped": "stopped",
    "gui4-lane-stopped": "lane-stopped",
    "gui4-save": "save",
    "gui4-save-replace": "save-replace",
    "gui4-random-list": "random",
    "gui4-mode": "movement-mode",
    "gui4-drag": "movement-drag",
    "gui4-links": "movement-links",
    "gui4-menu": "movement-menu",
    "960x600": "window-960x600",
    "gui4-tour-2": "tour",
    "gui4-output": ("output", (0.55, 0.79, 1.0, 1.0)),   # (the strip's right end: its band with OUTPUT, M/S, MOVEMENT)
    "gui4-binaural": ("binaural", (0.55, 0.42, 1.0, 1.0)),   # (OUTPUT's arrow: BINAURAL's panel over the strip's right end)
}


def main():
    src = sys.argv[1] if len(sys.argv) > 1 else "build/plugin/shots-manual"
    dst = sys.argv[2] if len(sys.argv) > 2 else "docs/plugin/img"
    os.makedirs(dst, exist_ok=True)
    for k, v in PICK.items():
        im = Image.open(os.path.join(src, "textura-window-%s.png" % k)).convert("RGB")
        if isinstance(v, tuple):
            v, (x0, y0, x1, y1) = v
            w, h = im.size
            im = im.crop((round(x0 * w), round(y0 * h), round(x1 * w), round(y1 * h)))
        q = im.quantize(colors=256, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.NONE)
        out = os.path.join(dst, v + ".png")
        q.save(out, optimize=True)
        print("%-40s %7d bytes" % (out, os.path.getsize(out)))


if __name__ == "__main__":
    main()
