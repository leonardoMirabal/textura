#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""The README's pictures, rendered from the built emulator (build/web, make fw-web) in headless Chromium (Playwright,
from .local/playwright), as Frodhes' tools/manual_shots.py makes its own from the host build:

  docs/img/device.png   the device with its keyboard map under the keys, at its full size (1200 px wide), the screen
                        on the TEXTURA home (LANDSCAPE) page with the landscape playing
  docs/img/logo.png     the TEXTURA home page itself: the screen's 240 x 240 framebuffer, doubled (nearest) to 480 x
                        480 like the other screenshots in docs/img/
  docs/img/splash.png   the splash at power-on (the mark, the name, what it is, the version), the same size: the logo
                        for the hub's listing

The landscape is started with REC and the firmware's audio pulled by the page (no sound card needed), so the lanes
show their HOW MANY / HOW ACTIVE bars and the keys their lights. The firmware's events are random: each run differs a
little, so the PNGs are committed as rendered, never compared byte for byte.

  make docs-img          (PLAYWRIGHT_BROWSERS_PATH=.local/playwright .local/venv/bin/python tools/emu_shots.py [OUT])
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
PAGE = "() => { const p = Module.HEAPU8, a = Module._web_page(); let s = ''; for (let i = a; p[i]; i++) s += String.fromCharCode(p[i]); return s; }"
# the audio interrupt pulled as a sound card would (8 blocks of 256 frames, ~46 ms)
PUMP = "() => { window.__pump = setInterval(() => Module._web_audio(8), 46); }"


def serve():
    class Quiet(http.server.SimpleHTTPRequestHandler):
        def log_message(self, *a):
            pass
    srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), functools.partial(Quiet, directory=SITE))
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    return srv


def shot(pg, path):
    """the screen's framebuffer, doubled (nearest) to 480 x 480"""
    png = pg.evaluate("document.getElementById('screen').toDataURL('image/png')")
    im = Image.open(io.BytesIO(base64.b64decode(png.split(",", 1)[1]))).convert("RGB")
    im.resize((480, 480), Image.NEAREST).save(path)


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "docs", "img")
    if not os.path.exists(os.path.join(SITE, "textura.wasm")):
        print("no build/web: make fw-web", file=sys.stderr)
        return 1
    os.makedirs(out, exist_ok=True)
    srv = serve()
    url = f"http://127.0.0.1:{srv.server_address[1]}/"
    with sync_playwright() as p:
        b = p.chromium.launch()
        # wide enough that the device column is over 1200 px: the device at scale 1
        pg = b.new_page(viewport={"width": 1680, "height": 1100})
        errs = []
        pg.on("pageerror", lambda e: errs.append(str(e)))
        pg.goto(url)
        pg.wait_for_function("window.Module && window.Module.calledRun", timeout=30000)
        pg.wait_for_timeout(900)                       # inside the splash (1.6 s), its mark drawn
        shot(pg, os.path.join(out, "splash.png"))
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
        if pg.evaluate("document.getElementById('scaler').style.transform") != "scale(1)":
            print("the device is not at scale 1", file=sys.stderr)
            return 1
        pg.locator("#rig").screenshot(path=os.path.join(out, "device.png"), omit_background=False)
        shot(pg, os.path.join(out, "logo.png"))
        b.close()
    srv.shutdown()
    if errs:
        print("JavaScript errors: " + "; ".join(errs), file=sys.stderr)
        return 1
    for n in ("device.png", "logo.png", "splash.png"):
        f = os.path.join(out, n)
        print(f"{os.path.relpath(f, ROOT)}: {Image.open(f).size[0]} x {Image.open(f).size[1]}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
