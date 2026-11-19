# SPDX-License-Identifier: GPL-3.0-only
# The Windows build's headless check without a Windows machine (tools/plugin_win.sh test; as Frodhes'): wine (64-bit)
# on Debian bookworm (12), amd64, with python3, numpy / scipy and gcc to run tools/plugin_gate.py there (the host build
# compiled natively, the plugin's test host txp_clap_render.exe and Textura.clap under wine). wine is not Windows:
# a pass is a smoke test, not a release check.
#   docker build --platform linux/amd64 -t textura-wine:bookworm-amd64 -f plugin/tests/docker/wine.Dockerfile plugin/tests/docker
FROM debian:bookworm@sha256:d74df7598108a173a621eda4d58a2adbf850b2d9a9d81bc2cca1a6c21174a880
RUN apt-get update && apt-get install -y --no-install-recommends wine wine64 xvfb xauth ca-certificates \
        python3 python3-numpy python3-scipy gcc libc6-dev \
    && rm -rf /var/lib/apt/lists/*
ENV WINEDEBUG=-all WINEPREFIX=/wine
RUN wine wineboot --init >/dev/null 2>&1 || true; wineserver -w || true
