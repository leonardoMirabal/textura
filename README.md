# Textura

A physically modelled **texture synthesizer** for the M-VAVE FM-1. Each engine simulates the physics of a natural
sound source instead of playing samples: drops, rain, streams, fire, sand and small stones, struck and scraped
minerals, ice, sparks, thunder, wind, insects. Every engine plays as a **cloud of sounding particles** from the keys:
tap for a burst, hold for a stream, and shape it with the engine's own physical controls.

Its sister project is [Frodhes](https://github.com/leonardoMirabal/frodhes), a physically modelled Rhodes for the same
hardware. Textura's firmware will be built on the same platform (Dinghy / Felucca), and Frodhes is the reference for
method, tooling and the integer DSP rules.

**Status: early development (2026-10-20).** The engines are being written and auditioned on a computer first, in a
browser player; the firmware comes later.

## Layout

| Folder | What |
| --- | --- |
| `engines/` | The engines and their shared DSP cores: integer C, the same code the device will run |
| `bench/` | Host tools: render scripts to WAV, checks, CPU bench |
| `audition/` | The browser player for trying the engines (keys, MIDI, CPU meter) |
| `tools/` | Helper scripts |

## Licence

Open source under the GPL-3.0 ([LICENSE](LICENSE)).
