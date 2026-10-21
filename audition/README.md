# Textura player

Play the texture engines in a browser: the same integer C as the firmware, compiled to WebAssembly.

## Build and run

```sh
make tx-bench        # optional, once: writes build/bench/calib.json (the FM-1 CPU estimate)
make tx-player       # build/audition/ (Emscripten from .local/emsdk, or emcc on the PATH)
python3 -m http.server -d build/audition 8000
```

Open <http://localhost:8000> and press **Start sound**. Web MIDI works on `localhost` (Chrome, Edge, Firefox).

`python3 audition/serve.py 8000` serves the same folder with cross-origin isolation: the browser's timer is then fine
(5 µs instead of 100 µs) and the CPU meters also show the slowest callback.

No Emscripten yet? Install it inside the project:

```sh
git clone https://github.com/emscripten-core/emsdk.git .local/emsdk
.local/emsdk/emsdk install 3.1.74 && .local/emsdk/emsdk activate 3.1.74
```

## Playing

- **Keys** (at the top): the on-screen keys are the FM-1's 27 (F3 to G5). Computer keys as in Ableton: `A S D F G H J
  K L ; '` play the white keys and `W E T Y U O P` the black ones (the letters are printed on the keys); `Z` / `X` move
  the octave, `C` / `V` the velocity. A held letter holds the note (no repeats). The keys are read at page level, so a
  focused slider or select never swallows them. A key pressed fires a burst, held keeps the cloud going, released lets
  it ring out. A MIDI keyboard plays with its velocity; CC 20-27 move the first eight parameters, CC 7 the level.
- **Engine**: a slider and `◀` `▶` step through the engines (`[` / `]` or `←` / `→` from the keyboard) with the
  number and name shown large. A switch is instant and click-free: the old engine fades out over 23 ms while the new
  one starts, and the keys held on that layer carry over to the new engine.
- **Landscape** (the page opens here: engine 35, `source`, on layer 1, lane mode on): `Start a landscape` starts three
  lanes at once (water, crickets, wind). The sounds are a grid of buttons grouped by nature (water, earth, air, fire,
  insects, birds; thunder and surf as classic engines in a lane): a click sets the selected lane's sound. Each lane
  has a row with its sound, its activity step, running or stopped, the CPU governor's state, and an activity bar
  (15 steps, C4 .. C6: click or drag it like the white keys; a thin marker shows the generator's live activity).
  The classic per-engine sections (engine stepper, A/B, layers, parameters, CHAMBER) follow under "Engines (classic)".
- **SOURCE** (shown when the edited layer runs engine 35, `source`): its preset (named from the engine), a `Run`
  switch (the source keeps going without a held key), INTENSITY and PROXIMITY (far .. close; 64 = the preset's own
  distance) as large sliders: each lane's own, as the panel follows the selected lane. `Lane mode`: the keyboard shows C4 to C6; the 15 white keys are the selected
  lane's activity fader (C4 the least, still sounding, C6 massive; a move glides in over ~1 s), the black keys C#4 to
  A#5 pick a lane (the first four; or the number keys `1`-`4`), and tapping the selected lane's black key again starts
  or stops it. A running lane generates on its own (no key held). The white keys light up as the bar, the chosen lane's
  black key is lit, the other running lanes' black keys glow. Computer letters, the on-screen keys and a MIDI keyboard
  (notes 60-84) play it alike. Turning lane mode off stops every lane.
  **Quick lanes**: any engine can sit in a lane (pick the lane, then the engine with the stepper): the player keeps it
  going on its own (a held note, or for the one-shot engines - thunder, rockfall, glass, ice, wood, plate, spring,
  stone drum, minerals - presses re-triggered at the step's rate) and the 15 steps move its own density / rate / amount
  parameters (the `QUICK` table in `app.js`), gliding over ~1 s. The source's presets add the character transitions.
- **Layers** (stage S3): four engines at once. Click a layer to edit it; the engine stepper sets the edited layer's
  engine, `off` empties a layer. Each layer has its own parameters (PARTICLES, the hero-pool limit, and
  LEVEL are shared by all engines), a pan (constant power) and a key zone. Keys: `shared` sends every key to every
  layer, `zones` only to the layers whose range holds it. The layers are summed in stereo, then the CHAMBER (if on),
  then the output limiter (`engines/tx_core_mix.c`, the same chain as the native checks).
- **CHAMBER**: a small space after the mix of the layers (off by default; the switch is remembered in this browser;
  mono, on the middle of the stereo pair): the room's
  lowest modes (BODY) and a short diffuse tail (TAIL) for a room of SIZE 0.2-3 m and wall absorption ABSORB (decay
  from Sabine's law). The same stage renders natively with `build/bench/txrender ENGINE SCENE -c`.
- **Meters**: CPU here (all layers' time per 256-frame block in this browser, averaged over 2 s); the FM-1 estimate
  of all four together against the 75 % gate (each callback also renders one block of a reference scene whose native
  time `make tx-bench` measured, so the engines are compared at the same clock, then scaled by the calibration against
  Frodhes' measured worst block), split per layer in the layer rows (a layer over 18 % is flagged); the output peak
  level (and the limiter when it acts); the edited layer's particles, events per second and steals.
- **A/B: references**: for the edited layer's engine, buttons that play ours (a 10 s render at the defaults) and each
  reference recording (its loudest 10 s), the reference at the gain that matches ours' loudness (BS.1770 integrated;
  less when its peak would clip, shown). Under them, side by side, the spectrograms of ours and of the chosen reference
  on one scale (10 s, 20 Hz-20 kHz log, -120 .. -20 dBFS per bin, the reference after its gain): missing grains, noise
  beds or unwanted tonal lines show at a glance. The data comes from `make tx-ab` (`audition/ab_prepare.py`) into
  `build/audition/ab/`: the recordings are read from `.local/reference/` and stay local (never committed, never
  played by an engine); without it the panel says so.
- **Rating** (on the A/B panel): 1-5 for the edited layer's engine (click again to clear), a short note, and `☆ ref k`
  to keep the reference candidates that sound right (1-2 per engine). Kept in this browser (localStorage) and, when
  served with `audition/serve.py`, also written to `.local/ratings.json` (localhost only, never committed);
  **export** downloads the same JSON (engine, rating, note, kept references, time). Typing a note never plays keys.
- **Record 10 s**: downloads the next 10 s of the output as a 24-bit stereo WAV.

## Checking it

`make tx-player-check` loads the page in headless Chromium (Playwright, in `.local/`), checks that every engine's
worst scene renders the same samples as the native build, plays keys, reads the meters, plays four layers (shared
keys and zones), records 10 s and takes screenshots in `build/audition-check/`. `make tx-test` and `make tx-bench`
run four of the heaviest engines' worst cases together through the same mix (`layers`).
