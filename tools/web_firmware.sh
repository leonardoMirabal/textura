#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# web_firmware.sh FWSC DIST X.Y.Z: the release's FM-1 package next to the emulator, for the page's installer
# (web/install.js): DIST/textura.fwsc and DIST/firmware.json ({version, identity, size, sha256, file} and packages:
# [the same]). GitHub's release downloads send no CORS headers, so the page fetches its own copy from Pages and checks
# it against this sha256. Used by release.yml (job release, before tools/deploy_pages.sh, only when PAGES_ENABLED is
# 'true'). From Frodhes' tools/web_firmware.sh (the FM-1 only).
set -e
FWSC="$1"; DIST="$2"; VER="$3"
[ -s "$FWSC" ] && [ -d "$DIST" ] && [ -n "$VER" ] || { echo "usage: web_firmware.sh FWSC DIST X.Y.Z" >&2; exit 2; }
cd "$(dirname "$0")/.."
python3 - "$DIST" "$VER" "$FWSC" <<'PY'
import hashlib, json, sys
sys.path.insert(0, "tools")
import fm1_install as I
dist, ver, fwsc = sys.argv[1], sys.argv[2], sys.argv[3]
WANT = "FM-1_959"                                  # web/fm1install.js PRODUCT
raw = open(fwsc, "rb").read()
product, _image = I.load_package(fwsc, False)
if product != WANT:
    sys.exit(f"web_firmware.sh: {fwsc} is {product}, not {WANT}")
p = {"identity": product, "file": "textura.fwsc", "size": len(raw), "sha256": hashlib.sha256(raw).hexdigest()}
open(f"{dist}/textura.fwsc", "wb").write(raw)
open(f"{dist}/firmware.json", "w").write(json.dumps({"version": ver, **p, "packages": [p]}, indent=1) + "\n")
print(f"{dist}/textura.fwsc: {product} {ver}, {p['size']} B, sha256 {p['sha256']}")
PY
