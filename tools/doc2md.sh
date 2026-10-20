#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Convert research documents to text next to them (FILE.pdf -> FILE.md), for reading and searching.
#   tools/doc2md.sh FILE_OR_DIR...      (a directory is walked; existing .md files are kept unless -f)
# PDFs: poppler's pdftotext (keeps word spacing and two-column order; equations stay rough: check the PDF).
# Everything else (html, docx, pptx, xlsx, epub ...): Microsoft MarkItDown from .local/venv (make it with
#   python3 -m venv .local/venv && .local/venv/bin/pip install -r requirements.txt).
set -e
cd "$(dirname "$0")/.."
MID=.local/venv/bin/markitdown
force=0; [ "$1" = -f ] && { force=1; shift; }
[ $# -gt 0 ] || { echo "usage: tools/doc2md.sh [-f] FILE_OR_DIR..."; exit 2; }
one() {
    in=$1; out=${in%.*}.md
    [ "$in" = "$out" ] && return 0
    [ $force = 0 ] && [ -s "$out" ] && return 0
    case $(printf %s "$in" | tr A-Z a-z) in
        *.pdf) pdftotext -enc UTF-8 "$in" "$out" ;;
        *.html|*.htm|*.docx|*.pptx|*.xlsx|*.epub|*.ipynb) "$MID" "$in" -o "$out" ;;
        *) return 0 ;;
    esac
    echo "$out ($(wc -l < "$out" | tr -d ' ') lines)"
}
for a in "$@"; do
    if [ -d "$a" ]; then find "$a" -type f | while read -r f; do one "$f"; done; else one "$a"; fi
done
