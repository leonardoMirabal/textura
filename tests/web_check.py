# SPDX-License-Identifier: GPL-3.0-only
"""The Textura web panel (web/, build/web/) in headless Chromium (Playwright): the page loads without JS errors, the
firmware runs (the splash, then the LANDSCAPE page on the screen), ?selftest starts the landscape and it sounds, the
on-screen keys reach the firmware and its key LEDs come back (a black key selects its group's lane, LANDSCAPE stays,
held it mutes the lane; a white key starts it), the knobs send one detent per distance of the hand (wheel 50 px, drag 10 px; at most 2 a frame, 12 queued: from Frodhes'
tests/py/test_web_knobs.py), HOME turns to MIXER, EDIT to SOUND / FILTER, FX to DRIVE / COMPRESSOR / GRAIN / REVERB, GLO to TEMPO / MIDI, LFO to
LFO, ARP to MATRIX (the captions follow), SAVE saves a scene that a reload brings back, and the page fits a phone (390 px, no sideways scroll). Screenshots at desktop and phone width.

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
SCENE = "() => { const p = Module.HEAPU8, a = Module._web_scene(); let s = ''; for (let i = a; p[i]; i++) s += String.fromCharCode(p[i]); return s; }"
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
        # a black key (C#5, key 20: the 4th group) selects lane 4, LANDSCAPE stays; a white key (D4, key 9: HOW MANY 16)
        # starts it
        for k in (20, 9):
            pg.locator(f'#kb > .key[data-k="{k}"]').click(delay=80)
            pg.wait_for_timeout(150)
        pg.wait_for_timeout(200)
        check(pg.evaluate(LED, 14 + 9) == 1 and pg.evaluate(LED, 14 + 4) == 1,
              "the white keys show HOW MANY (key LEDs back from the firmware)")
        check(pg.locator('#kb > .key[data-k="9"]').get_attribute("class").find("lit") >= 0, "the page lights the key")
        check(pg.evaluate(LED, 13) == 1, "REC's light: something plays")
        check(pg.evaluate(PAGE) == "LANDSCAPE", "the black key kept the LANDSCAPE page")
        # the black key held 0.8 s: lane 4 muted (it plays on: its keys glow, steady, never lit); held again: unmuted
        kb20 = pg.locator('#kb > .key[data-k="20"]')
        kb20.click(delay=800)
        pg.wait_for_timeout(100)
        seen = set()
        for _ in range(8):
            seen.add(pg.evaluate(LED, 14 + 20))
            pg.wait_for_timeout(120)
        check(seen == {2}, f"a black key held: its lane muted, its keys glow steadily (LEDs {sorted(seen)})")
        check(pg.evaluate(LED, 13) == 1, "muted, the lane still plays (REC's light)")
        kb20.click(delay=800)
        pg.wait_for_timeout(100)
        seen = set()
        for _ in range(12):
            seen.add(pg.evaluate(LED, 14 + 20))
            pg.wait_for_timeout(120)
        check(1 in seen, f"held again: unmuted, its keys blink again (LEDs {sorted(seen)})")
        pg.screenshot(path=os.path.join(OUT, "desktop.png"))
        home, edit = pg.locator('button.b[data-name="HOME"]'), pg.locator('button.b[data-name="EDIT"]')
        edit.click(delay=60)                             # EDIT: the lane's SOUND page
        pg.wait_for_timeout(300)
        check(pg.evaluate(PAGE) == "SOUND", "EDIT after the black key: its lane's SOUND page")
        # HOME (on SOUND): LANDSCAPE, remembering SOUND; again: back to SOUND; once more: LANDSCAPE (the captions follow)
        check(home.locator("small").inner_text() == "LANDSCAPE", "HOME's caption on SOUND: LANDSCAPE")
        home.click(delay=60)
        pg.wait_for_timeout(300)
        check(pg.evaluate(PAGE) == "LANDSCAPE", "HOME: the LANDSCAPE page")
        check(home.locator("small").inner_text() == "BACK", "HOME's caption on LANDSCAPE: BACK")
        home.click(delay=60)
        pg.wait_for_timeout(300)
        check(pg.evaluate(PAGE) == "SOUND", "HOME again: back to SOUND")
        home.click(delay=60)
        pg.wait_for_timeout(300)
        check(pg.evaluate(PAGE) == "LANDSCAPE", "HOME once more: LANDSCAPE")
        # EDIT: the lane's SOUND page, again: FILTER (the four lanes'); FX: DRIVE, COMPRESSOR, GRAIN, REVERB (the mix's;
        # the captions follow)
        fx = pg.locator('button.b[data-name="FX"]')
        edit.click(delay=60)
        pg.wait_for_timeout(300)
        check(pg.evaluate(PAGE) == "SOUND", "EDIT: the SOUND page")
        check(edit.locator("small").inner_text() == "FILTER", "EDIT's caption on SOUND: FILTER")
        pg.screenshot(path=os.path.join(OUT, "desktop-sound.png"))
        edit.click(delay=60)
        pg.wait_for_timeout(300)
        check(pg.evaluate(PAGE) == "FILTER", "EDIT again: the FILTER page")
        check(edit.locator("small").inner_text() == "SOUND", "EDIT's caption on FILTER: SOUND")
        fx.click(delay=60)
        pg.wait_for_timeout(300)
        check(pg.evaluate(PAGE) == "DRIVE", "FX: the DRIVE page (the mix's first effect)")
        check(fx.locator("small").inner_text() == "COMPRESSOR", "FX's caption on DRIVE: COMPRESSOR")
        pg.screenshot(path=os.path.join(OUT, "desktop-drive.png"))
        fx.click(delay=60)
        pg.wait_for_timeout(300)
        check(pg.evaluate(PAGE) == "COMPRESSOR", "FX again: the COMPRESSOR page")
        check(fx.locator("small").inner_text() == "GRAIN", "FX's caption on COMPRESSOR: GRAIN")
        pg.wait_for_timeout(700)                         # (the gain-reduction meter in the footer)
        pg.screenshot(path=os.path.join(OUT, "desktop-compressor.png"))
        fx.click(delay=60)
        pg.wait_for_timeout(300)
        check(pg.evaluate(PAGE) == "GRAIN", "FX a third time: the GRAIN page")
        check(fx.locator("small").inner_text() == "REVERB", "FX's caption on GRAIN: REVERB")
        fx.click(delay=60)
        pg.wait_for_timeout(300)
        check(pg.evaluate(PAGE) == "REVERB", "FX a fourth time: the REVERB page")
        pg.screenshot(path=os.path.join(OUT, "desktop-reverb.png"))
        glo = pg.locator('button.b[data-name="GLO"]')
        glo.click(delay=60)
        pg.wait_for_timeout(300)
        check(pg.evaluate(PAGE) == "TEMPO", "GLO: the TEMPO page")
        check(glo.locator("small").inner_text() == "MIDI", "GLO's caption on TEMPO: MIDI")
        pg.screenshot(path=os.path.join(OUT, "desktop-tempo.png"))
        glo.click(delay=60)
        pg.wait_for_timeout(300)
        check(pg.evaluate(PAGE) == "MIDI", "GLO again: the MIDI page")
        # LFO: the LFO pages (the caption: the next one / learn); ARP: the MATRIX
        lfo, arp = pg.locator('button.b[data-name="LFO"]'), pg.locator('button.b[data-name="ARP"]')
        check(lfo.locator("small").inner_text() == "LEARN · AGAIN: LFO" and arp.locator("small").inner_text() == "MATRIX",
              "LFO and ARP captions: LEARN · AGAIN: LFO, MATRIX")
        lfo.click(delay=60)                            # (a click latches mod learn; a second one: the LFO pages)
        pg.wait_for_timeout(300)
        check(pg.evaluate(PAGE) == "MIDI", "LFO click: learn latched, the page stays")
        lfo.click(delay=60)
        pg.wait_for_timeout(300)
        check(pg.evaluate(PAGE) == "LFO", "LFO: the LFO pages")
        check(lfo.locator("small").inner_text() == "NEXT", "LFO's caption on LFO: NEXT")
        pg.screenshot(path=os.path.join(OUT, "desktop-lfo.png"))
        arp.click(delay=60)
        pg.wait_for_timeout(300)
        check(pg.evaluate(PAGE) == "MATRIX", "ARP: the MATRIX page")
        pg.screenshot(path=os.path.join(OUT, "desktop-matrix.png"))
        home.click(delay=60)
        pg.wait_for_timeout(300)
        check(pg.evaluate(PAGE) == "LANDSCAPE", "HOME: back to LANDSCAPE")
        # the knobs (SELECT, encoder 0): one detent per distance of the hand
        pg.evaluate(COUNT_ENC)
        check(pg.evaluate(WHEEL, [[-4, 0, 8]] * 30) == 2, "a gentle trackpad swipe (120 px): 2 detents")
        check(pg.evaluate(WHEEL, [[100, 0, 0], [100, 0, 150], [100, 0, 150]]) == -6, "mouse wheel notches: 2 each")
        check(pg.evaluate(WHEEL, [[-1, 2, 0]]) == 8, "a page of wheel (deltaMode 2): 8 detents")
        check(pg.evaluate("window.__max") == 1, "one detent per call (the queue, at most 2 a frame)")
        # SAVE, SAVE: the scene into U01 (the bank in localStorage); a reload boots into it
        save = pg.locator('button.b[data-name="SAVE"]')
        check(save.locator("small").inner_text() == "SCENE · HOLD: UNDO", "SAVE's caption: SCENE · HOLD: UNDO")
        save.click(delay=60)
        pg.wait_for_timeout(300)
        save.click(delay=60)
        pg.wait_for_timeout(1500)
        check(pg.evaluate(SCENE) == "U01 WATER", f"SAVE, SAVE: U01 saved ({pg.evaluate(SCENE)})")
        pg.wait_for_timeout(2500)                      # (the settings: 2 s after the last change)
        check(pg.evaluate("!!localStorage.getItem('textura.scenes') && !!localStorage.getItem('textura.settings')"),
              "the scene bank and the settings in localStorage")
        check(not errs, "no JavaScript errors" + (": " + "; ".join(errs) if errs else ""))
        pg.reload()
        pg.wait_for_function("window.Module && window.Module.calledRun", timeout=30000)
        pg.click("#start")                             # (the lanes' engines switch in the audio callback)
        pg.wait_for_timeout(1500)
        check(pg.evaluate(SCENE) == "U01 WATER", f"a reload: U01 playing again ({pg.evaluate(SCENE)})")
        pg.evaluate("localStorage.clear()")
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
