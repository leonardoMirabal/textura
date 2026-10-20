# Textura: the operating manual

Textura is a physically modelled texture synthesizer for the M-VAVE FM-1 (JieLi AC791N, pi32v2 core, no FPU,
240 x 240 screen, 27 touch keys F3..G5, buttons, encoders, USB-MIDI, TRS MIDI in, USB audio, headphone jack). The
sounds are computed, never sampled. The repository is `leonardoMirabal/textura` (private). `CLAUDE.md` is the short
version. When this file and the code disagree, the code wins: fix the file.

Many rules here come from the sister project Frodhes (`../frodhes`, its AGENTS.md), where they were learned on the
same hardware. Each section says what exists (**done**), what is to be ported from Frodhes (**port**), and what is
still to be designed (**todo**).

## 1. Ground rules

- **Integers only in `engines/`.** No `float` / `double`, no 64-bit `/` or `%` (no libgcc on the device). 64-bit
  multiply is fine, but prefer 32-bit products in inner loops: felucca-bsp reports a register-corruption fault with
  64-bit multiplies next to 16-bit loads. The host build will not catch float; `txtest` greps for it.
- **Engines are instances**: all state in a struct, no globals, so up to four engines can play at once (layers).
- **The audio budget:** 256 frames at 44.1 kHz = 5805 us per block on Frodhes' platform. Design target for one
  engine's worst case: under ~18 % of the block at its default PARTICLES limit, so four layers fit the 75 % gate.
  Host numbers are estimates until measured on the FM-1.
- **No samples, ever.** Engines are physics models. Recordings are measured for numbers only; commit only numbers.
- **Licence:** GPL-3.0 open source. New files start with `SPDX-License-Identifier: GPL-3.0-only`. Reused code
  keeps its original file header (that's all; no notice lists in the docs). GPL-3.0-compatible code (Frodhes,
  Felucca, Dinghy, SLOOP, SDT, STK, Mutable Instruments) may be reused. Unlicensed code is ideas only.
- **Only our own work goes into git history, ever** (2026-10-20: an earlier bootstrap committed FM1 Open's tree,
  closed SDK binaries included, and the whole history had to be rewritten). Never base the repo on, merge, or fetch
  into it a third-party tree you have not audited; never commit binaries, SDKs or vendor folders. Third-party code
  enters only as the individual source files we reuse, with their original header, under a GPL-compatible licence.
  Before importing any outside history (e.g. Frodhes' tree for the firmware stage), audit every path in it first.
  `tools/guard.sh` (also in CI) rejects private files, audio, papers, binaries and sdk / vendor / third_party
  folders: run it before every commit.
- **Research is private**: papers, notes, plans, reports live in `.private/` (git-ignored, never pushed). The
  README and public files never cite papers.
- **Keep everything inside the project folder**: tools in `.local/`, outputs in `build/`, research in `.private/`.
  Never delete or recreate the project folder. Before any history rewrite, bundle to `.local-backup/`.
- **Every change is tested before it counts as done**; the commit message says what was tested and what was not.

## 2. Repository map

| Path | What | In git |
| --- | --- | --- |
| `engines/` | Engine API (`tx_engine.h`), shared cores (`tx_core_*`), engines (`tx_eng_*`) | yes |
| `bench/` | `txrender` (script -> WAV), `txtest` (checks), `txbench` (CPU, calibrated to the FM-1) | yes |
| `audition/` | Browser player (Emscripten): engine selector, per-engine sliders, keys + Web MIDI, CPU meter | yes |
| `tools/` | `doc2md.sh` (research documents to text) | yes |
| `build/` | All outputs (WAVs, reports, the built player) | no |
| `.local/` | Tooling: venv, emsdk, reference material | no |
| `.private/` | Research, plans, reports, the progress log | **never** |

## 3. Build and run (done in wave 0; see `audition/README.md` and the Makefile)

- Host tools and checks: the `tx-*` make targets (test, bench, render, player).
- Browser player: built to `build/audition/`, served with `python3 -m http.server` on localhost (Web MIDI works on
  localhost).
- Firmware: **todo** (stage S4: port onto Frodhes' platform; build on macOS through Docker as Frodhes does).

## 4. The change procedure

1. **Physics first.** An engine starts from its private research note (`.private/research/notes/`).
2. **Implement** in integer C on the shared cores; parameters are the engine's own, from its physics; PARTICLES and
   level are shared by all engines.
3. **Check:** `txtest` (zero when idle, no overflow, particle cap, determinism, no float), `txbench` (worst case).
4. **Audition:** the engine appears in the browser player; the user plays it and decides (keep / tune / drop).
5. **Commit and push** as soon as it passes: one commit per piece; message = what changed and what was tested.

## 5. Testing (port Frodhes' standards as the code grows)

- Pristine is a number: no clicks, no zipper noise, no aliasing, no DC; idle is exact digital zero.
- CPU: the worst-case scene per engine (all keys held, max density) measured 3 times; on the device later with a CC
  query for the longest block (Frodhes' `hil_cpu.py` pattern).
- Byte-identical renders for engines a change must not touch (a sha256 baseline checked in CI).
- Never claim "better" from numbers alone: only listening confirms a sound change.

## 6. Git and CI

- `origin` = leonardoMirabal/textura (private). No upstream remote yet (Frodhes' tree comes in at stage S4).
- `.private/` is never added; check `git status --short --ignored` before a commit.
- CI (`.github/workflows/ci.yml`, done): the guard job fails if anything private, audio or a PDF is tracked. Add the
  host checks (`txtest`) to CI once they exist (port).

## 7. Working with the user

- Wants work committed and pushed as soon as it is ready; research kept private.
- One agent at a time; agents never spawn sub-agents and write only their own files.
- Wants to hear each engine as soon as it is ready, then decide.
- Writes short messages, sometimes with typos; dislikes over-explanation. Reports are plain, concrete and short.
- The FM-1 may be busy with Frodhes: ask before using it. Confirm before outward-facing actions.
