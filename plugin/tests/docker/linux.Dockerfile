# SPDX-License-Identifier: GPL-3.0-only
# The Linux plugin's build and test machine (tools/plugin_linux.sh; as Frodhes'): Ubuntu 20.04 (glibc 2.31: the
# oldest the binaries need), clang 12 + lld, CMake 3.28.3, libX11 headers, Xvfb, and numpy / scipy for the gate's
# rates check.
#   docker build --platform linux/amd64 -t textura-plugin-linux:focal-amd64 -f plugin/tests/docker/linux.Dockerfile plugin/tests/docker
FROM ubuntu:20.04@sha256:8feb4d8ca5354def3d8fce243717141ce31e2c428701f6682bd2fafe15388214
ENV DEBIAN_FRONTEND=noninteractive
# TXP_APT_MIRROR (tools/plugin_linux.sh): another Ubuntu mirror for archive / security.ubuntu.com, when those fail
ARG APT_MIRROR=
RUN if [ -n "$APT_MIRROR" ]; then for f in /etc/apt/sources.list /etc/apt/sources.list.d/ubuntu.sources; do \
        [ -f "$f" ] && sed -i -E "s#https?://(archive|security)\.ubuntu\.com/ubuntu/?#$APT_MIRROR#g" "$f"; done; true; fi
RUN apt-get update && apt-get install -y --no-install-recommends clang-12 lld-12 llvm-12 ninja-build python3 \
        python3-pip python3-numpy python3-scipy git libx11-dev xvfb xauth ca-certificates file \
    && rm -rf /var/lib/apt/lists/* \
    && pip3 install --no-cache-dir cmake==3.28.3
