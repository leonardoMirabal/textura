#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments
# Download JieLi's Linux toolchain (clang 4.0.1 for pi32v2) and link it as DEST/toolchain.
#   tools/get_toolchain.sh [DEST]      (default: ~/.jieli)
set -e
DEST="${1:-$HOME/.jieli}"
mkdir -p "$DEST"
# to a file first, so a retried download never feeds tar a broken stream; --retry-all-errors: a connection reset
# (curl 35, seen in CI) is retried too, not only timeouts and 5xx
T="$DEST/.toolchain.tar.xz"
curl -fL --retry 5 --retry-all-errors --retry-delay 5 -o "$T" https://pkgman.jieliapp.com/s/linux-toolchain
tar -xJ -C "$DEST" -f "$T"
rm -f "$T"
TC="$(ls -d "$DEST"/jieli-linux-toolchains-* | sort | tail -1)"
ln -sfn "$(basename "$TC")" "$DEST/toolchain"
echo "JIELI_TOOLCHAIN=$DEST/toolchain ($(basename "$TC"))"
