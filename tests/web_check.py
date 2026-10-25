# SPDX-License-Identifier: GPL-3.0-only
"""The Textura web panel (web/, build/web/) in headless Chromium (Playwright): the page loads without JS errors, the
firmware runs (the splash, then the LANDSCAPE page on the screen), ?selftest starts the landscape and it sounds, the
on-screen keys reach the firmware and its key LEDs come back (a black key selects a sound, a white key starts it), the
knobs send one detent per distance of the hand (wheel 50 px, drag 10 px; at most 2 a frame, 12 queued: from Frodhes'
tests/py/test_web_knobs.py), HOME turns to MIXER, EDIT to SOUND / DRIVE, FX to GRAIN / REVERB (the captions follow),
and the page fits a phone (390 px, no sideways scroll). Screenshots at desktop and phone width.

    make fw-web-check     (PLAYWRIGHT_BROWSERS_PATH=.local/playwright .local/venv/bin/python tests/web_check.py)
Outputs in build/web-check/. Exit 1 on any failure. The sound itself is not judged here (no ears)."""
import functools
import http.server
import os
import re
import sys
import threading

from playwright.sync_api import sync_playwright

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SITE = os.path.join(ROOT, "build", "web")
OUT = os.path.join(ROOT, "build", "web-check")
fails = []


def check(ok, what):
    print(("ok    " if ok else "FAIL  ") + what)
    if not ok:
        fails.append(what)


def serve():
    class Quiet(http.server.SimpleHTTPRequestHandler):
        def log_message(self, *a):
            pass
    h = functools.partial(Quiet, directory=SITE)
    srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), h)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    return srv


WHEEL = """async ev => {
  const k = document.querySelectorAll('.knob.endless')[0];
  window.__det = 0;
  k.dispatchEvent(new PointerEvent('pointerup'));
  for (const [dy, mode, ms] of ev) {
    if (ms) await new Promise(r => setTimeout(r, ms));
    k.dispatchEvent(new WheelEvent('wheel', {deltaY: dy, deltaMode: mode, cancelable: true, bubbles: true}));
  }
  await new Promise(r => setTimeout(r, 400));
  return window.__det;
}"""
COUNT_ENC = """() => { let real = Module._web_enc; window.__det = 0; window.__max = 0;
  Object.defineProperty(Module, "_web_enc", { configurable: true, set(v) { real = v; },
    get() { return (id, d) => { if (id === 0) { window.__det += d; window.__max = Math.max(window.__max, Math.abs(d)); }
      return real(id, d); }; } }); }"""
LIT = "() => { const fb = new Uint16Array(Module.HEAPU8.buffer, Module._web_fb(), 240 * 240); let n = 0; for (const v of fb) if (v) n++; return n; }"
PAGE = "() => { const p = Module.HEAPU8, a = Module._web_page(); let s = ''; for (let i = a; p[i]; i++) s += String.fromCharCode(p[i]); return s; }"
LED = "id => new Uint8Array(Module.HEAPU8.buffer, Module._web_leds(), 64)[id]"


def main():
    os.makedirs(OUT, exist_ok=True)
    if not os.path.exists(os.path.join(SITE, "textura.wasm")):
        print("FAIL  no build/web (make fw-web)")
        return 1
    srv = serve()
    url = f"http://127.0.0.1:{srv.server_address[1]}/"
    with sync_playwright() as p:
        b = p.chromium.launch()
        pg = b.new_page(viewport={"width": 1280, "height": 900})
        errs = []
        pg.on("pageerror", lambda e: errs.append(str(e)))
        pg.goto(url)
        pg.wait_for_function("window.Module && window.Module.calledRun", timeout=30000)
        check(re.match(r"Textura ", pg.title()) is not None, f"the title names Textura ({pg.title()})")
        check(pg.locator("#ver").inner_text() == "dev", "the version: dev (a local build)")
        pg.wait_for_timeout(2200)                      # the splash (1.6 s), then the page
        check(pg.evaluate(PAGE) == "LANDSCAPE", "the firmware shows the LANDSCAPE page")
        check(pg.evaluate(LIT) > 3000, f"the screen is drawn ({pg.evaluate(LIT)} px lit)")
        pg.click("#start")
        pg.wait_for_timeout(300)
        # a black key (D#4, key 10: the 5th sound) selects its lane; a white key (D4, key 9: HOW MANY 16) starts it
        for k in (10, 9):
            pg.locator(f'#kb > .key[data-k="{k}"]').click(delay=80)
            pg.wait_for_timeout(150)
        pg.wait_for_timeout(200)
        check(pg.evaluate(LED, 14 + 9) == 1 and pg.evaluate(LED, 14 + 4) == 1,
              "the white keys show HOW MANY (key LEDs back from the firmware)")
        check(pg.locator('#kb > .key[data-k="9"]').get_attribute("class").find("lit") >= 0, "the page lights the key")
        check(pg.evaluate(LED, 13) == 1, "REC's light: something plays")
        pg.screenshot(path=os.path.join(OUT, "desktop.png"))
        # HOME: MIXER (and the captions follow the page)
        home = pg.locator('button.b[data-name="HOME"]')
        home.click(delay=60)
        pg.wait_for_timeout(300)
        check(pg.evaluate(PAGE) == "MIXER", "HOME: the MIXER page")
        check(home.locator("small").inner_text() == "LANDSCAPE", "HOME's caption on MIXER: LANDSCAPE")
        pg.screenshot(path=os.path.join(OUT, "desktop-mixer.png"))
        # EDIT: the lane's SOUND page, again: DRIVE; FX: GRAIN, again: REVERB (the captions follow)
        edit, fx = pg.locator('button.b[data-name="EDIT"]'), pg.locator('button.b[data-name="FX"]')
        edit.click(delay=60)
        pg.wait_for_timeout(300)
        check(pg.evaluate(PAGE) == "SOUND", "EDIT: the SOUND page")
        check(edit.locator("small").inner_text() == "DRIVE", "EDIT's caption on SOUND: DRIVE")
        pg.screenshot(path=os.path.join(OUT, "desktop-sound.png"))
        edit.click(delay=60)
        pg.wait_for_timeout(300)
        check(pg.evaluate(PAGE) == "DRIVE", "EDIT again: the DRIVE page")
        fx.click(delay=60)
        pg.wait_for_timeout(300)
        check(pg.evaluate(PAGE) == "GRAIN", "FX: the GRAIN page")
        check(fx.locator("small").inner_text() == "REVERB", "FX's caption on GRAIN: REVERB")
        fx.click(delay=60)
        pg.wait_for_timeout(300)
        check(pg.evaluate(PAGE) == "REVERB", "FX again: the REVERB page")
        pg.screenshot(path=os.path.join(OUT, "desktop-reverb.png"))
        home.click(delay=60)
        pg.wait_for_timeout(300)
        check(pg.evaluate(PAGE) == "LANDSCAPE", "HOME: back to LANDSCAPE")
        # the knobs (SELECT, encoder 0): one detent per distance of the hand
        pg.evaluate(COUNT_ENC)
        check(pg.evaluate(WHEEL, [[-4, 0, 8]] * 30) == 2, "a gentle trackpad swipe (120 px): 2 detents")
        check(pg.evaluate(WHEEL, [[100, 0, 0], [100, 0, 150], [100, 0, 150]]) == -6, "mouse wheel notches: 2 each")
        check(pg.evaluate(WHEEL, [[-1, 2, 0]]) == 8, "a page of wheel (deltaMode 2): 8 detents")
        check(pg.evaluate("window.__max") == 1, "one detent per call (the queue, at most 2 a frame)")
        check(not errs, "no JavaScript errors" + (": " + "; ".join(errs) if errs else ""))
        # ?selftest: the landscape started, its level, the screen
        st = b.new_page(viewport={"width": 1280, "height": 900})
        st.goto(url + "?selftest")
        st.wait_for_function("document.title.startsWith('SELFTEST')", timeout=30000)
        m = re.search(r"rms=(-?[\d.]+|-Infinity)dB screen=(\d+)px", st.title())
        rms = float(m.group(1)) if m else -999.0
        check(m is not None and -75 < rms < -3, f"selftest: the landscape sounds (low: its start) ({st.title()})")
        # a phone (390 px): the whole panel scaled in, no sideways scroll
        ph = b.new_page(viewport={"width": 390, "height": 844}, device_scale_factor=2, is_mobile=True, has_touch=True)
        perr = []
        ph.on("pageerror", lambda e: perr.append(str(e)))
        ph.goto(url)
        ph.wait_for_function("window.Module && window.Module.calledRun", timeout=30000)
        ph.wait_for_timeout(2200)
        sw = ph.evaluate("document.documentElement.scrollWidth")
        check(sw <= 390, f"phone: no sideways scroll ({sw} px wide)")
        box = ph.locator("#stage").bounding_box()
        check(box is not None and box["x"] >= 0 and box["x"] + box["width"] <= 390, "phone: the panel fits the width")
        ph.screenshot(path=os.path.join(OUT, "phone.png"), full_page=True)
        check(not perr, "phone: no JavaScript errors")
        b.close()
    srv.shutdown()
    print(f"{'FAIL' if fails else 'ok  '} web: {len(fails)} failure(s); screenshots in {os.path.relpath(OUT, ROOT)}/")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
