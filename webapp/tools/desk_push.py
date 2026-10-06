#!/usr/bin/env python3
"""Desk-push the built web bundle onto a sidekick (docs/architecture/sidekick.md (Installing the app bundle) bundle API).

Builds webapp/dist/app first (webapp/tools/build.py, docs/architecture/webapp.md
(Build)), so what lands on the bench is what Pages ships; resolves the host once (mDNS is slow per-request),
then pushes using a cached pairing token if one is on disk (see sidekick.py)
— the button is only needed the first time, or if the cached token gets
rejected (401), or with --forget-token.

Close any browser tab on micro-ft8.local first if the button *is* needed —
pairing.js also claims the token, and the first successful GET wins.

Usage:
  python3 webapp/tools/desk_push.py
  python3 webapp/tools/desk_push.py --host http://192.168.50.128
  python3 webapp/tools/desk_push.py --token HEX…   # skip cache + button
  python3 webapp/tools/desk_push.py --forget-token # clear cache, re-pair
  python3 webapp/tools/desk_push.py --no-build     # push webapp/dist/app as it is
  python3 webapp/tools/desk_push.py --usb [--port /dev/cu.usbmodemXXX]
      # no network: write the build to the sidekick's web partition over USB-C
      # (usb_push.py; replaces the whole web filesystem, keeps WiFi and pairing)
"""

from __future__ import annotations

import json
import sys
import urllib.parse
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from sidekick import (  # noqa: E402
    ROOT,
    AuthError,
    SidekickError,
    clear_token,
    die,
    http,
    resolve_base,
    url,
    with_token_retry,
)

APP = ROOT / "webapp" / "dist" / "app"
MANIFEST = APP / "manifest.json"
DEFAULT_HOST = "http://micro-ft8.local"
WAIT_S = 120.0


def build_bundle() -> dict:
    import build

    return build.build()


def fnv1a(data: bytes) -> str:
    """Matches app.html's own fnv1a() exactly -- same algorithm over the same
    bytes, so its printed hash for app.html can be eyeballed directly against
    the "App: ..." line Settings shows after a push, without needing
    crypto.subtle (unavailable on the sidekick's plain-HTTP origin) or a
    server-side version endpoint."""
    h = 0x811C9DC5
    for b in data:
        h ^= b
        h = (h * 0x01000193) & 0xFFFFFFFF
    return f"{h:08x}"


def _check(code: int, body: bytes, what: str) -> None:
    if code == 401:
        raise AuthError(f"{what} → 401")
    if code != 200:
        die(f"{what} → {code}: {body.decode(errors='replace')}")


def push(host: str, token: str, doc: dict) -> None:
    man_bytes = MANIFEST.read_bytes()
    code, body = http(
        "POST",
        url(host, "/api/bundle/begin"),
        token=token,
        body=man_bytes,
        content_type="application/json",
    )
    _check(code, body, "begin")
    print("begin ok")

    for asset in doc["assets"]:
        path = asset["path"]
        data = (APP / path).read_bytes()
        if len(data) != asset["size"]:
            die(f"{path}: size {len(data)} != manifest {asset['size']} — re-run without --no-build")
        code, body = http(
            "PUT",
            url(host, f"/api/bundle/file?path={urllib.parse.quote(path)}"),
            token=token,
            body=data,
            timeout=60.0,
        )
        _check(code, body, f"put {path}")
        suffix = f", fnv1a {fnv1a(data)}" if path == "app.html" else ""
        print(f"put {path} ({len(data)} bytes{suffix})")

    code, body = http("POST", url(host, "/api/bundle/commit"), token=token)
    _check(code, body, "commit")
    print("commit ok")

    land = "installed.html"
    for a in doc["assets"]:
        if a["path"] == "app.html":
            land = "app.html"
            break
    code, _ = http("GET", url(host, f"/{land}"), timeout=10.0)
    print(f"GET /{land} → {code}")
    if code != 200:
        die(f"/{land} not serving after commit")
    print(f"done — open {url(host, '/' + land)}")


def main() -> int:
    import argparse

    ap = argparse.ArgumentParser(
        description="Build webapp/app and desk-push the result to a sidekick via a cached pairing token or the button."
    )
    ap.add_argument("--host", default=DEFAULT_HOST, help=f"sidekick base URL (default {DEFAULT_HOST})")
    ap.add_argument("--token", default="", help="pairing token hex; skips the cache and the button")
    ap.add_argument("--wait", type=float, default=WAIT_S, help=f"seconds to wait for the button (default {int(WAIT_S)})")
    ap.add_argument("--no-build", action="store_true", help="push webapp/dist/app as it is, without rebuilding")
    ap.add_argument("--forget-token", action="store_true", help="clear the cached token before running (forces the button)")
    ap.add_argument("--usb", action="store_true", help="write the build over USB-C instead of the network (see usb_push.py)")
    ap.add_argument("--port", default="", help="serial port for --usb (default: esptool auto-detects)")
    args = ap.parse_args()

    if args.usb:
        import usb_push

        if not args.no_build:
            build_bundle()
        elif not MANIFEST.is_file():
            die(f"missing {MANIFEST}; run without --no-build")
        usb_push.push(args.port)
        return 0

    if args.forget_token:
        clear_token()
        print("cleared cached token")

    try:
        host = resolve_base(args.host)
        code, _ = http("GET", url(host, "/"), timeout=5.0)
    except SidekickError as e:
        die(str(e))
    if code != 200:
        die(f"sidekick not reachable at {host}/ (HTTP {code})")
    print(f"sidekick ok at {host}")

    if args.no_build:
        if not MANIFEST.is_file():
            die(f"missing {MANIFEST}; run without --no-build")
        doc = json.loads(MANIFEST.read_text())
        print(f"using existing build ({len(doc.get('assets', []))} assets)")
    else:
        doc = build_bundle()

    token = args.token.strip()
    try:
        if token:
            print("using --token from CLI (not cached)")
            push(host, token, doc)
        else:
            with_token_retry(host, args.wait, lambda tok: push(host, tok, doc))
    except SidekickError as e:
        die(str(e))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
