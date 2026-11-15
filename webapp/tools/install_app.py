#!/usr/bin/env python3
"""Push the built web app onto a sidekick, for the bench (docs/architecture/sidekick.md (The bench push)).

A sidekick serves the app its firmware image carries. This sends the app as built from this
checkout, in one request, and the sidekick serves it in the image's place: on the image it was
pushed onto and no other, so an update, a roll back or a flash of another build returns the
station to its image's app. A sidekick running a release refuses it, as does one that is updating
or has an image on trial.

Builds webapp/dist first (webapp/tools/build.py, docs/architecture/webapp.md (Build)); resolves
the host once (mDNS is slow per request), then sends the pack using a cached pairing token if one
is on disk (see sidekick.py). The button is only needed the first time, or if the cached token
gets rejected (401), or with --forget-token.

Close any browser tab on micro-ft8.local first if the button *is* needed: pairing.js also claims
the token, and the first successful GET wins.

Usage:
  python3 webapp/tools/install_app.py
  python3 webapp/tools/install_app.py --host http://192.168.50.128
  python3 webapp/tools/install_app.py --token HEX…   # skip cache + button
  python3 webapp/tools/install_app.py --forget-token # clear cache, re-pair
  python3 webapp/tools/install_app.py --no-build     # push webapp/dist/app.pack as it is, whatever
                                                     # built it last (the sidekick's build stamps
                                                     # it with the image's version)
"""

from __future__ import annotations

import gzip
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import app_pack  # noqa: E402
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

PACK = ROOT / "webapp" / "dist" / "app.pack"
DEFAULT_HOST = "http://micro-ft8.local"
WAIT_S = 120.0

# Why the sidekick would not take a push (sidekick/main/app_push.h, app_push_refusal()), in words.
REFUSALS = {
    "release": "this sidekick runs a release, which serves only the app it was built with; "
               "flash a dev or bench build of the sidekick to push an app onto it",
    "busy": "a firmware update is running; try again when it is done",
    "confirming": "the sidekick's last update is still being confirmed; try again in a minute",
    "size": "the pack does not fit the sidekick's spare firmware slot",
    "pack": "the sidekick read back a pack it could not use; re-run without --no-build",
    "flash": "the sidekick could not write its flash",
}


def fnv1a(data: bytes) -> str:
    """app.html's own fnv1a(), over the same bytes: the page hashes app.html as the browser decoded
    it and shows the result on Settings' App line, so a gzipped file is hashed decompressed here."""
    if data[:2] == b"\x1f\x8b":
        data = gzip.decompress(data)
    h = 0x811C9DC5
    for b in data:
        h ^= b
        h = (h * 0x01000193) & 0xFFFFFFFF
    return f"{h:08x}"


def build_pack() -> bytes:
    import build

    build.build()
    return PACK.read_bytes()


def why_of(body: bytes) -> str:
    try:
        return json.loads(body.decode()).get("why", "")
    except (ValueError, AttributeError):
        return ""


def refusal_text(code: int, body: bytes) -> str:
    """What a refused push means, from the sidekick's reply."""
    # Firmware from before the bench push has no such route: its only match for the path is
    # GET /*, so it answers 405 (404 from one with no wildcard route at all).
    if code in (404, 405):
        return ("this sidekick's firmware takes no pushed app: it is from before the app shipped in the image. "
                "Flash it (cd sidekick && idf.py build flash); that puts this checkout's app on it too")
    return REFUSALS.get(why_of(body)) or f"push → {code}: {body.decode(errors='replace')}"


def ask(host: str, token: str) -> None:
    """Asks whether the sidekick would take a push, with a request that carries no pack, and dies
    with the reason if not. A sidekick that would answers `size`, for the empty body. Asked first
    because a refusal sent ahead of a pack still arriving can be lost: firmware from before the
    push closes the connection with the body unread, and the tool would see only a reset."""
    code, body = http("POST", url(host, "/api/app/push"), token=token, body=b"", content_type="application/octet-stream")
    if code == 401:
        raise AuthError("push → 401")
    if code == 409 and why_of(body) == "size":
        return
    die(refusal_text(code, body))


def push(host: str, token: str, data: bytes) -> str:
    """Sends the pack. Returns the build id the sidekick says it now serves."""
    ask(host, token)
    code, body = http(
        "POST",
        url(host, "/api/app/push"),
        token=token,
        body=data,
        content_type="application/octet-stream",
        timeout=60.0,
    )
    if code == 401:
        raise AuthError("push → 401")
    if code != 200:
        die(refusal_text(code, body))
    try:
        served = json.loads(body.decode()).get("build", "")
    except ValueError:
        served = ""
    code, _ = http("GET", url(host, "/app.html"), timeout=10.0)
    if code != 200:
        die(f"/app.html → {code} after the push")
    return served


def main() -> int:
    import argparse

    ap = argparse.ArgumentParser(
        description="Build webapp/app and push the result onto a sidekick, to be served in its image's place."
    )
    ap.add_argument("--host", default=DEFAULT_HOST, help=f"sidekick base URL (default {DEFAULT_HOST})")
    ap.add_argument("--token", default="", help="pairing token hex; skips the cache and the button")
    ap.add_argument("--wait", type=float, default=WAIT_S, help=f"seconds to wait for the button (default {int(WAIT_S)})")
    ap.add_argument("--no-build", action="store_true", help="push webapp/dist/app.pack as it is, without rebuilding")
    ap.add_argument("--forget-token", action="store_true", help="clear the cached token before running (forces the button)")
    args = ap.parse_args()

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
        if not PACK.is_file():
            die(f"missing {PACK}; run without --no-build")
        data = PACK.read_bytes()
    else:
        data = build_pack()
    try:
        build_id, assets = app_pack.unpack(data)
    except app_pack.PackError as e:
        die(f"{PACK} is not a pack ({e}); run without --no-build")
    page = dict(assets).get("app.html")
    print(f"pushing {build_id}: {len(assets)} assets, {len(data)} bytes" + (f", app.html fnv1a {fnv1a(page)}" if page else ""))

    token = args.token.strip()
    try:
        if token:
            print("using --token from CLI (not cached)")
            served = push(host, token, data)
        else:
            served = with_token_retry(host, args.wait, lambda tok: push(host, tok, data))
    except SidekickError as e:
        die(str(e))
    if served != build_id:
        die(f"the sidekick says it serves '{served}', not '{build_id}'")
    print(f"done: {url(host, '/app.html')} is {build_id}, until this sidekick's firmware next changes")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
