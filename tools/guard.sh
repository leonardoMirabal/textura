#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Fails if git tracks anything that must never be published: research (.private/), local tooling, audio, papers,
# compiled or vendor binaries, closed SDKs. Run before every commit and in CI.
bad=$(git ls-files | grep -iE \
  '^\.private/|^\.local/|^\.local-backup/|^build/|\.(wav|flac|mp3|ogg|aiff?|pdf|a|o|so|dylib|lib|exe|dll|bin|boot|fwsc|elf|hex|zip|tar|gz|7z)$|(^|/)(sdk|vendor|third_party)/' || true)
if [ -n "$bad" ]; then echo "guard: these files must not be in git:"; echo "$bad"; exit 1; fi
echo "guard: ok ($(git ls-files | wc -l | tr -d ' ') tracked files)"
