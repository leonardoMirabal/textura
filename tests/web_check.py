# SPDX-License-Identifier: GPL-3.0-only
"""The Textura web panel (web/, build/web/) in headless Chromium (Playwright): the page loads without JS errors, the
firmware runs (the splash, then the LANDSCAPE page on the screen), ?selftest starts the landscape and it sounds, the
on-screen keys reach the firmware and its key LEDs come back (a black key selects its group's lane, LANDSCAPE stays,
held it mutes the lane; a white key starts it), the knobs send one detent per distance of the hand (wheel 50 px, drag 10 px; at most 2 a frame, 12 queued: from Frodhes'
tests/py/test_web_knobs.py), HOME goes home and back, EDIT to SOUND, FX through PERFORM / COMPRESSOR / DRIVE / FILTER / DELAY / REVERB, GLO through GROOVE /
TEMPO / SCALE / MIDI, FX held to PERFORM, LFO to
LFO, ARP to MATRIX (the captions follow), SAVE saves a scene that a reload brings back, and the page fits a phone (390 px, no sideways scroll). The keyboard map lines up with the keys and the quick guide is short; the installer installs on a fake FM-1 (?installtest). Screenshots at desktop and phone width.

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
WANT_VER = os.environ.get("TEXTURA_WEB_VER") or "dev"   # (the Makefile: REL, else dev)


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
STATE = "k => Module.ccall('web_state', 'string', ['string'], [k])"
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
        check(pg.locator("#ver").inner_text() == WANT_VER, f"the version: {WANT_VER} (make fw-web-check REL=X.Y.Z: X.Y.Z)")
        pg.wait_for_timeout(2200)                      # the splash (1.6 s), then the page
        check(pg.evaluate(PAGE) == "LANDSCAPE", "the firmware shows the LANDSCAPE page")
        check(pg.evaluate(LIT) > 3000, f"the screen is drawn ({pg.evaluate(LIT)} px lit)")
        # the keyboard map under the keys, key for key: LANE 1 over the first black group (F#3 .. A#3), LANE 4 over C#5
        # .. D#5, the HOW MANY values under F3 .. E4; the quick guide short, ending on the full manual
        MAPX = """() => { const r = e => e.getBoundingClientRect(), k = n => r(document.querySelector(`#kb > .key[data-k="${n}"]`));
          const br = [...document.querySelectorAll('#map .brk')].map(e => [e.textContent, r(e).left, r(e).right]);
          const v = [...document.querySelectorAll('#map .val')].map(e => [e.textContent, (r(e).left + r(e).right) / 2]);
          return { br, v, k1: k(1).left, k5: k(5).right, k20: k(20).left, k22: k(22).right, w0: (k(0).left + k(0).right) / 2,
                   w9: (k(9).left + k(9).right) / 2 }; }"""
        mx = pg.evaluate(MAPX)
        near = lambda a, b: abs(a - b) < 3
        check([b[0] for b in mx["br"]] == ["LANE 1", "LANE 2", "LANE 3", "LANE 4", "—"], f"the map: lanes 1-4 and F#5's dash ({[b[0] for b in mx['br']]})")
        check(near(mx["br"][0][1], mx["k1"]) and near(mx["br"][0][2], mx["k5"]) and near(mx["br"][3][1], mx["k20"])
              and near(mx["br"][3][2], mx["k22"]), "the map: LANE 1 and LANE 4 span their black keys")
        check([x[0] for x in mx["v"]] == ["OFF", "1", "2", "4", "10", "19", "32"] and near(mx["v"][0][1], mx["w0"])
              and near(mx["v"][5][1], mx["w9"]), "the map: HOW MANY under F3 .. E4, key for key")
        dts = pg.locator("#guide dt").all_inner_texts()
        check(len(dts) <= 20 and "HOME" in dts and "KNOB 1–4" in dts, f"the quick guide: one line per control ({len(dts)})")
        check(pg.locator("#guide a", has_text="Full manual").get_attribute("href").endswith("docs/MANUAL.md"),
              "the quick guide links the full manual")
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
        check(home.locator("small").inner_text() == "LANDSCAPE · HOLD: SETUP", "HOME's caption on SOUND: LANDSCAPE · HOLD: SETUP")
        home.click(delay=60)
        pg.wait_for_timeout(400)
        check(pg.evaluate(PAGE) == "LANDSCAPE", "HOME: the LANDSCAPE page")
        check(home.locator("small").inner_text() == "BACK · HOLD: SETUP", "HOME's caption on LANDSCAPE: BACK · HOLD: SETUP")
        home.click(delay=60)
        pg.wait_for_timeout(400)                         # (slower than a double tap: 0.3 s)
        check(pg.evaluate(PAGE) == "SOUND", "HOME again: back to SOUND")
        home.click(delay=60)
        pg.wait_for_timeout(400)
        check(pg.evaluate(PAGE) == "LANDSCAPE", "HOME once more: LANDSCAPE")
        # EDIT: the lane's SOUND page (again: its next page); FX: PERFORM, COMPRESSOR, DRIVE, FILTER, DELAY,
        # REVERB (every page, the chain's order; the captions follow); GLO: GROOVE 1/2 2/2, TEMPO, SCALE, MIDI
        fx = pg.locator('button.b[data-name="FX"]')
        edit.click(delay=60)
        pg.wait_for_timeout(300)
        check(pg.evaluate(PAGE) == "SOUND", "EDIT: the SOUND page")
        check(edit.locator("small").inner_text() == "NEXT", "EDIT's caption on SOUND: NEXT")
        pg.screenshot(path=os.path.join(OUT, "desktop-sound.png"))
        edit.click(delay=60)
        pg.wait_for_timeout(300)
        check(pg.evaluate(PAGE) == "SOUND", "EDIT again: still SOUND (its next page)")
        def fx_to(n):                                  # (FX steps every page of the effects)
            for _ in range(n):
                fx.click(delay=60)
                pg.wait_for_timeout(150)
        fx_to(1)
        check(pg.evaluate(PAGE) == "PERFORM", "FX: the PERFORM front page")
        check(fx.locator("small").inner_text() == "COMPRESSOR", "FX's caption on PERFORM: COMPRESSOR")
        pg.screenshot(path=os.path.join(OUT, "desktop-effects.png"))
        fx_to(1)
        check(pg.evaluate(PAGE) == "COMPRESSOR", "FX again: the COMPRESSOR pages (the chain's first)")
        check(fx.locator("small").inner_text() == "NEXT", "FX's caption on COMPRESSOR: NEXT")
        pg.wait_for_timeout(700)                         # (the gain-reduction meter in the footer)
        pg.screenshot(path=os.path.join(OUT, "desktop-compressor.png"))
        fx_to(2)
        check(pg.evaluate(PAGE) == "DRIVE", "FX past the COMPRESSOR's pages: the DRIVE page")
        check(fx.locator("small").inner_text() == "FILTER", "FX's caption on DRIVE: FILTER")
        pg.screenshot(path=os.path.join(OUT, "desktop-drive.png"))
        fx_to(1)
        check(pg.evaluate(PAGE) == "FILTER", "FX again: the FILTER page")
        check(fx.locator("small").inner_text() == "GRAIN", "FX's caption on FILTER: GRAIN")
        pg.screenshot(path=os.path.join(OUT, "desktop-filter.png"))
        fx_to(5)
        check(pg.evaluate(PAGE) == "REVERB", "FX past the DELAY's pages: the REVERB pages (the chain's last)")
        check(fx.locator("small").inner_text() == "NEXT", "FX's caption on REVERB: NEXT")
        pg.screenshot(path=os.path.join(OUT, "desktop-reverb.png"))
        glo = pg.locator('button.b[data-name="GLO"]')
        glo.click(delay=60)
        pg.wait_for_timeout(300)
        check(pg.evaluate(PAGE) == "GROOVE", "GLO: the GROOVE pages (every groove control)")
        check(glo.locator("small").inner_text() == "NEXT", "GLO's caption on GROOVE: NEXT")
        pg.screenshot(path=os.path.join(OUT, "desktop-groove.png"))
        glo.click(delay=60)
        pg.wait_for_timeout(300)
        check(pg.evaluate(PAGE) == "GROOVE", "GLO again: GROOVE 2/2, the lanes' roles")
        pg.screenshot(path=os.path.join(OUT, "desktop-groove-lanes.png"))
        glo.click(delay=60)
        pg.wait_for_timeout(300)
        check(pg.evaluate(PAGE) == "TEMPO", "GLO a third time: the TEMPO page")
        check(glo.locator("small").inner_text() == "SCALE", "GLO's caption on TEMPO: SCALE")
        glo.click(delay=60)
        pg.wait_for_timeout(300)
        check(pg.evaluate(PAGE) == "SCALE", "GLO a fourth time: the SCALE page")
        check(glo.locator("small").inner_text() == "MIDI", "GLO's caption on SCALE: MIDI")
        pg.screenshot(path=os.path.join(OUT, "desktop-scale.png"))
        glo.click(delay=60)
        pg.wait_for_timeout(300)
        check(pg.evaluate(PAGE) == "MIDI", "GLO a fifth time: the MIDI page")
        check(glo.locator("small").inner_text() == "MIDI IN", "GLO's caption on MIDI: MIDI IN")
        # FX held (a mouse held down 0.8 s): the PERFORM page from here, and nothing more on its release
        box = fx.bounding_box()
        pg.mouse.move(box["x"] + box["width"] / 2, box["y"] + box["height"] / 2)
        pg.mouse.down()
        pg.wait_for_timeout(800)
        check(pg.evaluate(PAGE) == "PERFORM", "FX held: the PERFORM page")
        pg.mouse.up()
        pg.wait_for_timeout(300)
        check(pg.evaluate(PAGE) == "PERFORM", "FX let go after its hold: still PERFORM")
        glo.click(delay=60)
        pg.wait_for_timeout(300)
        glo.click(delay=60)
        pg.wait_for_timeout(300)
        glo.click(delay=60)
        pg.wait_for_timeout(300)
        glo.click(delay=60)
        pg.wait_for_timeout(300)
        glo.click(delay=60)
        pg.wait_for_timeout(300)
        check(pg.evaluate(PAGE) == "MIDI", "GLO five times from FILTER: the MIDI page again")
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
        # SEQ (SOUNDS): the sound browser for the selected lane (4), one list on its sound; SELECT 3 (Fire), ALGORITHM 1,
        # EDIT: bonfire kept on lane 4, the list stays; HOME closes; SEQ again, ALGORITHM 2, HOME: closed, bonfire kept
        seq = pg.locator('button.b[data-name="SEQ"]')
        check(seq.locator("small").inner_text() == "SOUNDS", "SEQ's caption: SOUNDS")
        seq.click(delay=60)
        pg.wait_for_timeout(200)
        check(pg.evaluate(STATE, "browser") == "2" and pg.evaluate(STATE, "brfam") == pg.evaluate(STATE, "fam3"),
              "SEQ: the sound browser, at lane 4's family")
        pg.screenshot(path=os.path.join(OUT, "browser.png"))
        pg.evaluate("Module._web_enc(0, 3)"); pg.wait_for_timeout(100)
        pg.evaluate("Module._web_enc(1, 1)"); pg.wait_for_timeout(100)
        edit.click(delay=60); pg.wait_for_timeout(200)
        check(pg.evaluate(STATE, "browser") == "2" and pg.evaluate(STATE, "fam3") == "Fire" and pg.evaluate(STATE, "snd3") == "bonfire",
              f"the browser: Fire's bonfire kept for lane 4, the list open ({pg.evaluate(STATE, 'fam3')}, {pg.evaluate(STATE, 'snd3')})")
        home.click(delay=60); pg.wait_for_timeout(100)
        check(pg.evaluate(STATE, "browser") == "0", "the browser: HOME closes it")
        seq.click(delay=60); pg.wait_for_timeout(100)
        pg.evaluate("Module._web_enc(1, 2)"); pg.wait_for_timeout(600)
        home.click(delay=60); pg.wait_for_timeout(200)
        check(pg.evaluate(STATE, "browser") == "0" and pg.evaluate(STATE, "snd3") == "bonfire" and pg.evaluate(PAGE) == "LANDSCAPE",
              "the browser: walked, HOME closes (the kept sound, LANDSCAPE)")
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
        # the installer: the section shown (Chromium has Web MIDI), then ?installtest: a full install through the page on
        # a fake FM-1 (tests/web/fake_fm1.js): M-VAVE's firmware with a chosen file, then a Frodhes FM-1 with firmware.json
        check(pg.locator("#install").is_visible() and not pg.locator("#inst-ui").is_hidden(), "the Install on your FM-1 section")
        it = b.new_page(viewport={"width": 1280, "height": 900})
        ierr = []
        it.on("pageerror", lambda e: ierr.append(str(e)))
        it.add_init_script(path=os.path.join(ROOT, "tests", "web", "fake_fm1.js"))
        it.goto(url + "?installtest")
        it.wait_for_function("document.title.startsWith('INSTALLTEST')", timeout=60000, polling=500)
        check(it.title().startswith("INSTALLTEST ok") and not ierr, f"installtest: {it.title()}" + (f" errors {ierr}" if ierr else ""))
        it.close()
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
        check(box is not None and box["x"] >= 0 and box["x"] + box["width"] <= 390, "phone: the panel's box fits the width (it scrolls inside)")
        ph.screenshot(path=os.path.join(OUT, "phone.png"), full_page=True)
        check(not perr, "phone: no JavaScript errors")
        b.close()
    srv.shutdown()
    print(f"{'FAIL' if fails else 'ok  '} web: {len(fails)} failure(s); screenshots in {os.path.relpath(OUT, ROOT)}/")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
