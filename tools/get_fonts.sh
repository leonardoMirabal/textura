#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Fetch the two fonts make fw-fonts rasterises (both SIL OFL 1.1; their licences are in assets/fonts/) from their own
# releases into DEST (default .local/fonts), checked against pinned sha256 values: Terminus 4.49.1's ter-u16n.bdf
# (felucca_font.h) and Google Fonts' Inter Tight variable font (aa_font.h, aa_font_v24.h). Never committed.
#   tools/get_fonts.sh [DEST]
set -e
DEST="${1:-.local/fonts}"
mkdir -p "$DEST"
if [ ! -f "$DEST/ter-u16n.bdf" ]; then
    T="$DEST/terminus.tmp"
    rm -rf "$T" && mkdir -p "$T"
    curl -fsSL --retry 3 -o "$T/terminus.tar.gz" \
      https://downloads.sourceforge.net/project/terminus-font/terminus-font-4.49/terminus-font-4.49.1.tar.gz
    tar -xzf "$T/terminus.tar.gz" -C "$T" terminus-font-4.49.1/ter-u16n.bdf
    mv "$T/terminus-font-4.49.1/ter-u16n.bdf" "$DEST/" && rm -rf "$T"
fi
[ -f "$DEST/InterTight[wght].ttf" ] || curl -fsSL --retry 3 -o "$DEST/InterTight[wght].ttf" \
  'https://raw.githubusercontent.com/google/fonts/fac60545cc0de9d4eff6eb5b1ab88bcdfc9ecc69/ofl/intertight/InterTight%5Bwght%5D.ttf'
(cd "$DEST" && shasum -a 256 -c - <<'SUMS'
5197662b22bf9f3e68d4af9f969a7fefa3edae40dd82ae969a147381130fb4ae  ter-u16n.bdf
b81b73dcb64df3c230cabade7df6c5773bf863233f24c9ee51087519f1f88b6f  InterTight[wght].ttf
SUMS
)
echo "FONTS=$DEST"
