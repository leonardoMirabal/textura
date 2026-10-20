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

- **Engine**: the selector at the top. Each engine shows its own parameters; PARTICLES (the hero-pool limit: density
  against CPU) and LEVEL are shared by all.
- **Keys**: a key pressed fires a burst, held keeps the cloud going, released lets it ring out. The on-screen keys are
  the FM-1's 27 (F3 to G5). Computer keys: the `Z` row and the `Q` row play two octaves, `[` and `]` move them; their
  velocity is the slider. A MIDI keyboard plays with its velocity; CC 20-27 move the first eight parameters, CC 7 the
  level.
- **Meters**: CPU here (the engine's time per 256-frame block in this browser, averaged over 2 s); the FM-1 estimate
  (each callback also renders one block of a reference scene whose native time `make tx-bench` measured, so the
  engine is compared at the same clock, then scaled by the calibration against Frodhes' measured worst block); the
  output peak level; the particles sounding, events per second and steals.
- **Record 10 s**: downloads the next 10 s of the engine's output as a 24-bit mono WAV.

## Checking it

`make tx-player-check` loads the page in headless Chromium (Playwright, in `.local/`), checks that every engine's
worst scene renders the same samples as the native build, plays keys, reads the meters, records 10 s and takes
screenshots in `build/audition-check/`.
