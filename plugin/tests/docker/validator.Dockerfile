# SPDX-License-Identifier: GPL-3.0-only
# clap-validator's Linux build (plugin/deps.lock) is linked against Ubuntu 22.04's glibc (2.35), newer than the build
# image's 2.31: it runs here, Ubuntu 24.04 with the one library the plugin needs (libX11). tools/plugin_linux.sh validate.
#   docker build --platform linux/amd64 -t textura-clap-validator:noble-amd64 -f plugin/tests/docker/validator.Dockerfile plugin/tests/docker
FROM ubuntu:24.04@sha256:73b999393d07f2bf65a40373778a44dc9a6d54ad7718d29247d8ff9413c4d6a8
# TXP_APT_MIRROR (tools/plugin_linux.sh): another Ubuntu mirror for archive / security.ubuntu.com, when those fail
ARG APT_MIRROR=
RUN if [ -n "$APT_MIRROR" ]; then for f in /etc/apt/sources.list /etc/apt/sources.list.d/ubuntu.sources; do \
        [ -f "$f" ] && sed -i -E "s#https?://(archive|security)\.ubuntu\.com/ubuntu/?#$APT_MIRROR#g" "$f"; done; true; fi
RUN apt-get update && apt-get install -y --no-install-recommends libx11-6 \
    && rm -rf /var/lib/apt/lists/*
