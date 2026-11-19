#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# The desktop plugin for Windows x64 (CLAP + VST3), cross-built on macOS with clang-cl + lld-link (as Frodhes'
# tools/plugin_win.sh; plugin/cmake/win-x64-clangcl.cmake).
#
#   tools/plugin_win.sh setup      the toolchain, once: Homebrew llvm@20 + lld@20 (clang-cl, lld-link, llvm-lib/rc/mt/
#                                  nm), xwin 0.10.0 (cargo) and its splat of the Windows SDK + MSVC CRT, x86_64 only,
#                                  into .local/win-toolchain/sdk (~630 MB; xwin is told to accept Microsoft's licence:
#                                  read https://go.microsoft.com/fwlink/?LinkId=2086102 first)
#   tools/plugin_win.sh build      build/plugin/win-x86_64/bundles/CLAP/Textura.clap and VST3/Textura.vst3 + the test hosts
#   tools/plugin_win.sh check      x86_64 PE, the CLAP / VST3 entry points exported, only Windows' own DLLs imported
#                                  (the static CRT: no VCRUNTIME, no UCRT DLLs)
#   tools/plugin_win.sh test       under wine in Docker (amd64, plugin/tests/docker/wine.Dockerfile): the coroutine, then
#                                  tools/plugin_gate.py --wine (equal, lanes16, hq, rates, state, gui, cpu); GATE_ARGS
#                                  adds options (--quick). The window pictures are compared with macOS's when
#                                  build/plugin/shots-macos holds them. wine is not Windows: a pass is a smoke test.
#   tools/plugin_win.sh validate   clap-validator's Windows build (plugin/deps.lock) under wine on the CLAP
#   tools/plugin_win.sh pack       build/plugin/pkg/textura-plugin-windows-x64.zip (tools/plugin_pack.sh windows)
#   tools/plugin_win.sh all        build, check, test, validate, pack
# On a Windows machine none of this is needed: cmake -S plugin -B build/plugin -G Ninja
# -DCMAKE_TOOLCHAIN_FILE=plugin/cmake/win-x64-clangcl.cmake from a Visual Studio x64 prompt with LLVM installed.
# Light: builds under nice -n 15, 2 jobs; each container is removed when it ends.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
MAIN="$(cd "$(git -C "$ROOT" rev-parse --git-common-dir)/.." && pwd)"     # (a worktree: the main checkout)
TC="$MAIN/.local/win-toolchain"
B="$ROOT/build/plugin/win-x86_64"
LLVM="${TXP_LLVM_BIN:-/opt/homebrew/opt/llvm@20/bin}"
LLD="${TXP_LLD_BIN:-/opt/homebrew/opt/lld@20/bin}"
IMG=textura-wine:bookworm-amd64
DEPS="${TXP_DEPS_DIR:-$ROOT/.local/plugin-deps}"

setup() {
    [ -x "$LLVM/clang-cl" ] || brew install llvm@20
    [ -x "$LLD/lld-link" ] || brew install lld@20
    if [ ! -x "$TC/xwin-bin/bin/xwin" ]; then
        mkdir -p "$TC"
        nice -n 15 cargo install xwin --version 0.10.0 --locked -j2 --root "$TC/xwin-bin"
    fi
    if [ ! -d "$TC/sdk/sdk/lib/um/x86_64" ]; then
        (cd "$TC" && nice -n 15 ./xwin-bin/bin/xwin --accept-license --arch x86_64 --cache-dir xwin-cache splat --output sdk)
        rm -rf "$TC/xwin-cache"
    fi
    echo "plugin_win: toolchain ready in $TC (clang-cl: $LLVM, lld-link: $LLD)"
}

build() {
    [ -d "$TC/sdk/sdk/lib/um/x86_64" ] || { echo "plugin_win: run tools/plugin_win.sh setup first" >&2; exit 1; }
    [ -d "$DEPS/clap" ] || sh "$ROOT/tools/plugin_deps.sh" --libs
    [ -f "$ROOT/.local/fonts/InterTight[wght].ttf" ] || sh "$ROOT/tools/get_fonts.sh" "$ROOT/.local/fonts"
    [ -f "$B/CMakeCache.txt" ] || nice -n 15 cmake -S "$ROOT/plugin" -B "$B" -G Ninja \
        -DCMAKE_TOOLCHAIN_FILE="$ROOT/plugin/cmake/win-x64-clangcl.cmake" -DTXP_XWIN="$TC/sdk" -DTXP_LLVM_BIN="$LLVM" \
        -DTXP_LLD_BIN="$LLD" -DTXP_DEPS_DIR="$DEPS" -DTXP_VERSION="${PLUGIN_VER:-0.0.0-dev}" >/dev/null
    nice -n 15 cmake --build "$B" -j"${PLUGIN_J:-2}"
}

check() {
    local fail=0 f n dlls
    for f in "$B/bundles/CLAP/Textura.clap" "$B/bundles/VST3/Textura.vst3/Contents/x86_64-win/Textura.vst3"; do
        n=$(basename "$f")
        [ -f "$f" ] || { echo "FAIL $f missing"; fail=1; continue; }
        "$LLVM/llvm-objdump" -f "$f" | grep -q 'file format coff-x86-64' || { echo "FAIL $n is not an x86_64 PE"; fail=1; }
        dlls=$("$LLVM/llvm-objdump" -p "$f" | sed -n 's/^ *DLL Name: //p' | tr 'A-Z' 'a-z' | sort -u | tr '\n' ' ')
        for d in $dlls; do
            case $d in kernel32.dll|user32.dll|gdi32.dll|ole32.dll|shell32.dll|advapi32.dll|ntdll.dll) ;;
                *) echo "FAIL $n imports $d"; fail=1 ;; esac
        done
        case $n in
            Textura.clap) want="clap_entry" ;;
            *) want="GetPluginFactory InitDll ExitDll" ;;
        esac
        for s in $want; do
            "$LLVM/llvm-objdump" -p "$f" | grep -qE "^ +[0-9]+ +(0x)?[0-9a-f]+ +$s\$" ||
                "$LLVM/llvm-objdump" -p "$f" | grep -qw "$s" || { echo "FAIL $n does not export $s"; fail=1; }
        done
        echo "info  $n: $(wc -c < "$f" | tr -d ' ') bytes, imports $dlls"
    done
    [ $fail = 0 ] && echo "ok    check: x86_64 PE, entry points, system DLLs only"
    return $fail
}

wine_image() {
    docker image inspect "$IMG" >/dev/null 2>&1 && return 0
    nice -n 15 docker build -q --platform linux/amd64 -t "$IMG" -f "$ROOT/plugin/tests/docker/wine.Dockerfile" \
        "$ROOT/plugin/tests/docker"
}
in_wine() {
    local deps
    deps="$(cd "$DEPS" && pwd -P)"
    nice -n 15 docker run --rm --platform linux/amd64 -v "$ROOT:$ROOT" -v "$deps:$deps:ro" -w "$ROOT" "$IMG" bash -c "$1"
}

test_wine() {
    wine_image
    local shots="$ROOT/build/plugin/shots-win-x86_64"
    rm -rf "$shots"
    in_wine "set -e; echo '== coroutine'; wine '$B/txp_test_coro.exe'
        echo '== gate (wine)'; python3 -I tools/plugin_gate.py --wine --build '${B#"$ROOT"/}' --shots '$shots' ${GATE_ARGS:-}"
    bash "$ROOT/tools/plugin_linux.sh" shots "$shots"
}

validate() {
    wine_image
    [ -f "$DEPS/clap-validator-windows/clap-validator.exe" ] || TXP_DEPS_OS=windows sh "$ROOT/tools/plugin_deps.sh" --tools
    local rc=0 w="$B/wine-shim" sdk="$TC/sdk"
    # (wine 8 has no bcryptprimitives.dll, which clap-validator's Rust std imports: a stub next to a copy of it)
    mkdir -p "$w"
    "$LLVM/clang-cl" --target=x86_64-pc-windows-msvc /O2 /c -imsvc "$sdk/crt/include" -imsvc "$sdk/sdk/include/ucrt" \
        -imsvc "$sdk/sdk/include/um" -imsvc "$sdk/sdk/include/shared" /Fo"$w/bcp.obj" \
        -- "$ROOT/plugin/tests/wine_bcryptprimitives.c" >/dev/null
    "$LLD/lld-link" /DLL /NOENTRY /NODEFAULTLIB /MACHINE:X64 /OUT:"$w/bcryptprimitives.dll" "$w/bcp.obj" \
        /LIBPATH:"$sdk/sdk/lib/um/x86_64" advapi32.lib >/dev/null
    cp "$DEPS/clap-validator-windows/clap-validator.exe" "$w/"
    in_wine "wine 'Z:$w/clap-validator.exe' validate 'Z:$B/bundles/CLAP/Textura.clap' \
        > '$B/clap-validator.txt' 2>&1; r=\$?; tail -3 '$B/clap-validator.txt'; exit \$r" || rc=1
    [ $rc = 0 ] && echo "ok    clap-validator under wine ($B/clap-validator.txt)" ||
        echo "FAIL  clap-validator under wine ($B/clap-validator.txt)"
    return $rc
}

pack() { sh "$ROOT/tools/plugin_pack.sh" windows "$B"; }

case "${1:-}" in
    setup) setup ;;
    build) build ;;
    check) check ;;
    test) test_wine ;;
    validate) validate ;;
    pack) pack ;;
    all) build; check; test_wine; validate; pack ;;
    *) sed -n '3,22p' "$0"; exit 2 ;;
esac
