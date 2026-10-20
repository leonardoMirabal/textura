# SPDX-License-Identifier: GPL-3.0-only
"""Serve build/audition on localhost with cross-origin isolation (COOP / COEP), so the browser's timer is fine
(5 us instead of 100 us) and the player's CPU meter can show the slowest callback too.

    python3 audition/serve.py [PORT]        (default 8000), then open http://localhost:PORT"""
import functools
import http.server
import os
import sys


class Handler(http.server.SimpleHTTPRequestHandler):
    def end_headers(self):
        self.send_header("Cross-Origin-Opener-Policy", "same-origin")
        self.send_header("Cross-Origin-Embedder-Policy", "require-corp")
        self.send_header("Cache-Control", "no-store")
        super().end_headers()


if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8000
    site = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "build", "audition")
    h = functools.partial(Handler, directory=site)
    print(f"serving {site} on http://localhost:{port} (cross-origin isolated)")
    http.server.ThreadingHTTPServer(("127.0.0.1", port), h).serve_forever()
