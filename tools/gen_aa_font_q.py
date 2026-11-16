# SPDX-License-Identifier: GPL-3.0-only
"""Textura: Frodhes' gen_aa_font.py (tools/fonts/, staged by make fw-fonts) with fewer alpha levels.

  gen_aa_font_q.py TOOLS_DIR BITS ARGS...   runs TOOLS_DIR/gen_aa_font.py ARGS with each pixel's coverage kept to
                                            2^BITS levels (4: Frodhes' 16, unchanged; 2: 0, 5, 10, 15 of 15)

The glyphs stay 4-bit alpha (aatext.c draws them as before); fewer levels leave the Huffman code fewer symbols, so a
face takes less flash (2 bits: S -46 %, the value face -41 %), at a look the screen hardly shows (make fw-fonts)."""
import runpy
import sys

tools, bits, args = sys.argv[1], int(sys.argv[2]), sys.argv[3:]
sys.path.insert(0, tools)
import aa_raster  # noqa: E402  (the generator's own module: it imports this one)

if bits < 4:
    n = (1 << bits) - 1
    LEVELS = [round(15 * i / n) for i in range(n + 1)]

    def alpha4(v, gamma=1.0):
        """0..255 coverage -> one of 2^bits levels on 0..15."""
        return LEVELS[int(round(n * ((v / 255.0) ** gamma)))]

    aa_raster.alpha4 = alpha4
sys.argv = [tools + "/gen_aa_font.py"] + args
runpy.run_path(tools + "/gen_aa_font.py", run_name="__main__")
