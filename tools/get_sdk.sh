#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Fetch the 3 files of the JieLi AC79 SDK (Apache-2.0) the build needs, into DEST (default .local/fw-AC79_AIoT_SDK),
# and check them against pinned sha256 values (Textura: the checks here, from Frodhes' tools/build.py).
set -e
DEST="${1:-.local/fw-AC79_AIoT_SDK}"
B=https://gitee.com/Jieli-Tech/fw-AC79_AIoT_SDK/raw/AC79NN_SDK_V1.2.1_2023-12-13/cpu/wl82/tools
D="$DEST/cpu/wl82/tools"
mkdir -p "$D/cfg"
for f in uboot.boot cfg_tool.bin cfg/eq_cfg_hw.bin; do
    [ -f "$D/$f" ] || curl -fsSL --retry 5 --retry-all-errors --retry-delay 5 -o "$D/$f" "$B/$f"
done
(cd "$D" && shasum -a 256 -c - <<'SUMS'
4e3b4c220dc96641cb5a723f41e68ce41d5261ae9434bb33fbd7f2c59976ded4  uboot.boot
276579954f076886a6a7694f65dc71c034a63a2c204b76749065c0ac7b010d1b  cfg_tool.bin
41167491bffed4651750719c973d2758adeb9021a5670d02d6a53c85ed80ea7d  cfg/eq_cfg_hw.bin
SUMS
)
echo "AC79_SDK=$DEST"
