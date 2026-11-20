# Textura as a desktop plugin

The FM-1 firmware as a CLAP, VST3 and Audio Unit (v2) instrument for macOS, and a CLAP and VST3 instrument for
Windows (x64) and Linux (x86_64, aarch64), all built on a Mac (Windows cross-built, Linux in Docker). The plugin is the app itself, `firmware/src/textura.c`
compiled over the browser emulator's platform (`web/web_hal.c`), so it plays what the FM-1 plays: the same sounds,
parameters, presets and scenes. A desktop has no CPU limit like the FM-1's, so the plugin is the HQ build: the CPU
governor never thins a lane and the grain delay plays every grain (`LN_HQ` in `firmware/src/lanes.c`).

The plugin has up to 16 lanes where the FM-1 has 4: the build option `TX_MIX_SLOTS` (`engines/tx_core_mix.h`,
default 4; `plugin/cmake/slots.cmake` sets `TXP_LANES 16`). The FM-1 and the browser builds keep 4 lanes, and their
binaries do not change.

The plugin has its own window (below); hosts can still show their generic parameter view.

**The user manual** is [PLUGIN-MANUAL.md](PLUGIN-MANUAL.md): install, the window, MIDI, lanes, saving, the
differences from the FM-1. `make plugin-manual` prints it to `build/plugin/pkg/Textura-Plugin-Manual.pdf`
(`tools/plugin_manual_pdf.sh`: pandoc or python3's markdown, then a headless Chromium); the zips carry it when it has
been made first. Its pictures (`docs/plugin/img`) are the gate's window pictures: `make plugin-manual-shots`.

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

### Windows and Linux

    make plugin-win           # build/plugin/win-x86_64/bundles/CLAP/Textura.clap, VST3/Textura.vst3, checked, zipped
    make plugin-linux         # build/plugin/linux-x86_64/bundles/Textura.clap, Textura.vst3, checked, zipped
    make plugin-all           # macOS, Windows and Linux
    make test-plugin-linux    # in Docker: the coroutine, tools/plugin_gate.py, the window pictures, both validators
    make test-plugin-win      # under wine in Docker: the coroutine, tools/plugin_gate.py --wine, clap-validator

**Windows** is cross-built here with clang-cl and lld-link (`plugin/cmake/win-x64-clangcl.cmake`) against the
Windows SDK and the MSVC C runtime, linked statically: the binaries need only Windows' own DLLs. `bash
tools/plugin_win.sh setup` installs the toolchain once (Homebrew `llvm@20` and `lld@20`, `xwin` 0.10.0 and its copy of
the SDK in `.local/win-toolchain`, about 630 MB; xwin accepts Microsoft's SDK licence for you, so read it first). On a
Windows machine: `cmake -S plugin -B build/plugin -G Ninja -DCMAKE_TOOLCHAIN_FILE=plugin/cmake/win-x64-clangcl.cmake`
from a Visual Studio x64 prompt with LLVM installed. `tools/plugin_win.sh check` checks the files are x86_64 PE,
export the entry points and import only `kernel32`, `user32`, `gdi32` (and `ole32`, `shell32` for the VST3).

**Linux** builds in Docker (`plugin/tests/docker/linux.Dockerfile`: Ubuntu 20.04, clang 12, lld, CMake 3.28.3), so
the binaries run on glibc 2.31 and newer. They need only libc, libm, libpthread, libdl and libX11, and export only
their entry points. `tools/plugin_linux.sh check` checks this, and that the stack is not executable.
`TXP_LINUX_ARCH=aarch64` (or `make plugin-linux LINUX_ARCH=aarch64`) builds for 64-bit Arm. When Ubuntu's own mirrors
fail, `TXP_APT_MIRROR=http://mirrors.edge.kernel.org/ubuntu/` builds the images from another one.

### Packages

`tools/plugin_pack.sh` (the `pack` step of the two scripts) writes `build/plugin/pkg/textura-plugin-windows-x64.zip`
and `textura-plugin-linux-<arch>.zip`. Each holds the bundles, `INSTALL.txt` (where the files go), the GPL, the
font's licence, the source offer and the third-party notices.

## Try it in a DAW

Copy the bundle your host uses into your plug-in folder, clear the download flag, and rescan:

    cp -R build/plugin/bundles/Textura.clap      ~/Library/Audio/Plug-Ins/CLAP/
    cp -R build/plugin/bundles/Textura.vst3      ~/Library/Audio/Plug-Ins/VST3/
    cp -R build/plugin/bundles/Textura.component ~/Library/Audio/Plug-Ins/Components/
    xattr -dr com.apple.quarantine ~/Library/Audio/Plug-Ins/*/Textura.*

For the Audio Unit, `killall -9 AudioComponentRegistrar` makes macOS see it at once; `auval -v aumu Txt1 Txtr`
checks it. To remove the plugin, delete the three bundles again.

On Windows, copy `Textura.clap` into `C:\Program Files\Common Files\CLAP\` and the whole `Textura.vst3` folder into
`C:\Program Files\Common Files\VST3\` (the files are not code-signed: Properties, Unblock if Windows marks them as
downloaded). On Linux, copy `Textura.clap` into `~/.clap/` and `Textura.vst3` into `~/.vst3/`. Each zip's
`INSTALL.txt` says the same.

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

## The window

One resizable window, 1200 x 720 points by default, any shape from 840 to 2400 points wide and 504 to 1800 high,
drawn at the screen's own resolution (Retina: twice the points). The layout scales evenly by the smaller of width /
1200 and height / 720 and gives the rest to the lanes, so the MAP stays round at any size. Dark and nearly
monochrome: red marks only the selection, what is on (the solo) and what an LFO moves; each engine has a colour of its
own (bone until you choose one). The macOS view is built and tested; the Windows and Linux views (`plugin/gui_win.c`,
`plugin/gui_x11.c`) are built and their pictures checked headless, not yet tried on a real desktop.

Every host parameter has a place in the window (nothing is automation only), and every value there works one way:
drag (up or right raises; shift: fine), the wheel, a double-click for the default (a card: its sound's own), a click
steps a choice. A value's text is the app's own (its page's: `4.2 kHz`, `229 ms`); the host is told every write,
with a gesture around a drag.

- **Top bar**: TEXTURA, the previous / next factory scene and the scene's name (`*` when edited; click it for the
  presets browser), **SAVE** (the scene into a user scene U01 .. U16, named; kept in the project's state), **UNDO**
  (the app's: the last load or sound kept; again: redo), **PLAY ALL / STOP ALL** (the FM-1's REC; white while every
  lane is stopped), LANES - / +, the output's peak (dB), SETTINGS.
- **The place** (left, about a third of the width): **MAP** or **SPACE**, the selected lane's number, family, sound
  (the sound browser) and colour dot (the picker) at its head. MAP: a half circle, the listener at its centre, L, R
  and FAR inside the place; a dot's angle is the lane's PAN, its distance the sound's **DIST card** (a hollow dot
  when it has none: then a drag moves PAN only). Lanes on one spot fan out a few pixels and share a label
  (`1 Rain on water · 2 · 3 · 4`); a label goes to the first place clear of the dots, the other labels and the edges,
  or is left out (the selected lane's always shows). SPACE: the same place in perspective, each lane's events of the
  last 6 seconds drifting toward the horizon; a drag on the ground moves a lane.
- **Lanes** (right, scrolling): one row a lane in use, ADD LANE under them. A row: number and colour, sound and family
  (the sound browser), the **event line**, **HOW MANY** (six blocks, 1 2 4 10 19 32, none lit OFF; a click on a block
  sets that many, on the top lit one one less), **HOW ACTIVE** (nine blocks), PAN, CH (OFF, 1 .. 16), **VOL**, **M**,
  **S**. A stopped lane reads STOPPED across its event line; a landscape's start (all four stopped, as on the FM-1)
  adds ALL LANES STOPPED under the rows. HOW MANY or HOW ACTIVE starts a lane (the app's own: `sc_count`, `sc_step`).
- **Drawer** (the selected lane): its sound, colour, MANY / ACTIVE / VOL and channel; **every card** of the sound (up
  to `SCD_MAX`, 8) with its name, its value in its unit, and its hint (the FM-1 manual's line for that card:
  `plugin/card_hints.h`, made from `docs/MANUAL.md` section 5 by `tools/plugin_card_hints.py`); PAN, DIST, LEVEL,
  GROOVE PART and CYCLE. A value the modulation moves now is red with a `~`.
- **Strip** (bottom): COMP, DRIVE, FILTER, DELAY, REVERB in the mix's order, each with its picture (gain reduction,
  drive curve, the filter's response with HIGH PASS then LOW PASS marked, the echoes, the tail) and its main values;
  then MOVEMENT, two LFOs (the routed ones first) with shape, trace, RATE and route. A section's header opens the
  **expanded view** over the place and the rows: a tab for each effect (all its parameters), LFO 1 .. 8 (trace,
  NOW, ROUTES, its ten parameters), ENV · FOLLOW (ENV 1 and 2, FOLLOW 1 and 2) and MATRIX (MOD 1 .. 16: SOURCE,
  TARGET, DEPTH, SPREAD; MOD CC).
- **Sound browser**: the families and their sounds from the app's own tables, each with its engine. Type to search;
  arrows, left / right for the family. As on the FM-1 the sound under the cursor plays on the lane while it plays;
  **KEEP** keeps it, **CLOSE** puts the old one back.
- **Presets browser**: USER (when any), the factory presets by category, the landscapes. The preview shows the
  preset's lanes and plan; **LOAD** loads it the FM-1's way. EFFECTS ON LOAD and PRESETS CHANGE TEMPO under it.
- **SETTINGS**: MIDI IN (each lane's channel; FX CHANNEL; SELECTED CH; MOD CC; **KEYS**: PLAY or FM-1 KEYS), SCALE
  (MODE, SCALE, ROOT, TRANSPOSE), TIMING (the app's tempo, a BPM and a 0.1 BPM step, the host's leading; GROOVE, its
  AMOUNT and SHIFT), LOADING (EFFECTS ON LOAD, PRESETS CHANGE TEMPO, LANES), WINDOW (SIZE 100 .. 200 %, the view) and
  ENGINE COLOURS.
- **KEYS** (a plugin parameter, `MIDI KEYS`, 0x10001, not automatable): **PLAY** (default) leaves SELECTED CH's notes
  to the app (`nt_midi`: the selected lane plays them as sent). **FM-1 KEYS** takes F3 .. G5 on SELECTED CH as the
  FM-1's 27 keys (`fm1_in.notes`, read by `nt_poll` in its LANDSCAPE mode): black groups select lanes 1 .. 4 (held
  600 ms: mute), white F3 .. E4 set HOW MANY, F4 .. G5 HOW ACTIVE; the black keys above G5 select lanes 5 .. 16. PLAY
  is the default because a DAW's notes are music: tracks and older projects keep playing the selected lane, and the
  FM-1 itself plays USB MIDI notes as sent (its LANDSCAPE keys are its own keys).
- **The selected lane** is the app's own (`sc.sel`): the window shows it and sets it (an event the slot applies), so
  a lane picked by the keys is the window's too, and the state saves it.
- **Colours**: eleven, none red. A colour belongs to an engine, not a lane.

The window reads what the audio side last published (each lane's events and peak per app block, the lanes' names,
channels and cards, each parameter's text, the modulation's state, the output's peak) and never waits on the audio
thread; its writes go to the audio thread through a queue, and every parameter it sets is told to the host. It draws
the whole picture into a canvas (`plugin/ui/frui_*`, from Frodhes) and the platform view only shows it, so all three
systems look the same. Activity repaints only the place, the event lines, the drawer and strip and the meter.

The font is Inter Tight (SIL Open Font License 1.1, `assets/fonts/OFL-InterTight.txt`). `make plugin` fetches it
into `.local/fonts` (`tools/get_fonts.sh`, checksum-checked) and embeds it in the plugin; it is never committed. The
licence goes into every bundle's `Contents/Resources` (Windows / Linux: next to the CLAP, in the VST3's Resources).

## Parameters

479 host parameters, by the app's own stable ids, so automation stays put across versions. An id is the
scope times 256 plus the index (`firmware/src/params.c`): lanes 1 .. 4 are scopes 0 .. 3, the FM-1's own; lanes
5 .. 16 are scopes 16 .. 27 (ids 0x1000 .. 0x1BFF). The new globals take indices after the FM-1's (LANE 5 .. 16
CHANNEL 68 .. 79, FX CHANNEL 80, LANES 81). No FM-1 id moved, so NRPN numbers and FM-1 scenes stay valid.

- **PRESET**: F001 .. F100 (not automatable: it loads a whole scene, as a program change does).
- **MIDI KEYS** (0x10001, the plugin's own): PLAY or FM-1 KEYS ([the window](#the-window)); not automatable.
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
written to disk by the plugin: the user scene bank (U01 .. U16, the window's SAVE) is kept in the state.

The state is version 2: the scene record of this build, 2040 bytes with its lane count and all 16 lanes. A project
saved by the 4-lane plugin (version 1, the FM-1's scene record) still loads: lanes 1 .. 4 as saved, the others
empty, LANES 4. The state also keeps the solo (`"solo"`, the lanes soloed as a bit mask), KEYS (`"keys"`), the
app's selected lane (`"sel"`), the user scenes (`"umask"`, the slots saved as a bit mask, and `"user"`, each saved
slot's record in hex, in slot order: the FM-1's own scene records, written back into the session's bank on load),
the window's size (`"win"` and `"winh"`, 0 when at the default), the place's view (`"view"`, 1 for SPACE) and the
engines' colours (`"col"`, `"engine name:colour;..."` for the engines not bone; an engine a build has not is
skipped). These keys are optional: a state without them (an older project) loads with no solo, KEYS PLAY, lane 1, no
user scenes, the default window, the MAP and all engines bone. The state stays version 2.

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
- **gui**: the window without a native view (headless): `plugin/tests/scripts/gui4.txt` (four lanes) and `gui16.txt`
  (sixteen) played, then the window painted to a PNG in two separate runs. Both runs must give the same bytes, and
  the picture must not be empty. Each run also paints SPACE, the sound browser, the presets browser, SETTINGS and
  the colour picker, the strip expanded (DELAY, LFO 1, ENV · FOLLOW, MATRIX) and SAVE (`OUT-space.png` ...; all
  must be the same bytes twice too; `gui4.txt` routes three LFOs so the red shows: a lane's PAN, a card, LOW PASS).
  Then the window's controls are used (M, S, VOL, a map dot, LANES +, a preset loaded from the browser, a sound kept
  from the sound browser, a lane's channel and EFFECTS ON LOAD in SETTINGS, a colour, a drag in SPACE, HOW MANY and
  HOW ACTIVE blocks, a card dragged and double-clicked back, GROOVE PART, the strip's HIGH PASS, a parameter in each
  expanded tab, a matrix SOURCE click, MOD CC, KEYS, a SAVE into U01, a row) and checked against the parameters,
  the events sent to the host, the lane's sound, the mute and solo masks and the saved state (solo, lane, view,
  colour, keys, the user scene). The window at 1200 x 720, 1950 x 1210, 1440 x 1080 and 960 x 600: the MAP a half
  circle (one scale both ways) with its labels inside the place. KEYS = FM-1 KEYS (`plugin/tests/scripts/keys.txt`):
  the black keys pick a lane, the white keys set its HOW MANY and HOW ACTIVE, G#5 picks lane 5. The card hints must
  be `docs/MANUAL.md`'s (`tools/plugin_card_hints.py --check`). `--shots DIR` keeps a copy of every picture.

On Linux and Windows the same gate runs on that system's CLAP (`make test-plugin-linux`, `make test-plugin-win`;
Windows under wine, which is not Windows: a pass there is a smoke test). The window pictures are then compared with
macOS's (`make plugin-shots` makes those, `tools/plugin_shots_cmp.py` compares): each must be byte-identical, or at
most 2 levels apart on at most 0.1 % of its pixels, since each system's C library may round `sinf` or `powf` in the
last bit.

The headless host is `build/plugin/txp_clap_render PLUGIN.clap SCRIPT OUT.wav [--rate HZ] [--block N]
[--state-check] [--fuzz SEED] [--inactive] [--cpu-from MS] [--state-in FILE] [--state-out FILE] [--shot OUT.png]
[--shot-w PX] [--shot-h PX] [--gui-test] [--param ID=V] [--get ID] [--list]`; it plays a host-build script's MIDI
lines; `--state-in` loads a state before the activation, `--state-out` saves one after the render; `--param` sets a
parameter before the activation, `--get` prints one after the render, `--list` prints every parameter; `--shot`
paints the window after the render (through a test extension the plugin offers only with `TEXTURA_PLUGIN_GUI_SHOT`
set; the other views next to it), `--gui-test` then uses its controls.

Validators (after `make plugin-deps`):

    .local/plugin-deps/clap-validator/binaries/clap-validator validate build/plugin/bundles/Textura.clap
    sh tools/plugin_vst3_validator.sh                        # the VST3 SDK's own validator (built from the SDK)
    .local/plugin-deps/pluginval/pluginval.app/Contents/MacOS/pluginval --strictness-level 10 \
        --validate build/plugin/bundles/Textura.vst3
    auval -strict -v aumu Txt1 Txtr                          # the Audio Unit, once installed (above)

The VST3 validator passes its 47 tests on macOS and Linux (x86_64 and aarch64), and pluginval at strictness 10
passes on the VST3 (editor tests included). The Audio Unit's checks (`auval -strict`, pluginval) need it installed;
`auval` gives one warning, "Preset name is not retained in retrieved class data", from clap-wrapper's Audio Unit,
which names the class data from its own field.

clap-validator (its Linux build is x86_64 only) passes 33 tests and skips 9 (extensions the plugin does not have) on
macOS and Linux; under wine (`tools/plugin_win.sh validate` gives it the `bcryptprimitives.dll` wine 8 lacks) 32
pass and 10 are skipped. Two fail on every system: `param-set-events` and `param-set-no-cookies`. Each sends one
random value to every parameter at once, PRESET last, once by a flush to an instance never activated and once in a
process call, then compares what the parameters read. A card of lane 1 that the random modulation matrix holds
reads the scene's value after the flush but the sound's default after the process call: the scene load, the sound
switch and the modulation's base meet in a different order on the two paths. A host loading a preset and setting
modulated cards in the same instant would see the same difference. This is a known limit for now.

## How it is built

| File | What it does |
| --- | --- |
| `plugin/src/engine_unit.c` | one copy of the app: `firmware/src/textura.c` over `web/web_hal.c`, compiled 16 times (`-DTXP_SLOT=0..15`, `-DTX_MIX_SLOTS=16u`); every symbol static but its table `txp_slot_<n>`, which `plugin/cmake/nm_check.cmake` checks |
| `plugin/src/slot_bridge.h` | a slot's glue: the time (one step a millisecond), the event queue, the audio FIFO, the host parameters, the state |
| `plugin/src/bridge.c` | an instance: the slot it holds, the host's frames onto the app's, the oversampler (`txp_os.h`) and the sinc |
| `plugin/src/textura_clap.c` | the CLAP plugin; clap-wrapper makes the VST3 and the Audio Unit from it |
| `plugin/src/txp_ui.h` | what the window may ask of its instance: the published snapshot, parameter writes with gestures, mute, solo |
| `plugin/gui_core.c` | the window: layout, drawing, input (any platform) |
| `plugin/gui_clap.c` | the CLAP gui extension, and the test extension (`plugin/gui_shot.h`) |
| `plugin/gui_mac.m`, `gui_win.c`, `gui_x11.c` | the platform views behind `plugin/gui_platform.h` (from Frodhes); `plugin/ui/`: frui's canvas and font (from Frodhes) |
| `plugin/src/coro/` | the coroutine the app's main loop runs in (arm64 and x86_64 assembly; Windows: Fibers) |
| `plugin/compat/` | `emscripten.h` for `web_hal.c`, and the Windows shims |
| `tools/plugin_win.sh`, `tools/plugin_linux.sh`, `tools/plugin_pack.sh` | the Windows and Linux builds, checks, tests and zips (from Frodhes); `plugin/tests/docker/`: the Linux build, wine and clap-validator images |

The engines (`engines/`) are linked once and shared by the 16 copies: they keep no state outside the instances the
app hands them.
