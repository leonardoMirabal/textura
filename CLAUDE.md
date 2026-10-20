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
- Audio: 256-frame blocks at 44.1 kHz = 5805 us (Frodhes' platform). Every engine is an instance (no globals) so four
  can play at once later; each engine's worst case should stay under ~18 % of the block.
- No samples: sounds are computed. Reference recordings are for measurement only, never committed.
- GPL-3.0 open source. Reused code keeps its original file header; no copyright notice lists in the docs.
- **Physics plans, research, findings and downloaded papers are private, always**: they live in `.private/` (git-
  ignored, never pushed; its own local-only git for history). Public files never cite papers or link into it.
- **`.private/PROGRESS.md` is the progress log**: read it first; add an entry whenever work moves.
- Push code as soon as a piece is reviewed and its checks pass. Never push `.private/`.
- One research / implementation agent at a time; agents write their own files and never spawn sub-agents.
- The FM-1 may be busy with Frodhes: ask before using it. Confirm before any outward-facing action.
