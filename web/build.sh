#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# The Textura panel in a web browser: firmware/src/textura.c compiled to WebAssembly with Emscripten (the browser shim
# is web/web_hal.c), the engines beside it. Output: build/web/ (index.html, app.js, version.js, textura.js,
# textura.wasm), a static site. From Frodhes' web/build.sh.
#
#   web/build.sh [--release X.Y.Z]   (Emscripten from .local/emsdk, or emcc on the PATH)
# The version is stamped at build time, never written in the repo: --release X.Y.Z shows "Textura X.Y.Z" in the page's
# title, heading and footer (build/web/version.js) and on the screen (APP_VER); without it the build is "dev" (the
# screen shows DEV).
#   python3 -m http.server -d build/web 8000     then open http://localhost:8000   (or: make fw-web-serve)
set -e
cd "$(dirname "$0")/.."
VER=dev
if [ "$1" = "--release" ]; then
  VER="$2"
  echo "$VER" | grep -Eq '^[0-9]+\.[0-9]+(\.[0-9]+)?(-[A-Za-z0-9.]+)?$' || { echo "--release $VER: use X.Y.Z" >&2; exit 2; }
fi
# (sourced from its own folder, as Frodhes 03305ce99: under dash, the CI's sh, emsdk_env.sh cannot find where it is
# and sets nothing)
if [ -f .local/emsdk/emsdk_env.sh ]; then cd .local/emsdk && . ./emsdk_env.sh >/dev/null 2>&1; cd ../..; fi
command -v emcc >/dev/null || { echo "no emcc: install emsdk into .local/emsdk (see audition/README.md)"; exit 1; }
OUT=build/web
mkdir -p $OUT
emcc -O2 -DDINGHY_HOST -DDINGHY_WEB -DHOST_SAVE_MS=1u -DAPP_VER="\"$VER\"" -Ifirmware/src -Ifirmware/gen -Iengines \
  -Wno-unused-function -Wno-unused-variable \
  firmware/src/textura.c engines/*.c -o $OUT/textura.js \
  -sASYNCIFY -sASYNCIFY_IMPORTS='["emscripten_sleep"]' -sALLOW_MEMORY_GROWTH=0 -sINITIAL_MEMORY=33554432 \
  -sEXPORTED_FUNCTIONS='["_main","_web_audio","_web_btn","_web_keys","_web_enc","_web_midi","_web_master","_web_fb","_web_leds","_web_store","_web_store_take_dirty","_web_flw_buf","_web_flw_len","_web_flw_take_dirty","_web_page","_web_scene","_web_state"]' \
  -sEXPORTED_RUNTIME_METHODS='["HEAPU8","HEAPF32","ccall"]' -sENVIRONMENT=web
cp web/index.html web/app.js $OUT/
printf 'window.TEXTURA_VERSION = "%s";\n' "$VER" > $OUT/version.js
echo "$OUT: $(ls $OUT | tr '\n' ' ')"
