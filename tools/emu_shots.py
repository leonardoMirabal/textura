#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""The README's and the manual's pictures, rendered from the built emulator (build/web, make fw-web) in headless
Chromium (Playwright, from .local/playwright), as Frodhes' tools/manual_shots.py makes its own from the host build.

The README's (docs/img/):
  device.png   the device with its keyboard map under the keys, at its full size (1200 px wide), the screen on the
               TEXTURA home (LANDSCAPE) page with the landscape playing
  logo.png     the TEXTURA home page itself: the screen's 240 x 240 framebuffer, doubled (nearest) to 480 x 480
  splash.png   the splash at power-on (the mark, the name, what it is, the version), the same size: the logo for the
               hub's listing

The manual's (docs/manual/img/, docs/MANUAL.md): every page and overlay of the firmware at 480 x 480 (the framebuffer
doubled, nearest), walked to with the panel's own buttons and encoders (Module._web_btn / _web_enc / _web_keys, as
tests/web_check.py drives them): the home page, the lane's SOUND pages, the lane pages, PERFORM and every effect page
in signal order, the LFO, ENV / FOLLOW and MATRIX pages, GROOVE, TEMPO, SCALE, MIDI CLOCK, MIDI IN, DISPLAY, the
sound browser, the presets list and its LOAD OPTIONS, the save mode, a load's toast, PLAY mode, a muted lane, mod
learn. Then every landscape in turn, each of its sounds put on lane 1 (CC 20) and its SOUND page shot once per
engine (eng-<engine>.png; each checked to have the gaps between its rows black, the firmware's "gaps"); --cards PATH
also writes every sound's cards as JSON, for the manual's sound chapter. Each PNG is reduced to an adaptive palette and optimized, so it
stays small.

The landscape is started with REC and the firmware's audio pulled by the page (no sound card needed), so the lanes
show their HOW MANY / HOW ACTIVE bars and the keys their lights. The firmware's events are random: each run differs a
little, so the PNGs are committed as rendered, never compared byte for byte.

  make docs-img          (PLAYWRIGHT_BROWSERS_PATH=.local/playwright .local/venv/bin/python tools/emu_shots.py)
  tools/emu_shots.py --readme | --manual     one set only
  tools/emu_shots.py --manual --engines      the sound pages only (eng-*.png)
"""
import base64
import functools
import http.server
import io
import os
import sys
import threading

from PIL import Image
from playwright.sync_api import sync_playwright

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SITE = os.path.join(ROOT, "build", "web")
README_OUT = os.path.join(ROOT, "docs", "img")
MANUAL_OUT = os.path.join(ROOT, "docs", "manual", "img")
PAGE = "() => { const p = Module.HEAPU8, a = Module._web_page(); let s = ''; for (let i = a; p[i]; i++) s += String.fromCharCode(p[i]); return s; }"
STATE = "k => Module.ccall('web_state', 'string', ['string'], [k])"
# the audio interrupt pulled as a sound card would (8 blocks of 256 frames, ~46 ms)
PUMP = "() => { window.__pump = setInterval(() => Module._web_audio(8), 46); }"
# the panel's ids (web/app.js BTN, ENCS; firmware/src/platform.c)
OCTD, OCTU, FX, SEL, ENV, LFO, EDIT, GLO, HOME, SAVE, ARP, SEQ, PLAY, REC = range(14)
E_SELECT, E_ALG, E_K1, E_K2, E_K3, E_K4, E_PRESETS = range(7)


def serve():
    class Quiet(http.server.SimpleHTTPRequestHandler):
        def log_message(self, *a):
            pass
    srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), functools.partial(Quiet, directory=SITE))
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    return srv


def save_png(im, path, palette):
    """a PNG, small: the screen's few colours as an adaptive palette (the anti-aliased text keeps its steps)"""
    if palette:
        im = im.quantize(colors=96, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.NONE)
    im.save(path, optimize=True)


def shot(pg, path, palette=False):
    """the screen's framebuffer, doubled (nearest) to 480 x 480"""
    png = pg.evaluate("document.getElementById('screen').toDataURL('image/png')")
    im = Image.open(io.BytesIO(base64.b64decode(png.split(",", 1)[1]))).convert("RGB")
    save_png(im.resize((480, 480), Image.NEAREST), path, palette)


class Panel:
    """the FM-1's panel, from the page"""
    def __init__(self, pg):
        self.pg = pg

    def wait(self, ms):
        self.pg.wait_for_timeout(ms)

    def btn(self, b, hold=80, after=250):
        self.pg.evaluate(f"Module._web_btn({b}, 1)")
        self.wait(hold)
        self.pg.evaluate(f"Module._web_btn({b}, 0)")
        self.wait(after)

    def enc(self, e, d, after=250):
        self.pg.evaluate(f"Module._web_enc({e}, {d})")
        self.wait(after)

    def keys(self, mask, hold, after=300):
        self.pg.evaluate(f"Module._web_keys({mask})")
        self.wait(hold)
        self.pg.evaluate("Module._web_keys(0)")
        self.wait(after)

    def state(self, k):
        return self.pg.evaluate(STATE, k)

    def page(self):
        return self.state("page")


def sound_table():
    """the families and their sounds (firmware/src/landscapes.h: name, the source's preset or the classic engine, eng 1:
    a classic engine) and each sound's hosted engine (firmware/src/sound_cards.h SCD)"""
    import re
    ls = open(os.path.join(ROOT, "firmware", "src", "landscapes.h")).read()
    fams = []
    for m in re.finditer(r'\{"([^"]+)", (\d+), \{(.*?)\}\}\},?\n', ls[ls.index("LS[LS_N]"):ls.index("LS_KC")]):
        snds = re.findall(r'\{"([^"]+)", "([^"]+)", \d+, (\d), \d+', m.group(3))
        fams.append((m.group(1), [(n, src, int(e)) for n, src, e in snds]))
    sc = open(os.path.join(ROOT, "firmware", "src", "sound_cards.h")).read()
    hosted = {n: (h or "") for n, h in re.findall(r'\{"([^"]+)", (?:"([^"]*)"|0), \d+, (?:CL|SAME)', sc)}
    return fams, hosted


def engine_key(fam, src, eng, hosted):
    """the engine a sound runs on, as the manual names it: a classic engine's name, the engine the source hosts, or
    the source's own preset (the birds: the source's bird voice, one for each bird family)"""
    if fam in ("Songbirds", "Wild birds"):
        return "songbird" if fam == "Songbirds" else "wildbird"
    return src.replace(" ", "") if eng else hosted or "source-" + src.replace(" ", "")


def engine_shots(pn, out, errs, cards_json=None):
    """every sound of every family on lane 1: its SOUND pages' cards read back (lab / card states), the first SOUND
    page of each engine's first sound shot as eng-<engine>.png. Families load as landscapes (ALGORITHM + EDIT on the
    home page), a sound comes by CC 20 on lane 1's channel"""
    import json
    fams, hosted = sound_table()
    rows, seen, n = [], set(), 0
    pn.btn(HOME)
    pn.wait(1600)
    pn.keys(1 << 1, 120)                        # F#3: lane 1 selected
    pn.wait(1600)
    for f, (fam, snds) in enumerate(fams):
        for _ in range(3):                      # the next landscape, loaded from the home page
            if pn.state("landscape") == fam:
                break
            pn.btn(HOME)
            if pn.page() != "LANDSCAPE":
                pn.btn(HOME)
            pn.wait(300)
            pn.enc(E_ALG, 1)
            pn.btn(EDIT)
            pn.wait(2500)
        if pn.state("landscape") != fam:
            errs.append(f"landscape {fam} did not load ({pn.state('landscape')})")
            break
        names = [x[0] for x in snds]
        cc = lambda t: 0 if len(snds) < 2 else (t * 127 + (len(snds) - 1) // 2) // (len(snds) - 1)
        for i, (snd, src, eng) in enumerate(snds):
            v = cc(i)
            for j in (1, 2, 3):                 # CC 20 does not take a sound another lane of the family plays:
                if pn.state(f"snd{j}") == snd:  # that lane moves to one not in use
                    used = {pn.state(f"snd{q}") for q in range(4)}
                    free = [t for t in range(len(snds)) if names[t] not in used and t != i]
                    if free:
                        t = max([t for t in free if t < i] or free)
                        pn.pg.evaluate(f"Module._web_midi({0xB0 + j}, 20, {cc(t)})")
                        pn.wait(1500)
            for t in range(24):                 # sent again each second: a landscape's load swaps its lanes in
                if t % 2 == 0:                  # turn, and a lane still swapping does not take a new sound
                    pn.pg.evaluate(f"Module._web_midi(0xB0, 20, {v})")
                pn.wait(500)
                if pn.state("snd0") == snd:
                    break
            if pn.state("snd0") != snd:
                errs.append(f"lane 1 plays {pn.state('snd0')}, not {snd}")
                continue
            if pn.page() != "LANDSCAPE":        # (HOME on the home page goes back to the last page)
                pn.btn(HOME)
            pn.btn(EDIT, after=900)
            pages = []
            first = pn.page()
            if not first.startswith("SOUND") or first.startswith("SOUND 2"):
                errs.append(f"{snd}: cards read on {first}")
            npg = int(first.split("/")[1]) if "/" in first else 1
            for sub in range(npg):
                pages.append([(pn.state(f"lab{k}"), pn.state(f"card{k}")) for k in range(4)])
                if sub + 1 < npg:
                    pn.btn(EDIT, after=700)
            key = engine_key(fam, src, eng, hosted.get(snd, ""))
            rows.append({"family": fam, "sound": snd, "engine": pn.state("lane0"), "key": key, "pages": pages})
            if key not in seen:
                seen.add(key)
                if pn.page() != "LANDSCAPE":
                    pn.btn(HOME)
                pn.btn(EDIT)
                pn.wait(1200)
                if pn.page() not in ("SOUND", "SOUND 1/2"):
                    errs.append(f"{snd}: on {pn.page()}, not SOUND 1")
                path = os.path.join(out, f"eng-{key}.png")
                shot(pn.pg, path, palette=True)
                if pn.state("gaps") != "0":     # (an overlay's marks left between the rows)
                    errs.append(f"{snd}: the gaps between the rows not black ({pn.state('gaps')} px)")
                n += 1
    if cards_json:
        with open(cards_json, "w") as fh:
            json.dump(rows, fh, indent=1)
    return n


def manual_shots(pn, out, errs):
    """every page, in the manual's order; each checked to be the page it is named for"""
    n = 0

    def take(name, want=None, wait=1100, overlay=False):
        nonlocal n
        pn.wait(wait)
        if want is not None and pn.page() != want:
            errs.append(f"{name}: on {pn.page()}, not {want}")
            return
        shot(pn.pg, os.path.join(out, name + ".png"), palette=True)
        if not overlay and pn.state("gaps") != "0":   # (a page: the gaps between its rows black)
            errs.append(f"{name}: the gaps between the rows not black ({pn.state('gaps')} px)")
        n += 1

    def step(want, name):                       # SELECT one page on
        pn.enc(E_SELECT, 1)
        take(name, want)

    # the pages first, on the boot's landscape; the overlays (the lists, a load's toast) last
    take("landscape", "LANDSCAPE", 0)           # the boot's landscape (WATER), playing
    pn.keys(1 << 1, 900)                        # F#3 held: lane 1 muted
    take("landscape-muted", "LANDSCAPE", 200)
    pn.keys(1 << 1, 900)                        # again: back on
    pn.wait(1600)
    pn.enc(E_ALG, 1)                            # a landscape picked, not loaded
    take("landscape-pick", "LANDSCAPE", 600)
    pn.btn(HOME)                                # HOME: the landscape that plays kept
    pn.wait(1600)
    pn.btn(SEL)                                 # PLAY mode
    take("landscape-play", "LANDSCAPE", 1700)
    pn.btn(SEL)
    pn.wait(1600)
    pn.btn(EDIT)                                # the selected lane's SOUND
    take("sound", "SOUND 1/2")
    pn.btn(EDIT)
    take("sound2", "SOUND 2/2")
    for nm, pgname in (("howmany", "LANES 1/4"), ("howactive", "LANES 2/4"), ("pan", "LANES 3/4"),
                       ("onmute", "LANES 4/4"), ("perform", "PERFORM")):
        step(pgname, nm)
    pn.btn(LFO)                                 # the web's click learn: LFO clicked, then a card turned
    pn.enc(E_K2, -14, after=100)
    take("learn", "PERFORM", 150)
    pn.wait(1600)
    for nm, pgname in (("comp", "COMPRESSOR 1/2"), ("comp2", "COMPRESSOR 2/2"), ("drive", "DRIVE"),
                       ("filter", "FILTER"), ("delay", "GRAIN 1/4"), ("delay2", "GRAIN 2/4"), ("grains", "GRAIN 3/4"),
                       ("graintone", "GRAIN 4/4"), ("reverb", "REVERB 1/3"), ("reverb2", "REVERB 2/3"),
                       ("shimmer", "REVERB 3/3"), ("lfo", "LFO 1/3"), ("lfo2", "LFO 2/3"), ("lfo3", "LFO 3/3"),
                       ("env", "ENV 1/2"), ("env2", "ENV 2/2")):
        step(pgname, nm)
    pn.enc(E_ALG, 1)
    pn.enc(E_ALG, 1)                            # ENV 1, ENV 2, FOLLOW 1
    take("follow", "FOLLOW", 1800)
    for nm, pgname in (("matrix", "MATRIX"), ("groove", "GROOVE 1/2"), ("groove2", "GROOVE 2/2"),
                       ("tempo", "TEMPO"), ("scale", "SCALE"), ("midiclock", "MIDI"), ("midiin", "MIDI IN"),
                       ("display", "DISPLAY")):
        step(pgname, nm)
    pn.btn(HOME)
    pn.wait(1600)
    pn.btn(SAVE)                                # the save mode
    take("save", "LANDSCAPE")
    pn.btn(HOME)
    pn.wait(1600)
    # the overlays last
    pn.btn(SEQ)                                 # the sound browser
    take("sounds", "LANDSCAPE", overlay=True)
    pn.enc(E_ALG, 2, after=100)                 # two sounds on: heard on the lane after 0.4 s
    take("sounds-hearing", "LANDSCAPE", 900, overlay=True)
    pn.btn(HOME)                                # closed: the kept sound back
    pn.wait(1600)
    pn.enc(E_PRESETS, 1)                        # the presets list, opened on what plays (L01 WATER)
    pn.enc(E_PRESETS, -60)                      # up into the factory presets: F041 (FOREST)
    take("presets", "LANDSCAPE", overlay=True)
    pn.btn(EDIT, after=0)                       # F041 loads: its toast
    take("loaded", "LANDSCAPE", 400, overlay=True)
    pn.btn(FX)                                  # the list's LOAD OPTIONS
    take("loadoptions", "LANDSCAPE", 1800, overlay=True)
    pn.btn(FX)
    pn.btn(HOME)
    return n


def main():
    sets = {"readme", "manual"}
    if "--readme" in sys.argv:
        sets = {"readme"}
    elif "--manual" in sys.argv:
        sets = {"manual"}
    if not os.path.exists(os.path.join(SITE, "textura.wasm")):
        print("no build/web: make fw-web", file=sys.stderr)
        return 1
    for d in (README_OUT, MANUAL_OUT):
        os.makedirs(d, exist_ok=True)
    srv = serve()
    url = f"http://127.0.0.1:{srv.server_address[1]}/"
    errs = []
    nman = 0
    with sync_playwright() as p:
        b = p.chromium.launch()
        # wide enough that the device column is over 1200 px: the device at scale 1
        pg = b.new_page(viewport={"width": 1680, "height": 1100})
        pg.on("pageerror", lambda e: errs.append(str(e)))
        pg.goto(url)
        pg.wait_for_function("window.Module && window.Module.calledRun", timeout=30000)
        pg.wait_for_timeout(900)                       # inside the splash (1.6 s), its mark drawn
        if "readme" in sets:
            shot(pg, os.path.join(README_OUT, "splash.png"))
        if "manual" in sets:
            shot(pg, os.path.join(MANUAL_OUT, "splash.png"), palette=True)
        pg.wait_for_timeout(1300)                      # the splash over, then the page
        if pg.evaluate(PAGE) != "LANDSCAPE":
            print(f"the firmware is not on LANDSCAPE ({pg.evaluate(PAGE)})", file=sys.stderr)
            return 1
        pg.evaluate(PUMP)
        pg.evaluate("Module._web_btn(13, 1)")          # REC: the landscape's lanes start
        pg.wait_for_timeout(80)
        pg.evaluate("Module._web_btn(13, 0)")
        pg.mouse.move(1, 1)
        pg.wait_for_timeout(3500)                      # the lanes playing, their bars and lights
        if "readme" in sets:
            if pg.evaluate("document.getElementById('scaler').style.transform") != "scale(1)":
                print("the device is not at scale 1", file=sys.stderr)
                return 1
            pg.locator("#rig").screenshot(path=os.path.join(README_OUT, "device.png"), omit_background=False)
            shot(pg, os.path.join(README_OUT, "logo.png"))
        if "manual" in sets:
            if "--engines" not in sys.argv:
                nman = manual_shots(Panel(pg), MANUAL_OUT, errs) + 1
            cj = sys.argv[sys.argv.index("--cards") + 1] if "--cards" in sys.argv else None
            nman += engine_shots(Panel(pg), MANUAL_OUT, errs, cj)
        b.close()
    srv.shutdown()
    if errs:
        print("errors: " + "; ".join(errs), file=sys.stderr)
        return 1
    if "readme" in sets:
        for n in ("device.png", "logo.png", "splash.png"):
            f = os.path.join(README_OUT, n)
            print(f"{os.path.relpath(f, ROOT)}: {Image.open(f).size[0]} x {Image.open(f).size[1]}")
    if "manual" in sets:
        tot = sum(os.path.getsize(os.path.join(MANUAL_OUT, f)) for f in os.listdir(MANUAL_OUT) if f.endswith(".png"))
        print(f"{os.path.relpath(MANUAL_OUT, ROOT)}: {nman} screenshots, 480 x 480, {tot // 1024} KB")
    return 0


if __name__ == "__main__":
    sys.exit(main())
