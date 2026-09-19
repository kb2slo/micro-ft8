#!/usr/bin/env python3
"""Local preview for webapp/ with /download/* proxied to production.

Web Serial needs a secure context — http://localhost qualifies.
Bins/manifest are not in git; this proxies them from the live R2 mirror.

  python3 tools/serve_webapp_local.py
  open http://127.0.0.1:8787/downloads.html
"""
from __future__ import annotations

import http.client
import ssl
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import unquote, urlparse

ROOT = Path(__file__).resolve().parents[1] / "webapp"
UPSTREAM_HOST = "micro-ft8.kalikstein.com"
PORT = 8787


class Handler(SimpleHTTPRequestHandler):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=str(ROOT), **kwargs)

    def log_message(self, fmt: str, *args) -> None:
        print("[%s] %s" % (self.log_date_time_string(), fmt % args))

    def do_GET(self) -> None:  # noqa: N802
        parsed = urlparse(self.path)
        if parsed.path.startswith("/download/"):
            self._proxy_download(parsed.path)
            return
        super().do_GET()

    def do_HEAD(self) -> None:  # noqa: N802
        parsed = urlparse(self.path)
        if parsed.path.startswith("/download/"):
            self._proxy_download(parsed.path, head_only=True)
            return
        super().do_HEAD()

    def _proxy_download(self, path: str, head_only: bool = False) -> None:
        # Same path rules as worker.js: single path segment after /download/
        key = unquote(path[len("/download/") :])
        if not key or "/" in key or "\\" in key or ".." in key:
            self.send_error(404, "Not found")
            return
        conn = http.client.HTTPSConnection(UPSTREAM_HOST, context=ssl.create_default_context())
        try:
            conn.request("GET", "/download/" + key, headers={"User-Agent": "micro-ft8-local-preview/1"})
            upstream = conn.getresponse()
            body = b"" if head_only else upstream.read()
            self.send_response(upstream.status)
            ctype = upstream.getheader("Content-Type") or "application/octet-stream"
            self.send_header("Content-Type", ctype)
            self.send_header("Access-Control-Allow-Origin", "*")
            if not head_only:
                self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            if not head_only and upstream.status < 400:
                self.wfile.write(body)
        finally:
            conn.close()


def main() -> None:
    if not ROOT.is_dir():
        raise SystemExit(f"missing {ROOT}")
    httpd = ThreadingHTTPServer(("127.0.0.1", PORT), Handler)
    print(f"Serving {ROOT}")
    print(f"  http://127.0.0.1:{PORT}/downloads.html")
    print(f"  /download/* → https://{UPSTREAM_HOST}/download/*")
    print("Ctrl-C to stop.")
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        print("\nStopped.")


if __name__ == "__main__":
    main()
