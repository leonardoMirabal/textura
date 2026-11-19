#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# The desktop plugin's packages for Windows and Linux (docs/PLUGIN.md "Packages"; as Frodhes' tools/plugin_pack.sh):
# zips ready to hand out. The bundles first: tools/plugin_win.sh build, tools/plugin_linux.sh build (their pack step
# calls this script).
#   tools/plugin_pack.sh windows [BUILD] [OUT]    OUT/textura-plugin-windows-x64.zip
#   tools/plugin_pack.sh linux   [BUILD] [OUT]    OUT/textura-plugin-linux-<x86_64|aarch64>.zip (the bundle's arch)
# BUILD defaults to build/plugin/win-x86_64, build/plugin/linux-x86_64; OUT to build/plugin/pkg.
# Every package carries the GPL (LICENSE.txt), the font's licence (OFL-InterTight.txt), the source offer
# (SOURCE.txt), THIRD-PARTY-NOTICES.txt (the licence texts of what the binaries contain, from the pinned checkouts)
# and INSTALL.txt (where the files go); the VST3 bundle carries the licences in Contents/Resources too. The user
# manual (Textura-Plugin-Manual.pdf) goes in when it has been made first (make plugin-manual; TXP_MANUAL=FILE).
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
DEPS=${TXP_DEPS_DIR:-"$ROOT/.local/plugin-deps"}
OFL="$ROOT/assets/fonts/OFL-InterTight.txt"
MANUAL=${TXP_MANUAL:-"$ROOT/build/plugin/pkg/Textura-Plugin-Manual.pdf"}   # (tools/plugin_manual_pdf.sh)

die() { echo "plugin_pack: $*" >&2; exit 1; }

notices() {   # DIR windows|linux: LICENSE.txt, OFL-InterTight.txt, SOURCE.txt, THIRD-PARTY-NOTICES.txt
    mkdir -p "$1"
    cp "$ROOT/LICENSE" "$1/LICENSE.txt"
    cp "$OFL" "$1/OFL-InterTight.txt"
    if [ -f "$MANUAL" ]; then cp "$MANUAL" "$1/"; fi   # (the user manual, when make plugin-manual has made it)
    cat > "$1/SOURCE.txt" <<EOF
Textura is free software under the GNU General Public License, version 3 only (LICENSE.txt). It is based on Frodhes.
Its complete source, the plugin's build files and the exact versions of every dependency (plugin/deps.lock) are at
https://github.com/leonardoMirabal/textura. The plugin is built from plugin/ (docs/PLUGIN.md).
EOF
    n=$1/THIRD-PARTY-NOTICES.txt
    {
        echo "Third-party material in the Textura plugin binaries"
        echo "==================================================="
        echo
        echo "Textura itself: GPL-3.0-only (LICENSE.txt). The binaries also contain the following, each under its"
        echo "own licence, reproduced below from the exact versions built (plugin/deps.lock)."
        echo
        echo "  CLAP (the plugin API headers)                 MIT         https://github.com/free-audio/clap"
        echo "  clap-wrapper (the VST3 layer)                  MIT         https://github.com/free-audio/clap-wrapper"
        echo "    with its copy of {fmt}                       MIT         https://github.com/fmtlib/fmt"
        echo "    and the PreSonus gain-reduction header       public domain"
        echo "  VST3 SDK                                       MIT         https://github.com/steinbergmedia/vst3sdk"
        [ "$2" = windows ] &&
        echo "  The Microsoft C/C++ runtime, linked statically  Microsoft's redistribution terms (no notice needed)"
        [ "$2" = linux ] &&
        echo "  libstdc++ and libgcc (GCC runtime)             GPL-3.0 WITH GCC-exception-3.1"
        echo "  Inter Tight (the window's font)                SIL OFL 1.1 (OFL-InterTight.txt, and below)"
        echo
        echo "VST is a trademark of Steinberg Media Technologies GmbH. Textura is not made or endorsed by it."
        sect() { echo; echo "------------------------------------------------------------------------------"; echo "$1";
                 echo "------------------------------------------------------------------------------"; echo; cat "$2"; }
        sect "CLAP" "$DEPS/clap/LICENSE"
        sect "clap-wrapper" "$DEPS/clap-wrapper/LICENSE"
        sect "{fmt} (clap-wrapper/libs/fmt; its licence, from format.h)" /dev/stdin <<'FMT'
Copyright (c) 2012 - present, Victor Zverovich and {fmt} contributors

Permission is hereby granted, free of charge, to any person obtaining
a copy of this software and associated documentation files (the
"Software"), to deal in the Software without restriction, including
without limitation the rights to use, copy, modify, merge, publish,
distribute, sublicense, and/or sell copies of the Software, and to
permit persons to whom the Software is furnished to do so, subject to
the following conditions:

The above copyright notice and this permission notice shall be
included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

--- Optional exception to the license ---

As an exception, if, as a result of your compiling your source code, portions
of this Software are embedded into a machine-executable object form of such
source code, you may redistribute such embedded portions in such object form
without including the above copyright and permission notices.
FMT
        sect "VST3 SDK" "$DEPS/vst3sdk/LICENSE.txt"
        sect "Inter Tight (the window's font)" "$OFL"
    } > "$n"
    grep -q "MIT License" "$n" || die "notices: a licence text is missing"
}

zipdir() {   # PARENT NAME ZIP: a zip of PARENT/NAME, with fixed times (the same bundles give the same zip)
    find "$1/$2" -exec touch -t 202601010000 {} +
    rm -f "$3"
    (cd "$1" && cmake -E tar cf "$3" --format=zip "$2")
}

windows() {
    B=${1:-$ROOT/build/plugin/win-x86_64}/bundles; O=${2:-$ROOT/build/plugin/pkg}
    [ -f "$B/CLAP/Textura.clap" ] && [ -f "$B/VST3/Textura.vst3/Contents/x86_64-win/Textura.vst3" ] ||
        die "no Windows bundles in $B (tools/plugin_win.sh build)"
    mkdir -p "$O"; O=$(cd "$O" && pwd)
    S=$O/_win; rm -rf "$S"; mkdir -p "$S/Textura/CLAP" "$S/Textura/VST3"
    notices "$S/Textura" windows
    cp "$B/CLAP/Textura.clap" "$S/Textura/CLAP/"
    mkdir -p "$S/Textura/VST3/Textura.vst3/Contents/x86_64-win" "$S/Textura/VST3/Textura.vst3/Contents/Resources"
    cp "$B/VST3/Textura.vst3/Contents/x86_64-win/Textura.vst3" "$S/Textura/VST3/Textura.vst3/Contents/x86_64-win/"
    cp "$S/Textura/LICENSE.txt" "$S/Textura/OFL-InterTight.txt" "$S/Textura/THIRD-PARTY-NOTICES.txt" \
        "$S/Textura/VST3/Textura.vst3/Contents/Resources/"
    cat > "$S/Textura/INSTALL.txt" <<'EOF'
Textura plugin for Windows 10 or newer (x64)

Copy the formats your DAW uses (no installer, nothing else needed):
  CLAP\Textura.clap    into  C:\Program Files\Common Files\CLAP\
                       (or, for you only: %LOCALAPPDATA%\Programs\Common\CLAP\)
  VST3\Textura.vst3    the whole folder, into  C:\Program Files\Common Files\VST3\
Then rescan in your DAW. To remove the plugin, delete what you copied.
The files are not code-signed: if Windows marks them as downloaded, right-click each, Properties, Unblock.
EOF
    zipdir "$S" Textura "$O/textura-plugin-windows-x64.zip"
    rm -rf "$S"
    ls -l "$O/textura-plugin-windows-x64.zip"
}

linux() {
    B=${1:-$ROOT/build/plugin/linux-x86_64}/bundles; O=${2:-$ROOT/build/plugin/pkg}
    A=""
    for a in x86_64 aarch64; do if [ -f "$B/Textura.vst3/Contents/$a-linux/Textura.so" ]; then A=$a; fi; done
    [ -f "$B/Textura.clap" ] && [ -n "$A" ] || die "no Linux bundles in $B (tools/plugin_linux.sh build)"
    mkdir -p "$O"; O=$(cd "$O" && pwd)
    S=$O/_linux-$A; rm -rf "$S"; mkdir -p "$S/Textura"
    notices "$S/Textura" linux
    cp "$B/Textura.clap" "$S/Textura/"
    mkdir -p "$S/Textura/Textura.vst3/Contents/$A-linux" "$S/Textura/Textura.vst3/Contents/Resources"
    cp "$B/Textura.vst3/Contents/$A-linux/Textura.so" "$S/Textura/Textura.vst3/Contents/$A-linux/"
    cp "$S/Textura/LICENSE.txt" "$S/Textura/OFL-InterTight.txt" "$S/Textura/THIRD-PARTY-NOTICES.txt" \
        "$S/Textura/Textura.vst3/Contents/Resources/"
    cat > "$S/Textura/INSTALL.txt" <<EOF
Textura plugin for Linux $A (glibc 2.31 or newer: Ubuntu 20.04, Debian 11, Fedora 32 and later; X11 or XWayland)

  cp Textura.clap ~/.clap/            (or /usr/lib/clap/ for everyone)
  cp -R Textura.vst3 ~/.vst3/         (or /usr/lib/vst3/ for everyone)
Then rescan in your DAW. To remove the plugin, delete what you copied.
EOF
    zipdir "$S" Textura "$O/textura-plugin-linux-$A.zip"
    rm -rf "$S"
    ls -l "$O/textura-plugin-linux-$A.zip"
}

case ${1:-} in
windows) shift; windows "$@" ;;
linux) shift; linux "$@" ;;
*) sed -n '3,11p' "$0"; exit 2 ;;
esac
