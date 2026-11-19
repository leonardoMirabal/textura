#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# The VST3 SDK's own conformance test (its "validator" hosting sample), built from the pinned SDK checkout with the
# command line tools only (the SDK's CMake wants Xcode on macOS), then run on a Textura.vst3:
#   tools/plugin_vst3_validator.sh [Textura.vst3 [OUT]]   (default build/plugin/bundles/Textura.vst3, build/vst3-validator)
# The binary lands in OUT/validator (built once). macOS, and Linux (tools/plugin_linux.sh validate runs it in its
# container with CXX=clang++-12). Exit: the validator's (0 = pass).
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SDK="${TXP_DEPS_DIR:-$ROOT/.local/plugin-deps}/vst3sdk"
V="${1:-$ROOT/build/plugin/bundles/Textura.vst3}"
O="${2:-$ROOT/build/vst3-validator}"
case "$(uname -s)" in Linux) OS=linux; CXX="${CXX:-clang++-12}" ;; *) OS=mac; CXX="${CXX:-clang++}" ;; esac
[ -d "$SDK/public.sdk" ] || { echo "no VST3 SDK in $SDK (make plugin-deps)" >&2; exit 2; }
if [ ! -x "$O/validator" ]; then
    mkdir -p "$O"
    # the validator's own sources (its CMakeLists), the hosting classes, base and the interfaces
    python3 - "$SDK" > "$O/sources.txt" <<'EOF'
import glob, os, re, sys
sdk = sys.argv[1]
s = open(os.path.join(sdk, "public.sdk/samples/vst-hosting/validator/CMakeLists.txt")).read()
fs = [f.replace("${SDK_ROOT}/", "") for f in re.findall(r"(\S+\.cpp)", s)]
fs = ["public.sdk/samples/vst-hosting/validator/" + f if f.startswith("source/") else f for f in fs]
os.chdir(sdk)
for g in ["public.sdk/source/vst/hosting/*.cpp", "base/source/*.cpp", "base/thread/source/*.cpp", "pluginterfaces/base/*.cpp",
          "public.sdk/source/vst/utility/stringconvert.cpp", "public.sdk/source/common/commonstringconvert.cpp",
          "public.sdk/source/vst/vstinitiids.cpp", "public.sdk/source/vst/vstpresetfile.cpp"]:
    fs += glob.glob(g)
print("\n".join(sorted(f for f in set(fs) if not re.search(r"module_(linux|win32|mac)", f))))
EOF
    if [ $OS = linux ]; then
        (cd "$SDK" && nice -n 15 "$CXX" -std=c++17 -O1 -w -DRELEASE=1 -I. $(cat "$O/sources.txt") \
            public.sdk/source/vst/hosting/module_linux.cpp public.sdk/source/common/threadchecker_linux.cpp \
            public.sdk/source/common/commoniids.cpp \
            -o "$O/validator" -ldl -lpthread -lstdc++fs)
    else
        (cd "$SDK" && nice -n 15 "$CXX" -std=c++17 -O1 -w -DRELEASE=1 -I. $(cat "$O/sources.txt") \
            -x objective-c++ -fobjc-arc public.sdk/source/vst/hosting/module_mac.mm public.sdk/source/common/threadchecker_mac.mm \
            -framework Cocoa -o "$O/validator")
    fi
    "$O/validator" -selftest > "$O/selftest.log" 2>&1 || { echo "the validator's selftest failed ($O/selftest.log)" >&2; exit 1; }
fi
"$O/validator" "$V" > "$O/report.txt" 2>&1 && rc=0 || rc=$?
grep -E "^Result|ERROR" "$O/report.txt" || true
echo "(full report: $O/report.txt)"
exit $rc
