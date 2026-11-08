#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Textura player: engines/ compiled to WebAssembly with Emscripten -> build/audition/ (a static site).
#   sh audition/build.sh            (Emscripten from .local/emsdk, or emcc on the PATH)
#   python3 -m http.server -d build/audition 8000      then open http://localhost:8000
set -e
cd "$(dirname "$0")/.."
if [ -f .local/emsdk/emsdk_env.sh ]; then . .local/emsdk/emsdk_env.sh >/dev/null 2>&1; fi
command -v emcc >/dev/null || { echo "no emcc: install emsdk into .local/emsdk (see audition/README.md)"; exit 1; }
mkdir -p build/audition
emcc -O2 -std=c99 -D_DEFAULT_SOURCE -Iengines -Ibench -Wall -Wno-unused-function \
  audition/tx_web.c bench/tx_script.c bench/tx_host.c engines/*.c -o build/audition/txweb.js \
  -sALLOW_MEMORY_GROWTH=0 -sINITIAL_MEMORY=16777216 -sENVIRONMENT=web \
  -sEXPORTED_FUNCTIONS='["_txw_count","_txw_name","_txw_title","_txw_state_bytes","_txw_select","_txw_slots","_txw_slot","_txw_pan","_txw_trim","_txw_gate","_txw_comp_gr","_txw_key_slot","_txw_slot_time","_txw_slot_peak","_txw_nparams","_txw_pname","_txw_phelp","_txw_pmax","_txw_pdef","_txw_particles_max","_txw_particles_def","_txw_param","_txw_key","_txw_render","_txw_peak_take","_txw_lim_take","_txw_stats","_txw_ref","_txw_ref_step","_txw_max_sha","_txw_ch_n","_txw_ch_pname","_txw_ch_phelp","_txw_ch_pmax","_txw_ch_pdef","_txw_ch_param","_txw_ch_on","_txw_ch_max_sha","_txw_gen_on","_txw_gen_mute","_txw_gen_n","_txw_gen_pname","_txw_gen_phelp","_txw_gen_pmax","_txw_gen_pdef","_txw_gen_param","_txw_gen_state","_txw_gov_on","_txw_gov_budget","_txw_gov_factor","_txw_gov_cur","_txw_gov_user","_txw_fx_n","_txw_fx_pname","_txw_fx_phelp","_txw_fx_pmax","_txw_fx_pdef","_txw_fx_param","_txw_fx_frozen","_txw_fx_running","_txw_fx_grains","_txw_fx_cap","_txw_host_name","_txw_host_n","_txw_host_pname","_txw_host_phelp","_txw_host_pmax","_txw_host_value","_txw_host_over","_txw_host_param"]' \
  -sEXPORTED_RUNTIME_METHODS='["HEAPF32","HEAP32","UTF8ToString"]'
cp audition/index.html audition/app.js build/audition/
if [ -f build/bench/calib.json ]; then cp build/bench/calib.json build/audition/; else
  echo "no build/bench/calib.json (make tx-bench): the player shows browser times only"; rm -f build/audition/calib.json; fi
echo "build/audition: $(ls build/audition | tr '\n' ' ')"
