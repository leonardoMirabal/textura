#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# The desktop plugin for Linux (CLAP + VST3), built and tested in Docker (as Frodhes' tools/plugin_linux.sh): Ubuntu
# 20.04 (glibc 2.31) with clang 12 + lld (plugin/tests/docker/linux.Dockerfile). TXP_LINUX_ARCH=x86_64 (the default)
# or aarch64 (native on Apple silicon; emulated elsewhere).
#
#   tools/plugin_linux.sh build      build/plugin/linux-<arch>/bundles/Textura.clap and Textura.vst3 + the test hosts
#   tools/plugin_linux.sh check      the libraries the modules need (libc, libm, libpthread, libdl, librt, libX11 and
#                                    the loader only), no glibc symbol newer than 2.31, the architecture, nothing
#                                    exported but the entry points, a non-executable stack
#   tools/plugin_linux.sh test       in the container: the coroutine, then tools/plugin_gate.py (equal, lanes16, hq,
#                                    rates, state, gui, cpu) on the Linux CLAP; the gate's window pictures are compared
#                                    with macOS's when build/plugin/shots-macos holds them (make plugin-shots)
#   tools/plugin_linux.sh validate   clap-validator's Linux build (plugin/deps.lock) on the CLAP, in Ubuntu 24.04 (it
#                                    needs glibc 2.35: plugin/tests/docker/validator.Dockerfile); the VST3 SDK's
#                                    validator, built in the build container, on the VST3
#   tools/plugin_linux.sh pack       build/plugin/pkg/textura-plugin-linux-<arch>.zip (tools/plugin_pack.sh linux)
#   tools/plugin_linux.sh all        build, check, test, validate, pack
#   tools/plugin_linux.sh image      the two images alone (built by the steps above when missing); TXP_APT_MIRROR=URL
#                                    (e.g. http://mirrors.edge.kernel.org/ubuntu/) when archive.ubuntu.com fails
# The dependencies come from .local/plugin-deps (tools/plugin_deps.sh) and the font from .local/fonts, mounted
# read-only. Everything runs under nice, 2 jobs; each container is removed when it ends (docker run --rm).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ARCH="${TXP_LINUX_ARCH:-x86_64}"
case "$ARCH" in
    x86_64) PLAT=linux/amd64; RE='X86-64' ;;
    aarch64) PLAT=linux/arm64; RE='AArch64' ;;
    *) echo "plugin_linux: TXP_LINUX_ARCH x86_64 or aarch64" >&2; exit 2 ;;
esac
B="$ROOT/build/plugin/linux-$ARCH"
IMG="textura-plugin-linux:focal-${PLAT#linux/}"
# clap-validator 0.4.1's Linux build is linked against Ubuntu 22.04's glibc: it runs in its own image
VIMG="textura-clap-validator:noble-${PLAT#linux/}"
DEPS="${TXP_DEPS_DIR:-$ROOT/.local/plugin-deps}"
[ -d "$DEPS/clap" ] || sh "$ROOT/tools/plugin_deps.sh" --libs
[ -f "$ROOT/.local/fonts/InterTight[wght].ttf" ] || sh "$ROOT/tools/get_fonts.sh" "$ROOT/.local/fonts"
DEPS="$(cd "$DEPS" && pwd -P)"
FONTS="$(cd "$ROOT/.local/fonts" && pwd -P)"

image() {
    docker image inspect "$IMG" >/dev/null 2>&1 && return 0
    nice -n 15 docker build -q --platform "$PLAT" --build-arg "APT_MIRROR=${TXP_APT_MIRROR:-}" -t "$IMG" -f "$ROOT/plugin/tests/docker/linux.Dockerfile" \
        "$ROOT/plugin/tests/docker"
}
vimage() {
    docker image inspect "$VIMG" >/dev/null 2>&1 && return 0
    nice -n 15 docker build -q --platform "$PLAT" --build-arg "APT_MIRROR=${TXP_APT_MIRROR:-}" -t "$VIMG" -f "$ROOT/plugin/tests/docker/validator.Dockerfile" \
        "$ROOT/plugin/tests/docker"
}
mounts() {   # the tree at its own path; the dependencies and the font at their real paths (a symlinked .local works)
    echo "-v $ROOT:$ROOT -v $DEPS:$DEPS:ro"
    case "$FONTS" in "$ROOT"/*) ;; *) echo "-v $FONTS:$FONTS:ro" ;; esac
}
in_docker() {   # IMAGE COMMAND
    # (safe.directory: the container runs as root over a checkout another user owns; focal's git then refuses it,
    # and plugin/cmake/deps.cmake's "git -C <dep> rev-parse HEAD" would read '')
    # shellcheck disable=SC2046
    nice -n 15 docker run --rm --platform "$PLAT" $(mounts) -w "$ROOT" "$1" \
        bash -c "git config --global --add safe.directory '*' 2>/dev/null; $2"
}

build() {
    image
    in_docker "$IMG" "set -e; [ -f '$B/CMakeCache.txt' ] || cmake -S plugin -B '$B' -G Ninja \
        -DCMAKE_C_COMPILER=clang-12 -DCMAKE_CXX_COMPILER=clang++-12 \
        -DCMAKE_AR=/usr/bin/llvm-ar-12 -DCMAKE_RANLIB=/usr/bin/llvm-ranlib-12 -DCMAKE_NM=/usr/bin/llvm-nm-12 \
        -DCMAKE_EXE_LINKER_FLAGS=-fuse-ld=lld-12 -DCMAKE_SHARED_LINKER_FLAGS=-fuse-ld=lld-12 \
        -DCMAKE_MODULE_LINKER_FLAGS=-fuse-ld=lld-12 -DTXP_DEPS_DIR='$DEPS' -DTXP_VERSION='${PLUGIN_VER:-0.0.0-dev}' \
        >/dev/null; nice -n 15 cmake --build '$B' -j${PLUGIN_J:-2}"
}

check() {
    image
    in_docker "$IMG" "fail=0
    for f in '$B/bundles/Textura.clap' '$B/bundles/Textura.vst3/Contents/$ARCH-linux/Textura.so'; do
        n=\$(basename \"\$f\")
        [ -f \"\$f\" ] || { echo \"FAIL \$f missing\"; fail=1; continue; }
        readelf -h \"\$f\" | grep -q '$RE' || { echo \"FAIL \$n not $ARCH\"; fail=1; }
        for s in \$(nm -D --defined-only \"\$f\" | awk '{print \$3}'); do case \$s in clap_entry|GetPluginFactory|ModuleEntry|ModuleExit) ;;
            *) echo \"FAIL \$n exports \$s\"; fail=1 ;; esac; done
        for l in \$(readelf -d \"\$f\" | sed -n 's/.*(NEEDED).*\\[\\(.*\\)\\]/\\1/p'); do case \$l in
            libc.so.6|libm.so.6|libpthread.so.0|libdl.so.2|librt.so.1|libX11.so.6|ld-linux*) ;;
            *) echo \"FAIL \$n needs \$l\"; fail=1 ;; esac; done
        g=\$(objdump -T \"\$f\" | grep -o 'GLIBC_[0-9.]*' | sort -t. -k2,2n -k3,3n -u | tail -1)
        case \$g in GLIBC_2.[0-9]|GLIBC_2.[12][0-9]|GLIBC_2.[12][0-9].*|GLIBC_2.30|GLIBC_2.31|GLIBC_2.[0-9].*|'') ;;
            *) echo \"FAIL \$n needs \$g (newer than 2.31)\"; fail=1 ;; esac
        readelf -lW \"\$f\" | grep GNU_STACK | grep -q RWE && { echo \"FAIL \$n: executable stack\"; fail=1; }
        echo \"info  \$n: \$(stat -c %s \"\$f\") bytes, newest glibc symbol \${g:-none}, needs \$(readelf -d \"\$f\" | sed -n 's/.*(NEEDED).*\\[\\(.*\\)\\]/\\1/p' | tr '\\n' ' ')\"
    done
    [ \$fail = 0 ] && echo 'ok    check: $ARCH, exports, libraries, glibc 2.31, stack'; exit \$fail"
}

test_linux() {
    image
    local shots="$ROOT/build/plugin/shots-linux-$ARCH"
    rm -rf "$shots"
    in_docker "$IMG" "set -e; echo '== coroutine'; '$B/txp_test_coro'
        echo '== gate'; CC=clang-12 python3 -I tools/plugin_gate.py --build '${B#"$ROOT"/}' --shots '$shots' ${GATE_ARGS:-}"
    compare_shots "$shots"
}

compare_shots() {   # the gate's window pictures against macOS's (build/plugin/shots-macos: make plugin-shots)
    local mac="$ROOT/build/plugin/shots-macos"
    if [ ! -d "$mac" ]; then echo "skip  window pictures against macOS: no $mac (make plugin-shots)"; return 0; fi
    python3 -I "$ROOT/tools/plugin_shots_cmp.py" "$1" "$mac"
}

validate() {
    image
    [ -x "$DEPS/clap-validator-linux/clap-validator" ] || [ -x "$DEPS/clap-validator-linux/binaries/clap-validator" ] ||
        TXP_DEPS_OS=linux sh "$ROOT/tools/plugin_deps.sh" --tools
    local cv
    cv=$(find "$DEPS/clap-validator-linux" -name clap-validator -type f | head -1)
    [ "$ARCH" = x86_64 ] || { echo "skip  clap-validator: its Linux build is x86_64 only"; cv=""; }
    local rc=0
    if [ -n "$cv" ]; then
        vimage
        # shellcheck disable=SC2046
        nice -n 15 docker run --rm --platform "$PLAT" $(mounts) -w "$ROOT" "$VIMG" \
            bash -c "'$cv' validate '$B/bundles/Textura.clap' > '$B/clap-validator.txt' 2>&1; r=\$?; tail -3 '$B/clap-validator.txt'; exit \$r" || rc=1
        [ $rc = 0 ] && echo "ok    clap-validator ($B/clap-validator.txt)" || echo "FAIL  clap-validator ($B/clap-validator.txt)"
    fi
    in_docker "$IMG" "sh tools/plugin_vst3_validator.sh '$B/bundles/Textura.vst3' '$B/vst3-validator'" || rc=1
    return $rc
}

pack() { sh "$ROOT/tools/plugin_pack.sh" linux "$B"; }

case "${1:-}" in
    image) image; vimage ;;
    build) build ;;
    check) check ;;
    test) test_linux ;;
    validate) validate ;;
    pack) pack ;;
    shots) compare_shots "$2" ;;   # (DIR: another build's window pictures against macOS's; tools/plugin_win.sh test)
    all) build; check; test_linux; validate; pack ;;
    *) sed -n '3,20p' "$0"; exit 2 ;;
esac
