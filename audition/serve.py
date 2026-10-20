# SPDX-License-Identifier: GPL-3.0-only
"""Serve build/audition on localhost with cross-origin isolation (COOP / COEP), so the browser's timer is fine
(5 us instead of 100 us) and the player's CPU meter can show the slowest callback too. POST /ratings saves the player's
ratings to .local/ratings.json (TX_RATINGS to change it), for the coordinator to read.

    python3 audition/serve.py [PORT]        (default 8000), then open http://localhost:PORT"""
import functools
import http.server
import json
import os
import sys


RATINGS = os.environ.get("TX_RATINGS", os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), ".local",
                                                  "ratings.json"))


class Handler(http.server.SimpleHTTPRequestHandler):
    ratings_path = RATINGS                       # (the player check points this at a temporary file)

    def do_POST(self):
        """POST /ratings: the player's ratings (JSON, < 1 MB) -> .local/ratings.json. Localhost only (the server binds
        127.0.0.1; a request from elsewhere is refused anyway)."""
        if self.path.split("?")[0].rstrip("/").split("/")[-1] != "ratings" or self.client_address[0] not in ("127.0.0.1", "::1"):
            self.send_error(404); return
        n = int(self.headers.get("Content-Length") or 0)
        if n <= 0 or n > (1 << 20):
            self.send_error(413); return
        try:
            data = json.loads(self.rfile.read(n).decode("utf8"))
            if not isinstance(data, dict) or not isinstance(data.get("entries"), list):
                raise ValueError("not a ratings document")
        except Exception as e:
            self.send_error(400, str(e)[:80]); return
        os.makedirs(os.path.dirname(self.ratings_path), exist_ok=True)
        tmp = self.ratings_path + ".tmp"
        with open(tmp, "w") as f:
            json.dump(data, f, indent=1)
        os.replace(tmp, self.ratings_path)
        body = b'{"ok": true}'
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

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
