# Textura

Physically modelled texture synthesizer for the M-VAVE FM-1 (JieLi AC791N, pi32v2, no FPU): water, rain, fire, sand
and stones, minerals, ice, sparks, thunder, wind, insects, each played as a particle cloud from the keys.
**`AGENTS.md` is the operating manual.** The sister project `../frodhes` is the reference for method, tooling and
the firmware platform.

- **Base (decided 2026-10-19):** an own GPL-3.0 firmware from Frodhes' tree (Dinghy / Felucca platform), no
  Bluetooth. FM1 Open was dropped (its closed library gives a custom engine no MIDI). The firmware port is a later
  stage; today the engines are developed and auditioned on the host (`engines/`, `bench/`, `audition/`).
- Integers only in `engines/`: no float/double, no 64-bit `/` or `%`. Q15 levels, 32-bit phases; prefer 32-bit
  products in inner loops (a 64-bit multiply fault is reported on this chip).
- Audio: 256-frame blocks at 44.1 kHz = 5805 us (Frodhes' platform). Every engine is an instance (no globals) so
  several can play at once. CPU: the sources share one pool (~1700 us planned on the host, ~2500 on the device), the
  governor hands it out by cost; no per-engine cap, but no lane over the pool in 8 blocks nor over half of it in one
  block. `make tx-budget` reports it.
- No samples: sounds are computed. Reference recordings are for measurement only, never committed.
- GPL-3.0 open source. Reused code keeps its original file header; no copyright notice lists in the docs.
- **Physics plans, research, findings and downloaded papers are private, always**: they live in `.private/` (git-
  ignored, never pushed; its own local-only git for history). Public files never cite papers or link into it.
- **`.private/PROGRESS.md` is the progress log**: read it first; add an entry whenever work moves.
- Push code as soon as a piece is reviewed and its checks pass. Never push `.private/`.
- **Only our own work in git history.** No third-party trees, SDKs, binaries or vendor folders, ever; audit any outside
  history before importing it. Run `tools/guard.sh` before every commit (CI runs it too).
- One research / implementation agent at a time; agents write their own files and never spawn sub-agents.
- The FM-1 may be busy with Frodhes: ask before using it. Confirm before any outward-facing action.
- `../frodhes` is read-only for us: read it and copy files out, never write, build, stage or commit in it.
- Commit messages carry no Claude-Session line.
