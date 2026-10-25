# SPDX-License-Identifier: GPL-3.0-only
"""Textura player check (headless Chromium, Playwright): the page loads without JS errors, lists the engines and
builds each engine's sliders, renders the same samples as the native build (sha256 of every engine's worst scene
against bench/baselines/renders.sha256), starts the sound, plays computer keys and the on-screen keys, moves a
slider, fills the meters, plays four layers at once (a second layer sounds, the CPU split per layer, key zones),
records 10 s to a downloaded stereo WAV, and takes screenshots (desktop and phone width).

    PLAYWRIGHT_BROWSERS_PATH=.local/playwright .local/venv/bin/python audition/check_player.py
Outputs in build/audition-check/. Exit 1 on any failure. Web MIDI with a real keyboard and the sound itself are not
checked here (no MIDI device, no ears)."""
import functools
import json
import http.server
import os
import re
import sys
import threading
import time

from playwright.sync_api import sync_playwright

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SITE = os.path.join(ROOT, "build", "audition")
OUT = os.path.join(ROOT, "build", "audition-check")
fails = []


def check(ok, what):
    print(("ok    " if ok else "FAIL  ") + what)
    if not ok:
        fails.append(what)


def serve(isolated=True):
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    from serve import Handler                         # audition/serve.py: COOP / COEP (fine timer), POST /ratings
    Handler.ratings_path = os.path.join(OUT, "ratings-check.json")   # (never the user's .local/ratings.json)
    Handler.log_message = lambda *a: None
    h = functools.partial(Handler if isolated else http.server.SimpleHTTPRequestHandler, directory=SITE)
    h.log_message = lambda *a: None
    s = http.server.ThreadingHTTPServer(("127.0.0.1", 0), h)
    threading.Thread(target=s.serve_forever, daemon=True).start()
    return s


def main():
    os.makedirs(OUT, exist_ok=True)
    base = {}
    for line in open(os.path.join(ROOT, "bench", "baselines", "renders.sha256")):
        p = line.split()
        if len(p) == 2:
            base[p[1]] = p[0]
    srv = serve()
    url = f"http://127.0.0.1:{srv.server_address[1]}/index.html"
    errors = []
    with sync_playwright() as pw:
        # --disable-audio-output: Chromium renders to a null sink on its own clock, so the audio callbacks run whatever
        # the machine's sound device is doing (with the real device they can stall: the meters then read 0)
        br = pw.chromium.launch(args=["--autoplay-policy=no-user-gesture-required", "--disable-audio-output"])
        page = br.new_page(viewport={"width": 1100, "height": 900}, accept_downloads=True)
        page.on("pageerror", lambda e: errors.append(f"pageerror: {e}"))
        page.on("console", lambda m: errors.append(f"console.{m.type}: {m.text}") if m.type == "error" else None)
        page.goto(url)
        page.wait_for_function("window.txTest && window.txTest.ready", timeout=20000)
        check(page.evaluate("window.crossOriginIsolated"), "served cross-origin isolated (audition/serve.py)")
        # the page opens on the landscape: the source on layer 1, lane mode on, the LANDSCAPE section first; one click
        # starts the landscape's first three sounds (Water: rain on water, rain on a roof, water drops) on their own
        st = page.evaluate("window.txTest.src()")
        yl = page.evaluate("""() => ['kb', 'src-panel', 'eng-panel'].map(i => document.getElementById(i).getBoundingClientRect().top)""")
        bars = page.eval_on_selector_all("#lanes-box .abar", "x => x.length")
        check(st["laneMode"] and st["eng"][0] == "source" and yl == sorted(yl) and bars == 8,
              f"opens on the landscape: lane mode {st['laneMode']}, layer 1 {st['eng'][0]}, keys / Landscape / classic order {[round(y) for y in yl]}, {bars} bars (how many, how active: 2 per lane)")
        page.click("#landscape")
        time.sleep(1.5)
        st = page.evaluate("window.txTest.src()")
        pk = page.evaluate("window.txTest.render(64)")
        names_l = page.evaluate("[0, 1, 2].map(k => document.querySelector(`#lanes-box .lane[data-k='${k}'] .lab`).textContent)")
        check(all(st["run"][:3]) and st["step"][:3] == [3, 1, 1] and st["cnt"][:3] == [2, 1, 1] and pk > 0 and "rain on water" in names_l[0] and "roof" in names_l[1]
              and "water drops" in names_l[2], f"Start a landscape: three sounds running ({names_l}), sounding (peak {pk})")
        bar = page.locator("#lanes-box .lane[data-k='1'] .abar:not(.cbar)")
        cbar = page.locator("#lanes-box .lane[data-k='1'] .abar.cbar")
        bar.scroll_into_view_if_needed()
        bx = bar.bounding_box()
        page.mouse.click(bx["x"] + bx["width"] * 0.95, bx["y"] + bx["height"] / 2)   # how active: the top (busy)
        bx = cbar.bounding_box()
        page.mouse.click(bx["x"] + bx["width"] * 0.95, bx["y"] + bx["height"] / 2)   # how many: the top (32)
        st = page.evaluate("window.txTest.src()")
        check(st["lane"] == 1 and st["step"][1] == 14 and st["cnt"][1] == 6, f"the bars clicked: sound 2 busy (step {st['step'][1]} of 14), 32 of it (count {st['cnt'][1]})")
        page.mouse.click(bx["x"] + bx["width"] * 0.02, bx["y"] + bx["height"] / 2)   # how many: off
        st = page.evaluate("window.txTest.src()")
        check(not st["run"][1] and not st["held"][1] and st["cnt"][1] == 0, "how many at off: the sound off (silence)")
        fm = page.evaluate("window.txTest.family(7)")
        bt = page.eval_on_selector_all("#fam-box .snd button", "b => b.map(x => x.textContent)")
        check(fm["names"][3] == "nightingale" and bt == fm["names"] and fm["snd"] == [0, 1, 2, 3] and not any(page.evaluate("window.txTest.src()")["run"]),
              f"a landscape picked (Songbirds): every sound stopped, its sounds on the black keys {bt}")
        v = page.evaluate("[window.txTest.vol(0, 40), (window.txTest.family(1), window.txTest.family(7), window.txTest.vol(0))]")
        nv = page.eval_on_selector_all("#lanes-box .vol", "x => x.length")
        check(v == [40, 40] and nv == 4, f"a volume per sound: set 40, kept with the sound across landscapes ({v}), {nv} volume sliders")
        page.evaluate("window.txTest.vol(0, 100)")
        page.evaluate("window.txTest.family(0)")
        page.evaluate("window.txTest.setLaneMode(false)")   # (the classic checks below: lane mode off, layers 2-4 off)
        for k in (1, 2, 3):
            page.evaluate(f"window.txTest.layerOff({k})")
        page.evaluate("window.txTest.editLayer(0)")
        time.sleep(1.0)
        n = page.evaluate("Module._txw_count()")
        mx = page.eval_on_selector("#engine", "s => +s.max")
        check(n >= 1 and mx == n - 1, f"engine slider spans {n} engine(s)")
        ys = page.evaluate("""() => ['kb', 'engine', 'layers', 'sliders', 'fx-panel'].map(i => document.getElementById(i).getBoundingClientRect().top)""")
        check(ys == sorted(ys), f"layout: keyboard, engine, layers, parameters, EFFECTS from the top ({[round(y) for y in ys]})")
        def pick(i):
            page.evaluate(f"(() => {{ const s = document.getElementById('engine'); s.value = {i}; s.oninput(); }})()")
        for i in range(n):
            pick(i)
            name = page.evaluate(f"Module.UTF8ToString(Module._txw_name({i}))")
            np_ = page.evaluate("Module._txw_nparams()")
            ns = page.eval_on_selector_all("#sliders input", "x => x.length")
            check(ns == np_ + 2, f"{name}: {ns} sliders = {np_} own parameters + PARTICLES + LEVEL")
            if i in (0, n - 1):
                big = page.text_content("#eng-name")
                check(big == name and page.text_content("#eng-num") == str(i + 1), f"engine slider at {i}: shows {page.text_content('#eng-num')} {big}")
            sha = page.evaluate(f"window.txTest.sha({i})")
            want = base.get(f"{name}-max")
            check(want is not None and sha == want, f"{name}: wasm render of the max scene = native baseline ({sha[:16]})")
        nfx = [page.evaluate(f"Module._txw_fx_n({g})") for g in range(4)]
        nsl = [page.eval_on_selector_all(f"#{b} input", "x => x.length") for b in ("fx-dr", "fx-fl", "fx-rv", "fx-gd")]
        check(nfx == nsl and nfx == [3, 6, 10, 12], f"EFFECTS: drive / filter / reverb / grain delay sliders {nsl} = their parameters {nfx}")
        idrip = next((i for i in range(n) if page.evaluate(f"Module.UTF8ToString(Module._txw_name({i}))") == "drip"), None)
        if idrip is not None:
            sha = page.evaluate(f"window.txTest.chSha({idrip})")
            want = base.get("chamber+drip-max")
            check(want is not None and sha == want, f"CHAMBER: wasm render of drip max through it = native baseline ({sha[:16]})")
        # the computer keys (Ableton layout): letters on the keys, Z / X octave, C / V velocity, letters play even with a
        # slider or a select focused, held = no retrigger
        lab = page.eval_on_selector('#kb .key[data-n="60"] .lt', "e => e.textContent")
        lab2 = page.eval_on_selector('#kb .key[data-n="61"] .lt', "e => e.textContent")
        page.keyboard.press("KeyZ")
        lab3 = page.eval_on_selector('#kb .key[data-n="60"] .lt', "e => e.textContent")
        oct3 = page.text_content("#oct")
        page.keyboard.press("KeyX")
        check(lab == "A" and lab2 == "W" and lab3 == "K" and "C3" in oct3, f"key letters: C4 {lab}, C#4 {lab2}; after Z: C4 {lab3} ({oct3})")
        page.click("#engine")                           # a slider clicked (focused): letters still play
        page.focus("#vel")
        page.keyboard.down("KeyA")
        h1 = page.evaluate("window.txTest.held()")
        page.keyboard.up("KeyA")
        h2 = page.evaluate("window.txTest.held()")
        check(60 in h1 and 60 not in h2, f"a letter plays with a slider focused: A held {h1}, released {h2}")
        km = page.eval_on_selector("#keymode", "s => s.value")
        page.focus("#keymode")
        page.keyboard.down("KeyS")
        h3 = page.evaluate("window.txTest.held()")
        km2 = page.eval_on_selector("#keymode", "s => s.value")
        page.keyboard.up("KeyS")
        check(62 in h3 and km == km2, f"a letter plays with a select focused (S -> {h3}), the select unchanged ({km2})")
        ev0 = page.evaluate("Array.from(new Int32Array(Module.HEAP32.buffer, Module._txw_stats(), 8))[5]")
        page.keyboard.down("KeyF")
        for _ in range(5):
            page.keyboard.down("KeyF")                  # auto-repeat while held: no new notes
        page.keyboard.up("KeyF")
        check(page.evaluate("window.txTest.held()") == [], "a held letter does not retrigger, its release frees it")
        v0 = page.text_content("#velv")
        page.keyboard.press("KeyC")
        v1 = page.text_content("#velv")
        page.keyboard.press("KeyV")
        v2 = page.text_content("#velv")
        check(int(v1) < int(v0) and v2 == v0, f"velocity: {v0}, C -> {v1}, V -> {v2}")
        pick(n - 1)
        page.keyboard.press("BracketRight")             # the shortcuts: ] wraps to the first, - goes back
        first0 = page.text_content("#eng-name")
        pick(n - 1)
        page.evaluate("document.activeElement && document.activeElement.blur()")   # the arrows: not inside a select
        page.keyboard.press("ArrowRight")
        first = page.text_content("#eng-name")
        page.keyboard.press("Minus")
        last = page.text_content("#eng-name")
        names0 = [page.evaluate(f"Module.UTF8ToString(Module._txw_name({i}))") for i in (0, n - 1)]
        check(first0 == first == names0[0] and last == names0[1], f"shortcuts: ] -> {first0}, → -> {first}, - -> {last}")
        pick(0)
        cal = page.evaluate("window.txTest.calib || null")
        check(cal is not None, f"calibration loaded: {cal}")
        if page.is_enabled("#start"):                   # (a letter played above may have started it already)
            page.click("#start")
        try:
            page.wait_for_function("document.getElementById('status').textContent.startsWith('Sound on')", timeout=5000)
        except Exception:
            print("status:", page.text_content("#status"), errors[:3])
            raise
        check(True, "sound started: " + page.text_content("#status"))
        page.keyboard.down("KeyD")                      # E4 and B4 from the letters; then the on-screen C4 key
        page.keyboard.down("KeyJ")
        time.sleep(0.6)
        page.mouse.move(0, 0)
        page.locator('#kb .key[data-n="60"]').scroll_into_view_if_needed()   # (the page is taller than the window)
        box = page.locator('#kb .key[data-n="60"]').bounding_box()
        page.mouse.move(box["x"] + box["width"] / 2, box["y"] + box["height"] - 10)
        page.mouse.down()
        time.sleep(0.6)
        lit = page.eval_on_selector_all("#kb .key.on", "k => k.map(x => x.dataset.n)")
        check("60" in lit, f"on-screen key C4 lit while held (lit: {lit})")
        parts = page.text_content("#parts")
        lvl = page.text_content("#lvl")
        cpu_h = page.text_content("#cpu-host")
        cpu_f = page.text_content("#cpu-fm1")
        m = re.match(r"(\d+) of (\d+)", parts or "")
        check(m and int(m.group(1)) > 0, f"particles meter: {parts}")
        check(lvl and "dBFS" in lvl and "-inf dBFS (hold -inf" not in lvl, f"level meter: {lvl}")
        check(cpu_h and "µs/block" in cpu_h, f"CPU meter (browser): {cpu_h}")
        check(cpu_f and "%" in cpu_f, f"CPU meter (FM-1 estimate): {cpu_f}")
        time.sleep(2.2)                                 # a full 2 s CPU window
        cpu_h = page.text_content("#cpu-host")
        meter = float(cpu_h.split()[0])
        tight = page.evaluate("(() => { const t = performance.now(); for (let i = 0; i < 500; i++) Module._txw_render(4);"
                              " return (performance.now() - t) * 1000 / 2000; })()")
        # The meter times audio callbacks, which run at a lower clock than this tight loop (measured ~7x slower), and
        # load from other processes moves both: the check only catches a broken meter (off by an order of magnitude).
        check(0.2 * tight < meter < 12 * tight, f"CPU meter {meter:.2f} us/block agrees with a timed loop ({tight:.2f})")
        live = page.evaluate("window.txTest.live")
        est_live = live["host"] * live["per"]
        ref_fm1 = page.evaluate("calib.ref_native_run_us * calib.factor")   # the reference: 16 busy heroes too
        check(live["refUs"] > 0 and ref_fm1 / 3 < est_live < ref_fm1 * 3,
              f"FM-1 estimate live {est_live:.0f} us (16 heroes held) vs txbench's reference max@P16 mean {ref_fm1:.0f} us "
              f"(live reference block {live['refUs']:.2f} us: the callbacks run at a lower clock than a tight loop)")
        page.screenshot(path=os.path.join(OUT, "desktop.png"), full_page=True)
        # a slider moves the engine: RATE to 0 then back
        page.evaluate("""() => { const s = document.querySelector('#sliders input[data-id="0"]'); s.value = 127; s.oninput(); }""")
        check(page.eval_on_selector('#sliders input[data-id="0"]', "s => s.parentElement.querySelector('label span:last-child').textContent") == "127",
              "slider RATE moved to 127")
        page.evaluate("window.txTest.fx(2, 0, 0, 1)")   # the REVERB's ROOM (the old CHAMBER) on while the keys are held
        time.sleep(0.5)
        lv = page.evaluate("window.txTest.render(8)")
        check(lv > 0, f"REVERB ROOM on: the output still sounds (peak {lv})")
        for t in (2, 3, 4, 5, 6, 7):                    # HALL, INFINITE, DARK, DRIFT, SWELL, PLATE: each sounds; INFINITE frozen at DECAY 127
            page.evaluate(f"window.txTest.fx(2, 0, 0, {t})")
            page.evaluate("window.txTest.fx(2, 0, 2, 127)") if t == 3 else None
            page.evaluate("window.txTest.render(64)")
            lv = page.evaluate("window.txTest.render(16)")
            st = page.evaluate("window.txTest.fx(2, 0, 1, 90)")
            check(lv > 0 and st["running"] and (st["frozen"] == 2 if t == 3 else True),
                  f"REVERB TYPE {t}: sounds (peak {lv}), running {st['running']}, frozen {st['frozen']}")
        page.evaluate("window.txTest.fx(2, 0, 8, 100)")  # SHIMMER on the frozen-free hall below
        page.evaluate("window.txTest.fx(2, 0, 0, 2)")
        page.evaluate("window.txTest.fx(3, 0, 0, 100)")  # the GRAIN DELAY on, 16 grains asked
        page.evaluate("window.txTest.fx(3, 0, 3, 15)")
        page.evaluate("window.txTest.render(64)")
        lv = page.evaluate("window.txTest.render(16)")
        st = page.evaluate("window.txTest.fx(3, 0, 6, 71)")
        check(lv > 0 and 0 < st["grains"] <= st["cap"] <= 16, f"GRAIN DELAY on (16 asked) + HALL with SHIMMER: sounds (peak {lv}), {st['grains']} grains sounding, cap {st['cap']}")
        page.evaluate("window.txTest.fx(3, 0, 0, 0)")
        page.evaluate("window.txTest.fx(2, 0, 8, 0)")
        page.evaluate("window.txTest.fx(2, 0, 2, 64)")
        page.evaluate("window.txTest.fx(0, 0, 0, 90)")  # layer 1's DRIVE and FILTER on: still sounds
        page.evaluate("window.txTest.fx(1, 0, 0, 2)")
        lv = page.evaluate("window.txTest.render(16)")
        check(lv > 0, f"DRIVE + FILTER on layer 1: the output sounds (peak {lv})")
        for g, p in ((0, 0), (1, 0), (2, 0), (2, 1)):  # all off again, MIX 0: no tail left (the classic checks below)
            page.evaluate(f"window.txTest.fx({g}, 0, {p}, 0)")
        # a switch with keys held: the new engine takes them over; switching a sounding layer off fades (no click)
        page.evaluate("window.txTest.stepEngine(1)")
        time.sleep(0.6)
        st1 = page.evaluate("Module._txw_stats() && Array.from(new Int32Array(Module.HEAP32.buffer, Module._txw_stats(), 8))")
        check(st1[5] > 0, f"engine switched with keys held: the new engine ({page.text_content('#eng-name')}) got them ({st1[5]} events)")
        page.evaluate("window.txTest.stepEngine(-1)")
        time.sleep(0.4)
        jumps = page.evaluate("""() => {
            const a = window.txTest.renderCopy(16);       // the test engine sounding: its own largest sample step
            window.txTest.layerOff(0);
            const b = window.txTest.renderCopy(8);        // switched off: the fade
            const step = x => { let m = 0; for (let i = 2; i < x.length; i += 2) m = Math.max(m, Math.abs(x[i] - x[i - 2])); return m; };
            const peak = x => x.reduce((m, v) => Math.max(m, Math.abs(v)), 0);
            return [step(a), step(b), peak(b.slice(-512))];
        }""")
        check(jumps[1] <= jumps[0] * 1.05 and jumps[2] == 0, f"a layer switched off fades, no click: largest step {jumps[1]:.4f} "
              f"vs {jumps[0]:.4f} while sounding; silent after ({jumps[2]})")
        pick(0)                                         # back to the test engine, every key pressed again
        page.mouse.up(); page.keyboard.up("KeyD"); page.keyboard.up("KeyJ")
        page.keyboard.down("KeyD"); page.keyboard.down("KeyJ")
        page.mouse.move(box["x"] + box["width"] / 2, box["y"] + box["height"] - 10)
        page.mouse.down()
        # S3: a second layer (rain on surfaces, panned right) with the keys held: both layers sound, each has a CPU share
        names = [page.evaluate(f"Module.UTF8ToString(Module._txw_name({i}))") for i in range(n)]
        irs = names.index("rainsurf") if "rainsurf" in names else 1
        page.evaluate(f"(() => {{ window.txTest.editLayer(1); window.txTest.selectEngine({irs}); window.txTest.editLayer(0); }})()")
        page.evaluate("""() => { const p = document.querySelectorAll('#layers .pan')[1]; p.value = 110; p.oninput(); }""")
        page.keyboard.up("KeyJ"); page.keyboard.down("KeyJ")   # a fresh press reaches the new layer
        time.sleep(1.5)
        rows = page.eval_on_selector_all("#layers .layer .st", "x => x.map(e => e.textContent)")
        check(len(rows) == 4 and "%" in rows[0] and "%" in rows[1] and "dBFS" in rows[1] and "-inf" not in rows[1],
              f"two layers sounding, each with its CPU share and level: {rows[:2]}")
        check(page.evaluate("window.txTest.route(60).length") == 2, "shared keys: C4 goes to both layers")
        page.evaluate("(() => { window.txTest.setMode('zones'); window.txTest.zone(0, 53, 59); window.txTest.zone(1, 60, 79); })()")
        r60, r55 = page.evaluate("window.txTest.route(60)"), page.evaluate("window.txTest.route(55)")
        check(r60 == [1] and r55 == [0], f"zones: C4 -> layer {r60}, G3 -> layer {r55}")
        page.evaluate("window.txTest.setMode('shared')")
        for k, nm in ((2, "stream"), (3, "chimes")):          # four layers: the meter sums them against the 75 % gate
            if nm in names:
                page.evaluate(f"(() => {{ window.txTest.editLayer({k}); window.txTest.selectEngine({names.index(nm)}); }})()")
        page.evaluate("window.txTest.editLayer(0)")
        page.keyboard.up("KeyJ"); page.keyboard.down("KeyJ")
        time.sleep(2.2)
        cpu4 = page.text_content("#cpu-fm1")
        rows = page.eval_on_selector_all("#layers .layer .st", "x => x.map(e => e.textContent)")
        check(cpu4 and "%" in cpu4 and all("%" in r for r in rows), f"four layers: total {cpu4}; rows {rows}")
        page.screenshot(path=os.path.join(OUT, "layers.png"), full_page=True)
        for k in (1, 2, 3):
            page.evaluate(f"window.txTest.layerOff({k})")
        page.keyboard.down("KeyJ")
        with page.expect_download(timeout=20000) as dl:
            page.click("#rec")
        d = dl.value
        path = os.path.join(OUT, "record.wav")
        d.save_as(path)
        size = os.path.getsize(path)
        check(size == 44 + 441000 * 6, f"record 10 s: {d.suggested_filename}, {size} B (24-bit stereo 44.1 kHz)")
        page.mouse.up()
        page.keyboard.up("KeyD")
        page.keyboard.up("KeyJ")
        time.sleep(0.5)
        lit = page.eval_on_selector_all("#kb .key.on", "k => k.length")
        check(lit == 0, "all keys released")
        # SOURCE: its panel, and lane mode (black keys pick a lane, white keys F3 .. G5 its INTENSITY, the keys lit)
        names = page.evaluate("() => [...Array(Module._txw_count()).keys()].map(i => Module.UTF8ToString(Module._txw_name(i)))")
        if "source" in names:
            page.evaluate(f"(() => {{ window.txTest.editLayer(0); window.txTest.selectEngine({names.index('source')}); }})()")
            check(page.evaluate("window.txTest.src()")["panel"], "SOURCE panel shown for the source engine")
            opts = page.eval_on_selector_all("#src-grid button", "o => o.map(x => x.textContent)")
            check(len(opts) >= 22 and all(o in opts for o in ("wind", "water", "songbird", "gull", "thunder", "surf")),
                  f"the sounds as a grid of buttons, grouped: {len(opts)}")
            check(page.eval_on_selector_all("#src-to, #src-morph", "x => x.length") == 0 and page.query_selector("#src-prox") is not None,
                  "SOURCE panel: no morph controls, a PROXIMITY slider")
            page.click("#lanes")
            key = lambda n: page.click(f'#kb .key[data-n="{n}"]')
            kr = page.eval_on_selector_all("#kb .key", "k => [Math.min(...k.map(x => +x.dataset.n)), Math.max(...k.map(x => +x.dataset.n))]")
            check(kr == [60, 84], f"lane mode: the keyboard shows C4 .. C6 ({kr})")
            key(61); key(74)                                 # C#4: sound 1; D5 (how active 2 of 8): running, one of it
            st = page.evaluate("window.txTest.src()")
            g0 = page.evaluate("window.txTest.gen(0)")
            check(st["laneMode"] and st["lane"] == 0 and st["run"][0] and st["held"][0] and st["step"][0] == 3 and st["cnt"][0] == 1 and g0["on"]
                  and st["lit"] == 3 and st["sel"] == [61], f"lane mode: sound 1, one of it, activity 2 ({g0}), {st['lit']} keys lit, selected {st['sel']}")
            key(67)                                          # G4: how many = 8
            st = page.evaluate("window.txTest.src()")
            check(st["cnt"][0] == 4 and st["step"][0] == 3 and st["lit"] == 6, f"lane mode: G4 sets how many (8: count step {st['cnt'][0]}), the activity kept, {st['lit']} keys lit")
            key(63); key(76)                                 # D#4: sound 2; E5: how active 3; sound 1 keeps running and glows
            st = page.evaluate("window.txTest.src()")
            check(st["lane"] == 1 and st["run"][0] and st["run"][1] and st["glow"] == [61] and st["sel"] == [63] and st["lit"] == 4,
                  f"lane mode: sound 2 running beside sound 1 (selected {st['sel']}, glowing {st['glow']}, its own bar {st['lit']})")
            time.sleep(1.0)
            pk = page.evaluate("window.txTest.render(64)")
            check(pk > 0, f"lane mode: two lanes sound (peak {pk})")
            gv = page.evaluate("window.txTest.gov(60)")      # the CPU governor: a tiny budget makes it take heroes
            time.sleep(1.5)
            gv = page.evaluate("window.txTest.gov(0)")
            pk2 = page.evaluate("window.txTest.render(64)")
            cut = [k for k in (0, 1) if gv[k][0] < gv[k][1]]
            page.evaluate("window.txTest.gov(100000)")       # plenty of budget: the heroes come back, one a second
            time.sleep(3.5)
            gv2 = page.evaluate("window.txTest.gov(0)")
            check(cut and pk2 > 0 and any(gv2[k][0] > gv[k][0] for k in cut),
                  f"CPU governor: over budget it took heroes ({gv[:2]}, still sounding: peak {pk2}), back under it gave them back ({gv2[:2]})")
            ng = page.eval_on_selector_all("#gen-sliders input", "x => x.length")
            key(84)                                          # C6: the generator at its most: notes come
            time.sleep(2.0)
            g1 = page.evaluate("window.txTest.gen(1)")
            check(ng == 7 and g1["on"] and g1["state"][4] == 1 and g1["state"][2] > 3 and g1["state"][0] > 60,
                  f"the lane generator: 7 knobs, notes {g1['state'][2]} in 2 s, activity now {g1['state'][0]}")
            page.screenshot(path=os.path.join(OUT, "lanes.png"), full_page=True)
            key(61)                                          # C#4: sound 1 selected again, its own bar shown
            st = page.evaluate("window.txTest.src()")
            check(st["lane"] == 0 and st["lit"] == 6 and st["run"][0], f"lane mode: back to sound 1, its bars ({st['lit']} keys)")
            key(61)                                          # tapped again while selected: sound 1 off
            st = page.evaluate("window.txTest.src()")
            check(not st["run"][0] and not st["held"][0] and st["run"][1], "lane mode: a selected sound's black key tapped again turns it off")
            key(63); key(60)                                 # C4: the selected sound off
            st = page.evaluate("window.txTest.src()")
            check(not st["run"][1] and st["lit"] == 0 and st["cnt"][1] == 0, "lane mode: C4 (how many: off) turns the selected sound off")
            key(75); key(72)                                 # D#5 (the 7th black key): Water's surf, in a free lane; C5 rare
            st = page.evaluate("window.txTest.src()")
            check(any(e == "surf" and r for e, r in zip(st["eng"], st["run"])), f"lane mode: a black key past the fourth plays its sound ({st['eng']})")
            # quick lanes: an old engine in a lane, its own parameters on the fader (stream: FLOW), and a one-shot one
            key(66)                                          # F#4: sound 3 (water drops); put the stream there; F5 = how active 4 (step 7)
            page.evaluate(f"window.txTest.selectEngine({names.index('stream')})")
            key(77)
            time.sleep(1.6)
            st = page.evaluate("window.txTest.src()")
            fl = st["quick"][2]["cur"].get("FLOW", -1)
            check(st["eng"][2] == "stream" and st["run"][2] and st["held"][2] and abs(fl - (10 + 117 * 0.25)) < 20 and st["lit"] == 5,
                  f"quick lane: stream, one of it at activity 4 of 8, holds itself, FLOW {fl:.1f} (39 at the faders, the generator wandering around it), {st['lit']} keys lit")
            key(68)                                          # G#4: sound 4 (stream); thunder there (one-shot: re-pressed at its rate)
            page.evaluate(f"window.txTest.selectEngine({names.index('thunder')})")
            key(84)                                          # C6: the most
            time.sleep(0.5)
            st = page.evaluate("window.txTest.src()")
            check(st["eng"][3] == "thunder" and st["run"][3] and not st["held"][3], "quick lane: thunder runs on re-pressed flashes (no held key)")
            pk = page.evaluate("window.txTest.render(64)")
            check(pk > 0, f"quick lanes sound (peak {pk})")
            page.screenshot(path=os.path.join(OUT, "quick-lanes.png"), full_page=True)
            page.click("#lanes")
            st = page.evaluate("window.txTest.src()")
            check(not st["laneMode"] and not any(st["run"]) and not any(st["held"]), "lane mode off: every lane stopped")
        if os.path.exists(os.path.join(SITE, "ab", "manifest.json")):   # A/B (make tx-ab: local data, references)
            names = page.evaluate("() => [...Array(Module._txw_count()).keys()].map(i => Module.UTF8ToString(Module._txw_name(i)))")
            page.evaluate("window.txTest.editLayer(0)")
            page.evaluate(f"window.txTest.selectEngine({names.index('stream')})")
            time.sleep(0.8)
            ab = page.evaluate("window.txTest.ab()")
            imgs = page.evaluate("() => ['ab-ours', 'ab-ref'].map(i => { const m = document.getElementById(i); return m.complete ? m.naturalWidth : 0; })")
            check(ab["loaded"] and ab["buttons"] >= 3 and ab["ours"].endswith("stream-ours.png") and ab["ref"].endswith("stream-r1.png")
                  and min(imgs) > 0, f"A/B: stream has {ab['buttons'] - 2} reference button(s), both spectrograms shown ({imgs})")
            page.evaluate("window.txTest.abPlay(2)")         # reference 2: plays, its spectrogram replaces the first
            time.sleep(0.8)
            ab2 = page.evaluate("window.txTest.ab()")
            check(ab2["playing"] == "ref 2" and ab2["ref"].endswith("stream-r2.png"), f"A/B: ref 2 plays ({ab2['playing']}), its spectrogram shown")
            page.screenshot(path=os.path.join(OUT, "ab.png"), full_page=True)
            page.evaluate("window.txTest.abPlay(document.querySelectorAll('#ab-buttons button').length - 1)")   # stop
            check(page.evaluate("window.txTest.ab()")["playing"] is None, "A/B: stop")
            if "source" in names:                            # the bird candidates on the source engine's page (picks by ear)
                page.evaluate(f"window.txTest.selectEngine({names.index('source')})")
                time.sleep(0.3)
                nb0 = page.evaluate("window.txTest.ab()")["buttons"] - 2
                page.evaluate("window.txTest.sound(0, 'crow')")
                time.sleep(0.5)
                nb = page.evaluate("window.txTest.ab()")["buttons"] - 2
                keeps = page.eval_on_selector_all("#ab-keep button", "b => b.length")
                check(nb0 == 0 and nb >= 10 and keeps == nb,
                      f"A/B: the bird candidates shown for a bird preset ({nb} references, {keeps} keep buttons; {nb0} for the rain)")
                page.evaluate(f"window.txTest.selectEngine({names.index('stream')})")
                time.sleep(0.3)
            # ratings (step 5): a 4, a note typed without playing notes, a kept reference, saved and exported
            rp = os.path.join(OUT, "ratings-check.json")
            if os.path.exists(rp):
                os.remove(rp)
            page.click("#rate-stars button:nth-child(4)")
            page.fill("#rate-note", "")
            page.click("#rate-note")
            page.keyboard.type("too even, ask")      # (A, S, K are note keys: in the note they must stay text)
            held = page.evaluate("window.txTest.held()")
            page.click("#ab-keep button:nth-of-type(1)")
            time.sleep(1.0)
            rt = page.evaluate("window.txTest.ratings()").get("stream", {})
            check(rt.get("rating") == 4 and rt.get("note") == "too even, ask" and len(rt.get("kept", [])) == 1 and held == [],
                  f"rating: stream 4, the note typed (no keys played: {held}), one reference kept ({rt.get('kept', [{}])[0].get('name', '?') if rt.get('kept') else '-'})")
            saved = None
            try:
                saved = json.load(open(rp))
            except Exception:
                pass
            ent = [x for x in (saved or {}).get("entries", []) if x.get("engine") == "stream"]
            check(bool(ent) and ent[0].get("rating") == 4 and ent[0].get("kept"), f"ratings posted to the local server ({rp})")
            with page.expect_download() as dl:
                page.click("#rate-export")
            dp = os.path.join(OUT, dl.value.suggested_filename)
            dl.value.save_as(dp)
            ex = json.load(open(dp))
            check(any(x.get("engine") == "stream" and x.get("rating") == 4 for x in ex.get("entries", [])),
                  f"ratings export: {dl.value.suggested_filename}")
            page.reload()
            page.wait_for_function("window.txTest && window.txTest.ready", timeout=20000)
            check(page.evaluate("window.txTest.ratings()").get("stream", {}).get("rating") == 4, "ratings kept in this browser (after a reload)")
        else:
            check("make tx-ab" in page.text_content("#ab-note"), "A/B: no data here (make tx-ab), the panel says so")
        # an SMK-37 (or any MIDI keyboard) as the controller: its 37-key layout, the sustain pedal, the wheels' defaults,
        # MIDI learn (synthetic MIDI messages through the same handler as Web MIDI)
        page.evaluate("window.txTest.setLaneMode(false)")
        page.evaluate("window.txTest.editLayer(0)")
        page.select_option("#kblayout", "smk")
        ks = page.evaluate("window.txTest.keys()")
        page.select_option("#kblayout", "fm1")
        ks2 = page.evaluate("window.txTest.keys()")
        check(ks == list(range(48, 85)) and ks2 == list(range(53, 80)),
              f"keyboard layouts: SMK-37 {len(ks)} keys {ks[0]}..{ks[-1]}, FM-1 {len(ks2)} keys {ks2[0]}..{ks2[-1]}")
        page.evaluate("window.txTest.midi(0xB0, 64, 127); window.txTest.midi(0x90, 60, 100); window.txTest.midi(0x80, 60, 0)")
        s1 = page.evaluate("window.txTest.held()")
        page.evaluate("window.txTest.midi(0xB0, 64, 0)")
        s2 = page.evaluate("window.txTest.held()")
        check(60 in s1 and 60 not in s2, f"sustain pedal (CC 64): a note released under it held {s1}, freed when it lifts {s2}")
        sid = page.evaluate("[...Array(Module._txw_count()).keys()].find(i => Module.UTF8ToString(Module._txw_name(i)) === 'source')")
        if sid is not None and sid >= 0:
            page.evaluate(f"window.txTest.selectEngine({sid})")
            page.evaluate("window.txTest.midi(0xE0, 0, 100); window.txTest.midi(0xB0, 1, 20)")
            got = page.evaluate("""() => { const v = n => { for (const i of document.querySelectorAll('#sliders input'))
                if (i.closest('.sl').querySelector('label span').textContent === n) return +i.value; return null; };
                return [v('PROXIMITY'), v('INTENSITY')]; }""")
            check(got == [100, 20], f"the pitch wheel moves PROXIMITY, the mod wheel INTENSITY (source): {got}")
        page.click("#learn")
        page.click('#sliders input[data-id="0"]')
        tgt = page.evaluate("window.txTest.midiMap().learn")
        page.evaluate("window.txTest.midi(0xB0, 30, 0)")
        a0 = page.eval_on_selector('#sliders input[data-id="0"]', "e => +e.value")
        page.evaluate("window.txTest.midi(0xB0, 30, 127)")
        a1 = page.eval_on_selector('#sliders input[data-id="0"]', "e => [+e.value, +e.max]")
        mm = page.evaluate("window.txTest.midiMap()")
        check(bool(tgt) and mm["map"].get("cc30") == tgt and a0 == 0 and a1[0] == a1[1] and "CC 30" in page.text_content("#midimap"),
              f"MIDI learn: CC 30 -> {tgt}: 0 -> {a0}, 127 -> {a1[0]} of {a1[1]}; shown as '{page.text_content('#midimap')}'")
        page.click("#unlearn")
        check(page.evaluate("window.txTest.midiMap()")["map"] == {}, "MIDI map forgotten")
        plain = serve(isolated=False)                   # the README's plain python3 -m http.server case, phone width
        ph = br.new_page(viewport={"width": 390, "height": 844})
        ph.on("pageerror", lambda e: errors.append(f"phone pageerror: {e}"))
        ph.on("console", lambda m: errors.append(f"phone console.{m.type}: {m.text}") if m.type == "error" else None)
        ph.goto(f"http://127.0.0.1:{plain.server_address[1]}/index.html")
        ph.wait_for_function("window.txTest && window.txTest.ready", timeout=20000)
        sw = ph.evaluate("document.documentElement.scrollWidth")
        check(sw <= 390, f"phone width: no horizontal scroll (scrollWidth {sw})")
        ph.screenshot(path=os.path.join(OUT, "phone.png"), full_page=True)
        br.close()
        plain.shutdown()
    srv.shutdown()
    if not os.path.exists(os.path.join(SITE, "ab", "manifest.json")):   # (no A/B data here: its fetch's 404 is expected)
        errors = [e for e in errors if "Failed to load resource" not in e or "404" not in e]
    check(not errors, "no JS errors" + ("" if not errors else ": " + "; ".join(errors[:5])))
    print(f"\n{'all player checks passed' if not fails else str(len(fails)) + ' FAILED'}; screenshots in {OUT}")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
