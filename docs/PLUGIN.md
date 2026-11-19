# Textura as a desktop plugin

The FM-1 firmware as a CLAP, VST3 and Audio Unit (v2) instrument for macOS; the same tree builds a CLAP and VST3
instrument for Windows and Linux (not built or tested yet). The plugin is the app itself, `firmware/src/textura.c`
compiled over the browser emulator's platform (`web/web_hal.c`), so it plays what the FM-1 plays: the same sounds,
parameters, presets and scenes. A desktop has no CPU limit like the FM-1's, so the plugin is the HQ build: the CPU
governor never thins a lane and the grain delay plays every grain (`LN_HQ` in `firmware/src/lanes.c`).

The plugin has up to 16 lanes where the FM-1 has 4: the build option `TX_MIX_SLOTS` (`engines/tx_core_mix.h`,
default 4; `plugin/cmake/slots.cmake` sets `TXP_LANES 16`). The FM-1 and the browser builds keep 4 lanes, and their
binaries do not change.

No window of its own yet. Hosts show their generic parameter view.

## Build

Needs Xcode's command line tools and CMake 3.21 or newer (Ninja is used when installed). The third-party sources
(CLAP, clap-wrapper, the VST3 SDK, AudioUnitSDK) and the test tools (clap-validator, pluginval) download into the
project (`.local/plugin-deps`, never committed), pinned by commit or checksum and checked at configure time
(`plugin/deps.lock`).

    make plugin-deps          # the pinned sources and test tools
    make plugin               # build/plugin/bundles/Textura.clap, Textura.vst3, Textura.component
    make test-plugin          # the coroutine and tools/plugin_gate.py (below)

The bundles are universal (Apple silicon and Intel, about 9 MB each); `make plugin PLUGIN_ARCH=arm64` in a fresh
build folder builds arm64 only, faster. The architectures are fixed when a build folder is first configured. The
build runs at low priority with 2 jobs (`PLUGIN_J`, `PLUGIN_NICE`). `make plugin` installs nothing.

## Try it in a DAW

Copy the bundle your host uses into your plug-in folder, clear the download flag, and rescan:

    cp -R build/plugin/bundles/Textura.clap      ~/Library/Audio/Plug-Ins/CLAP/
    cp -R build/plugin/bundles/Textura.vst3      ~/Library/Audio/Plug-Ins/VST3/
    cp -R build/plugin/bundles/Textura.component ~/Library/Audio/Plug-Ins/Components/
    xattr -dr com.apple.quarantine ~/Library/Audio/Plug-Ins/*/Textura.*

For the Audio Unit, `killall -9 AudioComponentRegistrar` makes macOS see it at once; `auval -v aumu Txt1 Txtr`
checks it. To remove the plugin, delete the three bundles again.

The plugin appears as **Textura** (vendor Textura), an instrument with a stereo output and a note input (MIDI or
CLAP notes). Play it from a MIDI track as you would play the FM-1 over USB MIDI (docs/MANUAL.md, section 12):

| MIDI | What it reaches |
| --- | --- |
| Channel 1 notes | the selected lane (SELECTED CH) |
| Channel n notes, CCs (n = 1 .. 16) | lane n |
| Channel 5 CCs | the effects, the tempo, the groove and the scale (FX CHANNEL) |
| Channel 1 program change | 0 .. 99 load F001 .. F100, 100 .. 115 load U01 .. U16 |

Each lane's channel is a parameter: OFF or 1 .. 16, lane n on channel n by default (LANE 1 .. 4 CHANNEL as on the
FM-1, LANE 5 .. 16 CHANNEL new). Lanes on one channel layer: its notes and CCs reach all of them. The effects'
channel is FX CHANNEL, 5 by default as on the FM-1: lane 5 is on channel 5 too, and both work there, because the
effects' CCs and the lanes' CCs share no number (MIDI IN: docs/MANUAL.md, section 12).

The host's tempo leads when it gives one (the FM-1's own tempo otherwise).

## Lanes

LANES (1 .. 16, a parameter) is the number of lanes in use. A lane past it is empty: no sound, no engine, no CPU.
Raising LANES adds empty lanes; a SOUND set on lane n (from the host or by CC 20 on its channel) raises it to n.
Lowering it empties the lanes past it: each stops, its cards go back, and its engine rings out and is let go.

A factory preset, a landscape's start or an FM-1 scene fills lanes 1 .. 4 exactly as the FM-1 does and sets LANES to
4. A plugin scene keeps all its lanes.

## Parameters

About 480 host parameters, by the app's own stable ids, so automation stays put across versions. An id is the
scope times 256 plus the index (`firmware/src/params.c`): lanes 1 .. 4 are scopes 0 .. 3, the FM-1's own; lanes
5 .. 16 are scopes 16 .. 27 (ids 0x1000 .. 0x1BFF). The new globals take indices after the FM-1's (LANE 5 .. 16
CHANNEL 68 .. 79, FX CHANNEL 80, LANES 81). No FM-1 id moved, so NRPN numbers and FM-1 scenes stay valid.

- **PRESET**: F001 .. F100 (not automatable: it loads a whole scene, as a program change does).
- **Lane 1 .. 16**: SOUND, HOW MANY, HOW ACTIVE, LEVEL, PAN, CARD 1 .. 8, GROOVE PART, GROOVE CYCLE. A card's meaning
  depends on the lane's sound; its host value is 0 .. 127 onto the card's own range, as its CC. A card the user has
  not set shows its default to the host. An empty lane shows neutral values (LEVEL 100, PAN centre, the rest 0).
  LEVEL is the sound's, as on the FM-1: lanes playing one sound share it.
- **LANES**.
- **Effects**: the grain delay, reverb, compressor, drive and filter, as on their pages.
- **Groove, keys (scale), MIDI IN channels (lanes 1 .. 16, FX CHANNEL), EFFECTS ON LOAD**.
- **LFO 1 .. 8, the envelopes and followers, the matrix (MOD 1 .. 16) and MOD CC**. A modulation target names
  lane 1 .. 4 or ALL LANES, as on the FM-1; ALL LANES reaches lanes 5 .. 16 too.

The FM-1's tempo, clock, screen and status settings are not host parameters.

A change the app makes itself (a CC, a program change, a scene load) goes back to the host as a parameter change.
Set SOUND and a card of the new sound in separate steps: a new sound starts from its own cards, so a card value sent
together with the SOUND change is replaced by the new sound's default (as on the FM-1).

## State

A host project keeps what a user scene keeps (the landscape, each lane's sound, HOW MANY, HOW ACTIVE, level, pan,
mute and cards, the effects, the modulation, the groove, the tempo), the scene loaded last, and the settings that are
parameters (scale, MIDI IN, EFFECTS ON LOAD, PRESETS CHANGE TEMPO). It loads as a scene with its effects. Nothing is
written to disk by the plugin: its user scene bank (U01 .. U16) lives for the session.

The state is version 2: the scene record of this build, 2040 bytes with its lane count and all 16 lanes. A project
saved by the 4-lane plugin (version 1, the FM-1's scene record) still loads: lanes 1 .. 4 as saved, the others
empty, LANES 4.

## Sample rates and latency

The app always runs at 44.1 kHz in 256-frame blocks. At 44.1 kHz the output is the app's own; 48 kHz and the
multiples of both (88.2, 96, 176.4, 192, 352.8, 384 kHz) go through an oversampler (a 160/147 polyphase stage, then
half-bands); any other rate from 1 to 1536 kHz through a windowed sinc. The latency is constant per rate: one app
block (256 frames, 5.8 ms) plus the oversampler's delay, 258 frames at 44.1 kHz, 325 at 48 kHz. An event acts from
the start of the app block that holds it, as every event does on the FM-1 (notes are not placed inside a block).

Up to 16 instances play in one process (each holds one of 16 independent copies of the app; the bundle's static data
is about 15 MB for the 16); more play silence and say so in the host's log.

## Tests

`make test-plugin` runs `tools/plugin_gate.py`:

- **equal**: at 44.1 kHz, in the test time (`TEXTURA_PLUGIN_TEST=ms`: the host build's order of events per
  millisecond), the plugin's render of a MIDI script is the host build's (`tests/host.c`, built with `LN_HQ`) sample
  for sample: a lane stopped, notes and CCs on every lane and channel 5 with two presets, a heavy 4-lane scene, and
  every factory preset (`--quick` skips the presets). The plugin has 16 lanes, the host build 4, so this also shows
  that 4-lane content plays in a 16-lane instance exactly as on the FM-1. `lanes16`: the plugin against the 16-lane
  host build (`textura-host-hq16`): sixteen lanes given sounds by MIDI, each on its own channel, notes on all
  sixteen channels, an NRPN on lane 12, the effects on channel 5, then a factory preset over them (lanes 5 .. 16
  emptied, LANES back to 4).
- **hq**: the host build with `LN_HQ` plays the heavy scene with the governor's pool unlimited and no lane cut.
- **rates**: at 48, 96 and 88.2 kHz (the oversampler) and 32 kHz (the sinc) the plugin's render equals the host
  build's taken to that rate offline: the residual under -90 dB in the band both resamplers keep.
- **state**: a render's state saved, loaded into a second instance: every parameter reads back the same, and it
  saves the same again, with 4 lanes and with 16 lanes playing; a 4-lane plugin's state (version 1,
  `plugin/tests/fixtures/state_v1.json`) loads into the 16-lane instance (lanes, effects, records, settings); also
  for instances never activated, every parameter set at random by a flush.
- **cpu**: the time per 512-frame process call at 48 kHz for heavy 4-lane scenes and for 16 heavy lanes (the heavy
  presets' lanes in one state), printed, not judged. On an Apple M-series machine: 4 heavy lanes about 110 .. 130 us
  a call on average (1.0 .. 1.2 % of the 10.7 ms the call has), 16 heavy lanes about 280 us (2.6 %), the worst call
  under 400 us (3.7 %).

The headless host is `build/plugin/txp_clap_render PLUGIN.clap SCRIPT OUT.wav [--rate HZ] [--block N]
[--state-check] [--fuzz SEED] [--inactive] [--cpu-from MS] [--state-in FILE] [--state-out FILE]`; it plays a
host-build script's MIDI lines; `--state-in` loads a state before the activation, `--state-out` saves one after the
render.

Validators (after `make plugin-deps`):

    .local/plugin-deps/clap-validator/binaries/clap-validator validate build/plugin/bundles/Textura.clap
    sh tools/plugin_vst3_validator.sh                        # the VST3 SDK's own validator (built from the SDK)
    .local/plugin-deps/pluginval/pluginval.app/Contents/MacOS/pluginval --strictness-level 10 --skip-gui-tests \
        --validate build/plugin/bundles/Textura.vst3
    auval -strict -v aumu Txt1 Txtr                          # the Audio Unit, once installed (above)

All four pass with 16 lanes: clap-validator (35 passed, 9 skipped for extensions the plugin does not have), the VST3 validator
(47 tests), pluginval at strictness 10 (the VST3 and the Audio Unit, without its editor tests: there is no window
yet). `auval -strict` passes with one warning, "Preset name is not retained in retrieved class data": it comes from
clap-wrapper's Audio Unit, which names the class data from its own field.

## How it is built

| File | What it does |
| --- | --- |
| `plugin/src/engine_unit.c` | one copy of the app: `firmware/src/textura.c` over `web/web_hal.c`, compiled 16 times (`-DTXP_SLOT=0..15`, `-DTX_MIX_SLOTS=16u`); every symbol static but its table `txp_slot_<n>`, which `plugin/cmake/nm_check.cmake` checks |
| `plugin/src/slot_bridge.h` | a slot's glue: the time (one step a millisecond), the event queue, the audio FIFO, the host parameters, the state |
| `plugin/src/bridge.c` | an instance: the slot it holds, the host's frames onto the app's, the oversampler (`txp_os.h`) and the sinc |
| `plugin/src/textura_clap.c` | the CLAP plugin; clap-wrapper makes the VST3 and the Audio Unit from it |
| `plugin/src/coro/` | the coroutine the app's main loop runs in (arm64 and x86_64 assembly; Windows: Fibers) |
| `plugin/compat/` | `emscripten.h` for `web_hal.c`, and the Windows shims |

The engines (`engines/`) are linked once and shared by the 16 copies: they keep no state outside the instances the
app hands them.
