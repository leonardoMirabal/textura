# SPDX-License-Identifier: GPL-3.0-only
"""Textura player check (headless Chromium, Playwright): the page loads without JS errors, lists the engines and
builds each engine's sliders, renders the same samples as the native build (sha256 of every engine's worst scene
against bench/baselines/renders.sha256), starts the sound, plays computer keys and the on-screen keys, moves a
slider, fills the meters, records 10 s to a downloaded WAV, and takes screenshots (desktop and phone width).

    PLAYWRIGHT_BROWSERS_PATH=.local/playwright .local/venv/bin/python audition/check_player.py
Outputs in build/audition-check/. Exit 1 on any failure. Web MIDI with a real keyboard and the sound itself are not
checked here (no MIDI device, no ears)."""
import functools
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
    from serve import Handler                         # audition/serve.py: COOP / COEP (fine timer)
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
        br = pw.chromium.launch(args=["--autoplay-policy=no-user-gesture-required"])
        page = br.new_page(viewport={"width": 1100, "height": 900}, accept_downloads=True)
        page.on("pageerror", lambda e: errors.append(f"pageerror: {e}"))
        page.on("console", lambda m: errors.append(f"console.{m.type}: {m.text}") if m.type == "error" else None)
        page.goto(url)
        page.wait_for_function("window.txTest && window.txTest.ready", timeout=20000)
        check(page.evaluate("window.crossOriginIsolated"), "served cross-origin isolated (audition/serve.py)")
        n = page.eval_on_selector_all("#engine option", "o => o.length")
        check(n >= 1, f"engine selector lists {n} engine(s)")
        for i in range(n):
            page.select_option("#engine", str(i))
            name = page.evaluate(f"Module.UTF8ToString(Module._txw_name({i}))")
            np_ = page.evaluate("Module._txw_nparams()")
            ns = page.eval_on_selector_all("#sliders input", "x => x.length")
            check(ns == np_ + 2, f"{name}: {ns} sliders = {np_} own parameters + PARTICLES + LEVEL")
            sha = page.evaluate(f"window.txTest.sha({i})")
            want = base.get(f"{name}-max")
            check(want is not None and sha == want, f"{name}: wasm render of the max scene = native baseline ({sha[:16]})")
        nch = page.evaluate("Module._txw_ch_n()")
        ns = page.eval_on_selector_all("#ch-sliders input", "x => x.length")
        check(nch == 4 and ns == nch, f"CHAMBER: {ns} sliders = its {nch} parameters, switch present ({page.locator('#ch-on').count()})")
        idrip = next((i for i in range(n) if page.evaluate(f"Module.UTF8ToString(Module._txw_name({i}))") == "drip"), None)
        if idrip is not None:
            sha = page.evaluate(f"window.txTest.chSha({idrip})")
            want = base.get("chamber+drip-max")
            check(want is not None and sha == want, f"CHAMBER: wasm render of drip max through it = native baseline ({sha[:16]})")
        page.select_option("#engine", "0")
        cal = page.evaluate("window.txTest.calib || null")
        check(cal is not None, f"calibration loaded: {cal}")
        page.click("#start")
        page.wait_for_function("document.getElementById('status').textContent.startsWith('Sound on')", timeout=5000)
        check(True, "sound started: " + page.text_content("#status"))
        page.keyboard.down("KeyZ")                      # C3 below the FM-1 range; then the on-screen C4 key
        page.keyboard.down("KeyQ")
        time.sleep(0.6)
        page.mouse.move(0, 0)
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
        page.check("#ch-on")                            # the CHAMBER on while the keys are held: still sound
        time.sleep(0.5)
        lv = page.evaluate("window.txTest.render(8)")
        check(lv > 0, f"CHAMBER on: the output still sounds (peak {lv})")
        page.uncheck("#ch-on")
        with page.expect_download(timeout=20000) as dl:
            page.click("#rec")
        d = dl.value
        path = os.path.join(OUT, "record.wav")
        d.save_as(path)
        size = os.path.getsize(path)
        check(size == 44 + 441000 * 3, f"record 10 s: {d.suggested_filename}, {size} B (24-bit mono 44.1 kHz)")
        page.mouse.up()
        page.keyboard.up("KeyZ")
        page.keyboard.up("KeyQ")
        time.sleep(0.5)
        lit = page.eval_on_selector_all("#kb .key.on", "k => k.length")
        check(lit == 0, "all keys released")
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
    check(not errors, "no JS errors" + ("" if not errors else ": " + "; ".join(errors[:5])))
    print(f"\n{'all player checks passed' if not fails else str(len(fails)) + ' FAILED'}; screenshots in {OUT}")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
