#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# The desktop plugin's user manual as a PDF: docs/PLUGIN-MANUAL.md and its pictures (docs/plugin/img) to HTML, then
# printed by a headless Chromium. Uses only what is on the machine, nothing is committed or fetched:
#   Markdown -> HTML   pandoc (gfm), else python3's markdown module
#   HTML -> PDF        $CHROME, else Playwright's Chromium in .local/playwright, else Google Chrome / chromium
# The links into the FM-1 manual (MANUAL.md#...) point at its page on GitHub in the PDF.
#   tools/plugin_manual_pdf.sh [OUT.pdf]        (default build/plugin/pkg/Textura-Plugin-Manual.pdf; make plugin-manual)
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
SRC=$ROOT/docs/PLUGIN-MANUAL.md
OUT=${1:-$ROOT/build/plugin/pkg/Textura-Plugin-Manual.pdf}
W=$ROOT/build/plugin/manual
REPO=https://github.com/leonardoMirabal/textura/blob/main/docs
die() { echo "plugin_manual_pdf: $*" >&2; exit 1; }

[ -f "$SRC" ] || die "no $SRC"
rm -rf "$W"; mkdir -p "$W/plugin" "$W/img" "$(dirname "$OUT")"
cp -R "$ROOT/docs/plugin/img" "$W/plugin/"
cp "$ROOT/docs/img/card.png" "$W/img/"
sed -e "s#](MANUAL\.md#]($REPO/MANUAL.md#g" -e "s#](PLUGIN\.md#]($REPO/PLUGIN.md#g" "$SRC" > "$W/body.md"

# ---- Markdown -> HTML (a fragment; heading ids as GitHub makes them, so the contents' links work)
if command -v pandoc >/dev/null 2>&1; then
    pandoc -f gfm -t html5 "$W/body.md" -o "$W/body.html"
elif python3 -c 'import markdown' 2>/dev/null; then
    python3 -c 'import sys, markdown
src = open(sys.argv[1], encoding="utf-8").read()
open(sys.argv[2], "w", encoding="utf-8").write(markdown.markdown(src, extensions=["tables", "toc", "fenced_code", "sane_lists"]))' \
        "$W/body.md" "$W/body.html"
else
    die "needs pandoc or python3's markdown module"
fi

# ---- the font: Inter Tight (the window's) when tools/get_fonts.sh has fetched it, else the system's sans
FONT=""
if [ -f "$ROOT/.local/fonts/InterTight[wght].ttf" ]; then
    cp "$ROOT/.local/fonts/InterTight[wght].ttf" "$W/InterTight.ttf"
    FONT="@font-face { font-family: 'Inter Tight'; src: url('InterTight.ttf') format('truetype'); font-weight: 100 900; }"
fi

{
cat <<EOF
<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<title>Textura — desktop plugin manual</title>
<style>
$FONT
@page { size: A4; margin: 18mm 17mm 20mm 17mm;
  @bottom-center { content: counter(page); font: 8.5pt 'Inter Tight', 'Helvetica Neue', Arial, sans-serif; color: #888; }
  @bottom-left { content: "Textura — desktop plugin manual"; font: 8pt 'Inter Tight', 'Helvetica Neue', Arial, sans-serif; color: #aaa; } }
@page :first { @bottom-center { content: none; } @bottom-left { content: none; } }
html { -webkit-print-color-adjust: exact; print-color-adjust: exact; }
body { font-family: 'Inter Tight', 'Helvetica Neue', Helvetica, Arial, 'DejaVu Sans', sans-serif; font-size: 10pt;
  line-height: 1.45; color: #1d1d1f; margin: 0; }
.cover { height: 255mm; display: flex; flex-direction: column; align-items: center; justify-content: center;
  text-align: center; break-after: page; }
.cover img { width: 70mm; height: 70mm; image-rendering: auto; border-radius: 2mm; }
.cover h1 { font-size: 28pt; font-weight: 700; letter-spacing: 0.02em; margin: 14mm 0 3mm; color: #1d1d1f; border: 0; }
.cover h1 span { color: #EF4A3C; }
.cover p { margin: 1.5mm 0; color: #555; font-size: 11pt; }
.cover .small { margin-top: 18mm; font-size: 9pt; color: #888; }
main > h1:first-child { display: none; }
h1, h2, h3, h4 { font-weight: 700; line-height: 1.2; break-after: avoid; page-break-after: avoid; }
h2 { font-size: 17pt; margin: 9mm 0 3mm; padding-bottom: 1.5mm; border-bottom: 0.6mm solid #EF4A3C; }
h3 { font-size: 12.5pt; margin: 6mm 0 2mm; color: #EF4A3C; }
h4 { font-size: 10.5pt; margin: 4mm 0 1.5mm; }
p { margin: 0 0 2.4mm; orphans: 3; widows: 3; }
ul, ol { margin: 0 0 2.6mm; padding-left: 5.5mm; }
li { margin: 0.6mm 0; }
a { color: inherit; text-decoration: none; border-bottom: 0.2mm solid #EF4A3C; }
strong { font-weight: 650; }
code { font-family: Menlo, 'DejaVu Sans Mono', Consolas, monospace; font-size: 8.6pt; background: #f3f3f3;
  padding: 0.2mm 1mm; border-radius: 0.8mm; }
pre { background: #f3f3f3; padding: 2.5mm 3.5mm; border-radius: 1.2mm; overflow-wrap: anywhere; white-space: pre-wrap;
  break-inside: avoid; }
pre code { background: none; padding: 0; }
table { border-collapse: collapse; width: 100%; margin: 1.5mm 0 3.5mm; font-size: 9pt; break-inside: auto; }
tr { break-inside: avoid; }
th, td { text-align: left; vertical-align: top; padding: 1.3mm 2mm; border-bottom: 0.2mm solid #ddd; }
th { border-bottom: 0.4mm solid #1d1d1f; font-weight: 650; }
img { max-width: 100%; }
p > img:only-child, p img { display: block; margin: 2mm auto 3mm; border-radius: 1mm; break-inside: avoid; }
p:has(> img) { break-inside: avoid; }
</style></head><body>
<section class="cover">
  <img src="img/card.png" alt="Textura">
  <h1>Textura <span>—</span> desktop plugin manual</h1>
  <p>CLAP, VST3 and Audio Unit for macOS · CLAP and VST3 for Windows and Linux</p>
  <p>Up to 16 lanes of rain, streams, fire, stones, ice, wind, insects and birds, computed live</p>
  <p class="small">Free software, GPL-3.0 · github.com/leonardoMirabal/textura</p>
</section>
<main>
EOF
cat "$W/body.html"
echo "</main></body></html>"
} > "$W/manual.html"

# ---- HTML -> PDF
B=${CHROME:-}
if [ -z "$B" ]; then
    for c in "$ROOT"/.local/playwright/chromium_headless_shell-*/chrome-headless-shell-*/chrome-headless-shell \
             "$ROOT"/.local/playwright/chromium-*/chrome-mac*/"Google Chrome for Testing.app"/Contents/MacOS/"Google Chrome for Testing" \
             "$ROOT"/.local/playwright/chromium-*/chrome-linux*/chrome \
             "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome" \
             "/Applications/Chromium.app/Contents/MacOS/Chromium"; do
        if [ -x "$c" ]; then B=$c; break; fi
    done
fi
if [ -z "$B" ]; then
    for c in google-chrome google-chrome-stable chromium chromium-browser; do
        if command -v "$c" >/dev/null 2>&1; then B=$(command -v "$c"); break; fi
    done
fi
[ -n "$B" ] || die "no Chromium or Chrome found (set CHROME=/path/to/chrome)"
rm -f "$OUT"
"$B" --headless --disable-gpu --no-first-run --no-default-browser-check --allow-file-access-from-files \
    --user-data-dir="$W/profile" --no-pdf-header-footer --virtual-time-budget=10000 \
    --print-to-pdf="$OUT" "file://$W/manual.html" >"$W/chrome.log" 2>&1 || { cat "$W/chrome.log" >&2; die "the print failed"; }
[ -s "$OUT" ] || { cat "$W/chrome.log" >&2; die "no PDF written"; }
rm -rf "$W/profile"
PAGES=$(python3 -c 'import re, sys; print(len(re.findall(rb"/Type\s*/Page[^s]", open(sys.argv[1], "rb").read())))' "$OUT")
echo "plugin_manual_pdf: $OUT ($PAGES pages, $(wc -c < "$OUT" | tr -d ' ') bytes)"
