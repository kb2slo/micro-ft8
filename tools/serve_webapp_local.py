#!/usr/bin/env python3
"""Local preview for webapp/ with /download/* proxied to production.

Web Serial needs a secure context — http://localhost qualifies.
Bins/manifest are not in git; this proxies them from the live R2 mirror.

  python3 tools/serve_webapp_local.py
  open http://127.0.0.1:8787/downloads.html

  python3 tools/serve_webapp_local.py --dist   # /app/ from the built bundle
    (run python3 webapp/tools/build.py first; docs/architecture/webapp.md, Build)

  python3 tools/serve_webapp_local.py --sim   # /api/* from a scripted QSO
    open http://127.0.0.1:8787/app/app.html
"""
from __future__ import annotations

import atexit
import http.client
import ssl
import subprocess
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import unquote, urlparse

ROOT = Path(__file__).resolve().parents[1] / "webapp"
DIST_APP = ROOT / "dist" / "app"
# The sidekick serves /pairing.js from its bootstrap, not from the bundle
# (docs/architecture/system.md, Who may key the transmitter); serve the same
# file here so the app's script runs past its pairing calls.
PAIRING_JS = ROOT.parent / "sidekick" / "main" / "web" / "pairing.js"
SERVE_DIST = False  # set by --dist
SIM = False  # set by --sim; /api/* is proxied to sim_station
SIM_PORT = 8790
SIM_PROC: subprocess.Popen | None = None
UPSTREAM_HOST = "micro-ft8.kalikstein.com"
PORT = 8787


class Handler(SimpleHTTPRequestHandler):
    # The sidekick sends these with charset=utf-8 (sidekick/main/web_fs.c) and
    # the pages carry no <meta charset>, so say the same here or "·" renders as "Â·".
    extensions_map = {**SimpleHTTPRequestHandler.extensions_map,
                      ".html": "text/html; charset=utf-8", ".js": "text/javascript; charset=utf-8"}

    # No caching: a preview must show the file on disk now. Without this the
    # browser reused an earlier build's app.html after a code change, and the
    # preview checked the old code (2026-09-28).
    def end_headers(self) -> None:
        self.send_header("Cache-Control", "no-store")
        super().end_headers()

    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=str(ROOT), **kwargs)

    def translate_path(self, path: str) -> str:
        if urlparse(path).path == "/pairing.js":
            return str(PAIRING_JS)
        # --dist: /app/ comes from the built bundle, everything else from webapp/.
        if SERVE_DIST:
            rel = urlparse(path).path
            if rel.startswith("/app/"):
                return str(DIST_APP / unquote(rel[len("/app/"):]))
        return super().translate_path(path)

    def log_message(self, fmt: str, *args) -> None:
        print("[%s] %s" % (self.log_date_time_string(), fmt % args))

    def do_GET(self) -> None:  # noqa: N802
        parsed = urlparse(self.path)
        if SIM and parsed.path.startswith("/api/"):
            self._proxy_sim()
            return
        if parsed.path.startswith("/download/"):
            self._proxy_download(parsed.path)
            return
        super().do_GET()

    def do_POST(self) -> None:  # noqa: N802
        if SIM and urlparse(self.path).path.startswith("/api/"):
            self._proxy_sim()
            return
        self.send_error(404, "Not found")

    def do_PUT(self) -> None:  # noqa: N802
        if SIM and urlparse(self.path).path.startswith("/api/"):
            self._proxy_sim()
            return
        self.send_error(404, "Not found")

    def do_DELETE(self) -> None:  # noqa: N802
        if SIM and urlparse(self.path).path.startswith("/api/"):
            self._proxy_sim()
            return
        self.send_error(404, "Not found")

    def _proxy_sim(self) -> None:
        length = int(self.headers.get("Content-Length", "0") or 0)
        body = self.rfile.read(length) if length else b""
        headers = {}
        token = self.headers.get("X-MicroFT8-Token")
        if token:
            headers["X-MicroFT8-Token"] = token
        if length:
            headers["Content-Length"] = str(length)
            headers["Content-Type"] = self.headers.get("Content-Type", "text/plain")
        try:
            conn = http.client.HTTPConnection("127.0.0.1", SIM_PORT, timeout=5)
            conn.request(self.command, self.path, body=body, headers=headers)
            upstream = conn.getresponse()
            payload = upstream.read()
            status = upstream.status
            ctype = upstream.getheader("Content-Type") or "application/json"
            conn.close()
        except OSError as exc:
            self.send_error(502, f"sim_station: {exc}")
            return
        self.send_response(status)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)

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


def _start_sim(scale: str, scenario: Path) -> None:
    global SIM_PROC
    binary = ROOT.parent / "host_mock" / "sim_station"
    if not binary.is_file():
        raise SystemExit(f"missing {binary}; run: make -C host_mock sim_station")
    if not scenario.is_file():
        raise SystemExit(f"missing {scenario}")
    SIM_PROC = subprocess.Popen(
        [str(binary), "--scale", scale, "--port", str(SIM_PORT), str(scenario)],
        cwd=str(binary.parent),
    )

    def _stop() -> None:
        if SIM_PROC and SIM_PROC.poll() is None:
            SIM_PROC.terminate()
            try:
                SIM_PROC.wait(timeout=2)
            except subprocess.TimeoutExpired:
                SIM_PROC.kill()

    atexit.register(_stop)


def main() -> None:
    global SERVE_DIST, SIM
    import argparse

    ap = argparse.ArgumentParser(description="Local preview of webapp/.")
    ap.add_argument("--dist", action="store_true", help="serve /app/ from webapp/dist/app (the built bundle)")
    ap.add_argument("--sim", action="store_true", help="proxy /api/* to sim_station (a scripted QSO, no radio)")
    ap.add_argument("--scale", default="1", help="with --sim, divide the 15s period by this (default 1, real time)")
    ap.add_argument("--scenario", default=str(ROOT.parent / "host_mock" / "test_qso.json"),
                    help="with --sim, the host_mock JSON scenario to play")
    args = ap.parse_args()
    SERVE_DIST = args.dist
    SIM = args.sim
    if not ROOT.is_dir():
        raise SystemExit(f"missing {ROOT}")
    if SERVE_DIST and not (DIST_APP / "app.html").is_file():
        raise SystemExit(f"missing {DIST_APP}; run: python3 webapp/tools/build.py")
    if SIM:
        _start_sim(args.scale, Path(args.scenario))
    httpd = ThreadingHTTPServer(("127.0.0.1", PORT), Handler)
    print(f"Serving {ROOT}" + (f" (/app/ from {DIST_APP})" if SERVE_DIST else ""))
    print(f"  http://127.0.0.1:{PORT}/downloads.html")
    print(f"  /download/* → https://{UPSTREAM_HOST}/download/*")
    if SIM:
        print(f"  http://127.0.0.1:{PORT}/app/app.html  (sim_station on 127.0.0.1:{SIM_PORT})")
    print("Ctrl-C to stop.")
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        print("\nStopped.")


if __name__ == "__main__":
    main()
