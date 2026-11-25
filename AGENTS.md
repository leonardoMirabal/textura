# Textura: the operating manual

Textura is a physically modelled texture synthesizer for the M-VAVE FM-1 (JieLi AC791N, pi32v2 core, no FPU,
240 x 240 screen, 27 touch keys F3..G5, buttons, encoders, USB-MIDI, TRS MIDI in, USB audio, headphone jack). The
sounds are computed, never sampled. The repository is `leonardoMirabal/textura`. `CLAUDE.md` is the short
version. When this file and the code disagree, the code wins: fix the file.

Textura is based on Frodhes (github.com/leonardoMirabal/frodhes, same author): its firmware platform and tools were
copied in, and many rules here were learned there on the same hardware. The repository is self-contained: nothing
builds, installs, tests or calibrates from a Frodhes checkout. Each section says what exists (**done**), what is to be ported from Frodhes (**port**), and what is
still to be designed (**todo**).

## 1. Ground rules

- **Integers only in `engines/`.** No `float` / `double`, no 64-bit `/` or `%` (no libgcc on the device). 64-bit
  multiply is fine, but prefer 32-bit products in inner loops: felucca-bsp reports a register-corruption fault with
  64-bit multiplies next to 16-bit loads. The host build will not catch float; `txtest` greps for it.
- **Engines are instances**: all state in a struct, no globals, so up to four engines can play at once (layers).
- **The audio budget:** 256 frames at 44.1 kHz = 5805 us per block on Frodhes' platform. The whole interrupt stays
  under the 4500 us gate (target 4200, USB at 48 kHz): the platform takes ~784 us, the effects their reserve
  (880 us: the reverb, the grain delay under its dynamic cap), and the sources share the rest as **one pool**
  (~1700 us planned on the host; on the device what the measured interrupt leaves, ~2500 us). The governor hands the
  pool out by cost (lane inserts count against their lane); there is no per-engine cap. Sanity limits: a lane alone,
  at any setting, stays inside the planned pool over 8 blocks, and no single block of a lane goes over half of it
  (~870 us), since a spike passes the governor. `make tx-budget` reports all of it. Host numbers are estimates until
  measured on the FM-1.
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
- **Self-contained**: no tool, target, test or doc step may read a sibling repository (`../frodhes` included). Code
  we take from Frodhes is copied in with its header; third-party tooling (the JieLi toolchain and SDK files,
  Emscripten, Chromium, the fonts) is fetched by `make setup` from its own public source into `.local/`, pinned by
  sha256 where the source allows it.
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
- Setup: `make setup` fetches the tooling into `.local/` (venv, JieLi toolchain, 3 SDK files, Emscripten, Chromium,
  fonts); Docker runs the toolchain on macOS. Firmware: `make fm1-app` (the app image, `build/fm1/textura-app.fwsc`),
  `make fm1-bench`; the platform sources are in `firmware/` (Frodhes v0.9.5's, headers kept).
- On USB the FM-1 reads **Textura FM-1** (the device, its MIDI port and its audio interface; maker "Textura"). The
  "FM-1" keeps the web installers working: thefirmwarehub.com probes only ports named like an FM-1, and a unit on
  Textura must always take another firmware from there. It has its own
  product id (the strings in `firmware/src/usb.c`, checked by `tools/fm1_bench.py` and `tests/ports_check.py`; `USB_PID`; interim 1209:000A, a
  pid.codes test PID for in-house tests only, until Textura's own is granted). The update loader keeps 1209:0002.
  Our tools take a "Textura" port first and never a "Frodhes" one beside it (`tools/tx_ports.py`: `fm1_timing.py`,
  `hil_jack.py`); a "Frodhes" port may be Frodhes' own FM-1. Install with `tools/tx_install.py PACKAGE.fwsc`; a unit
  still on the firmware named "Frodhes" takes `--port Frodhes` once. With two FM-1s connected, pin the unit by its USB
  location: `tools/tx_install.py --info` lists every FM-1 with its location, then `--location 0x...` (every update
  loader is named "Felucca Update"; the installer follows the pinned unit into its loader and refuses, exit 7, when it
  cannot tell the units apart).

## 4. The change procedure

1. **Physics first.** An engine starts from its private research note (`.private/research/notes/`).
2. **Implement** in integer C on the shared cores; parameters are the engine's own, from its physics; PARTICLES and
   level are shared by all engines.
3. **Check:** `txtest` (zero when idle, no overflow, particle cap, determinism, no float), `txbench` (worst case).
4. **Audition:** the engine appears in the browser player; the user plays it and decides (keep / tune / drop).
5. **Commit and push** as soon as it passes: one commit per piece; message = what changed and what was tested (no
   Claude-Session line).

## 5. Testing (port Frodhes' standards as the code grows)

- Pristine is a number: no clicks, no zipper noise, no aliasing, no DC; idle is exact digital zero.
- CPU: the worst-case scene per engine (all keys held, max density) measured 3 times; on the device later with a CC
  query for the longest block (Frodhes' `hil_cpu.py` pattern).
- Byte-identical renders for engines a change must not touch (a sha256 baseline checked in CI).
- Never claim "better" from numbers alone: only listening confirms a sound change.

## 6. Git and CI

- `origin` = leonardoMirabal/textura. No upstream remote: Frodhes' files were copied in, never its history.
- `.private/` is never added; check `git status --short --ignored` before a commit.
- CI, as Frodhes' (`.github/workflows/`): `ci.yml` on every push to main and PR. A Markdown-only PR runs only the
  guard. Otherwise the host tests run on arm64 Linux in three parts (engines: `tx-test` + `tx-libcalls`;
  `tx-pristine`; `fw-test`), each after the guard. The job `host` is the required check. `firmware` builds
  `make fm1-app` with the toolchain and SDK fetched as `make setup` does. `web` runs `make fw-web-check` in
  Playwright's image. A failure cancels the run. `release.yml`: a `textura-vX.Y.Z` tag passes the CI gate
  (`tools/ci_gate.sh`; else the tests run there), builds `make fm1-app REL=X.Y.Z` in CI and publishes a full
  release with `textura-app-X.Y.Z.fwsc` and `.json`. Notes come from `docs/releases/<tag>.md` when it exists. The
  emulator's GitHub Pages deploy is prepared there but off: only with the repository variable `PAGES_ENABLED` set to
  `true` does the tag also build and check the emulator (`web-dist`), keep the release a draft, put the package beside
  the page for its installer (`tools/web_firmware.sh`), deploy to gh-pages (`tools/deploy_pages.sh`), wait for it to
  be live (`tools/pages_wait.sh`) and then publish. `ci.yml` never deploys.
  `runner-retry.yml` reruns jobs GitHub never started (`tools/runner_retry.py`). `fw-c1map` is not in CI: it needs
  macOS and the FM-1 image together.

## 7. Working with the user

- Wants work committed and pushed as soon as it is ready; research kept private.
- One agent at a time; agents never spawn sub-agents and write only their own files.
- Wants to hear each engine as soon as it is ready, then decide.
- Writes short messages, sometimes with typos; dislikes over-explanation. Reports are plain, concrete and short.
- The FM-1 may be busy with Frodhes: ask before using it. Confirm before outward-facing actions.

## 8. Hardware-in-the-loop (the installed FM-1)

`make hil` (`tools/hil_suite.py`) tests the unit after every install; ask before using the FM-1. Rig: USB MIDI port
"Textura", USB audio "Textura FM-1" (opened at 44.1 kHz so the recording is the unit's 16-bit samples, never resampled
by the computer), the headphone jack into the MOTU M6 inputs 5/6 (never change the M6's gain; which input carries which
side is found from the signal) and the M6's MIDI OUT into the FM-1's TRS MIDI IN (`--midi usb|trs|both`; debug replies
always come back over USB). Checks (`--only`): `midi` (with `--midi both`: each path's debug reply, an NRPN read back,
NRPN and program change heard), `clicks` (per preset, SPEAKERS and HEADPHONES: digital artefacts only, crackle is the
sound: USB blocks repeated or zeroed bit for bit, spikes on the 256-frame grid, the jack against the USB with the analog
path fitted, late blocks from the CPU debug reply; the crackle rate and the spectrum against the host's render),
`binaural` (far ear lag in the lows 25/29/33 samples, 10 to 20 dB down; AMOUNT 0 = SPEAKERS), `ms` (each case alternated
with the default in one recording; MID must stay), `stereo` (MID >= SIDE + 3 dB, L-R within the host render's span,
correlation >= 0.3), `activity` (a lane's silent window fails only when the host's render of that lane never rests so
long). `--presets all` scans the 110; the host's renders need `make fw-host` (`--no-host` skips them). The exit code
is the number of failures; the unit is restored to its defaults at the end. `--save DIR` keeps the WAVs and a summary,
`--wav DIR` re-analyses them with no hardware, `--dry-run` prints the plan. The analysis is tested on synthetic signals
and the host's FULL STORM by `make hil-test` (`tests/hil_suite_check.py`).
