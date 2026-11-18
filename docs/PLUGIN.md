# Textura as a desktop plugin

The FM-1 firmware as a CLAP, VST3 and Audio Unit (v2) instrument for macOS; the same tree builds a CLAP and VST3
instrument for Windows and Linux (not built or tested yet). The plugin is the app itself, `firmware/src/textura.c`
compiled unchanged over the browser emulator's platform (`web/web_hal.c`), so it plays what the FM-1 plays: the same
four lanes, the same sounds, parameters, presets and scenes. A desktop has no CPU limit like the FM-1's, so the
plugin is the HQ build: the CPU governor never thins a lane and the grain delay plays every grain (`LN_HQ` in
`firmware/src/lanes.c`).

This is step 1: no window of its own yet. Hosts show their generic parameter view.

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
| Channel n notes, CCs (n = 1 .. 4) | lane n |
| Channel 5 CCs | the effects, the tempo, the groove and the scale |
| Channel 1 program change | 0 .. 99 load F001 .. F100, 100 .. 115 load U01 .. U16 |

The host's tempo leads when it gives one (the FM-1's own tempo otherwise).

## Parameters

About 290 host parameters, by the app's own stable ids, so automation stays put across versions:

- **PRESET**: F001 .. F100 (not automatable: it loads a whole scene, as a program change does).
- **Lane 1 .. 4**: SOUND, HOW MANY, HOW ACTIVE, LEVEL, PAN, CARD 1 .. 8, GROOVE PART, GROOVE CYCLE. A card's meaning
  depends on the lane's sound; its host value is 0 .. 127 onto the card's own range, as its CC. A card the user has
  not set shows its default to the host.
- **Effects**: the grain delay, reverb, compressor, drive and filter, as on their pages.
- **Groove, keys (scale), MIDI IN channels, EFFECTS ON LOAD**.
- **LFO 1 .. 8, the envelopes and followers, the matrix (MOD 1 .. 16) and MOD CC**.

The FM-1's tempo, clock, screen and status settings are not host parameters.

A change the app makes itself (a CC, a program change, a scene load) goes back to the host as a parameter change.
Set SOUND and a card of the new sound in separate steps: a new sound starts from its own cards, so a card value sent
together with the SOUND change is replaced by the new sound's default (as on the FM-1).

## State

A host project keeps what a user scene keeps (the landscape, each lane's sound, HOW MANY, HOW ACTIVE, level, pan,
mute and cards, the effects, the modulation, the groove, the tempo), the scene loaded last, and the settings that are
parameters (scale, MIDI IN, EFFECTS ON LOAD, PRESETS CHANGE TEMPO). It loads as a scene with its effects. Nothing is
written to disk by the plugin: its user scene bank (U01 .. U16) lives for the session.

## Sample rates and latency

The app always runs at 44.1 kHz in 256-frame blocks. At 44.1 kHz the output is the app's own; 48 kHz and the
multiples of both (88.2, 96, 176.4, 192, 352.8, 384 kHz) go through an oversampler (a 160/147 polyphase stage, then
half-bands); any other rate from 1 to 1536 kHz through a windowed sinc. The latency is constant per rate: one app
block (256 frames, 5.8 ms) plus the oversampler's delay, 258 frames at 44.1 kHz, 325 at 48 kHz. An event acts from
the start of the app block that holds it, as every event does on the FM-1 (notes are not placed inside a block).

Up to 16 instances play in one process (each holds one of 16 independent copies of the app, about 2 MB of memory
each); more play silence and say so in the host's log.

## Tests

`make test-plugin` runs `tools/plugin_gate.py`:

- **equal**: at 44.1 kHz, in the test time (`TEXTURA_PLUGIN_TEST=ms`: the host build's order of events per
  millisecond), the plugin's render of a MIDI script is the host build's (`tests/host.c`, built with `LN_HQ`) sample
  for sample: a lane stopped, notes and CCs on every lane and channel 5 with two presets, a heavy 4-lane scene, and
  every factory preset (`--quick` skips the presets).
- **hq**: the host build with `LN_HQ` plays the heavy scene with the governor's pool unlimited and no lane cut.
- **rates**: at 48, 96 and 88.2 kHz (the oversampler) and 32 kHz (the sinc) the plugin's render equals the host
  build's taken to that rate offline: the residual under -90 dB in the band both resamplers keep.
- **state**: a render's state saved, loaded into a second instance: every parameter reads back the same, and it
  saves the same again; also for instances never activated, every parameter set at random by a flush.
- **cpu**: the time per 512-frame process call at 48 kHz for heavy 4-lane scenes (printed, not judged).

The headless host is `build/plugin/txp_clap_render PLUGIN.clap SCRIPT OUT.wav [--rate HZ] [--block N]
[--state-check] [--fuzz SEED] [--inactive] [--cpu-from MS]`; it plays a host-build script's MIDI lines.

Validators (after `make plugin-deps`):

    .local/plugin-deps/clap-validator/binaries/clap-validator validate build/plugin/bundles/Textura.clap
    sh tools/plugin_vst3_validator.sh                        # the VST3 SDK's own validator (built from the SDK)
    .local/plugin-deps/pluginval/pluginval.app/Contents/MacOS/pluginval --strictness-level 10 --skip-gui-tests \
        --validate build/plugin/bundles/Textura.vst3
    auval -strict -v aumu Txt1 Txtr                          # the Audio Unit, once installed (above)

All four pass: clap-validator (35 passed, 9 skipped for extensions the plugin does not have), the VST3 validator
(47 tests), pluginval at strictness 10 (the VST3 and the Audio Unit, without its editor tests: there is no window
yet). `auval -strict` passes with one warning, "Preset name is not retained in retrieved class data": it comes from
clap-wrapper's Audio Unit, which names the class data from its own field.

## How it is built

| File | What it does |
| --- | --- |
| `plugin/src/engine_unit.c` | one copy of the app: `firmware/src/textura.c` over `web/web_hal.c`, compiled 16 times (`-DTXP_SLOT=0..15`); every symbol static but its table `txp_slot_<n>`, which `plugin/cmake/nm_check.cmake` checks |
| `plugin/src/slot_bridge.h` | a slot's glue: the time (one step a millisecond), the event queue, the audio FIFO, the host parameters, the state |
| `plugin/src/bridge.c` | an instance: the slot it holds, the host's frames onto the app's, the oversampler (`txp_os.h`) and the sinc |
| `plugin/src/textura_clap.c` | the CLAP plugin; clap-wrapper makes the VST3 and the Audio Unit from it |
| `plugin/src/coro/` | the coroutine the app's main loop runs in (arm64 and x86_64 assembly; Windows: Fibers) |
| `plugin/compat/` | `emscripten.h` for `web_hal.c`, and the Windows shims |

The engines (`engines/`) are linked once and shared by the 16 copies: they keep no state outside the instances the
app hands them.
