#!/usr/bin/env python3
"""Install a locally built firmware image on a board over the air, the way the
web app's Settings > Updates does (docs/architecture/sidekick.md (Firmware
updates)), but from a file on this machine instead of the release host.

It serves the image over plain HTTP on this machine's LAN address, hands the
sidekick its URL, size, SHA-256 and built-in version behind the pairing token
(sidekick.py caches it), then follows /api/fw/status to the result. The
sidekick must be on the same network as this machine, joined to WiFi (not on
its own access point).

Every update stops the radio first (the main board's update mode), and it
stays stopped afterwards until Resume operating in Settings, or --resume here.

Usage:
  python3 webapp/tools/install_firmware.py                    # sidekick/build/sidekick.bin
  python3 webapp/tools/install_firmware.py --board main       # poe_p4/build/poe_p4.bin, sent on down Port A
  python3 webapp/tools/install_firmware.py --both             # one request: the main board, then the sidekick
  python3 webapp/tools/install_firmware.py --resume           # take the main board out of update mode
  python3 webapp/tools/install_firmware.py --image path/to/sidekick.bin
  python3 webapp/tools/install_firmware.py --host http://192.168.50.128
  python3 webapp/tools/install_firmware.py --wrong-hash       # the sidekick must refuse it: FAILED: hash
  python3 webapp/tools/install_firmware.py --status           # print /api/fw/status and exit
"""

from __future__ import annotations

import argparse
import functools
import hashlib
import http.server as http_server  # `http` below is sidekick.py's request helper
import json
import socket
import struct
import sys
import threading
import time
import urllib.parse
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from sidekick import ROOT, AuthError, SidekickError, die, http, resolve_base, url, with_token_retry  # noqa: E402

DEFAULT_HOST = "http://micro-ft8.local"
IMAGES = {
    "sidekick": ROOT / "sidekick" / "build" / "sidekick.bin",
    "main": ROOT / "poe_p4" / "build" / "poe_p4.bin",
}
WAIT_S = 120.0
FOLLOW_S = 8 * 60


def image_version(data: bytes) -> str:
    """The version string in an app image's esp_app_desc_t (tools/make_manifest.py reads the same)."""
    if len(data) < 288 or data[0] != 0xE9 or struct.unpack_from("<I", data, 32)[0] != 0xABCD5432:
        die("not an ESP-IDF app image (a merged 0x0 image is not one)")
    return data[48:80].split(b"\0", 1)[0].decode("ascii", "replace")


def lan_address(sidekick_base: str) -> str:
    """This machine's address on the route to the sidekick."""
    host = urllib.parse.urlsplit(sidekick_base).hostname
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        s.connect((host, 80))
        return s.getsockname()[0]


def serve(directory: Path, bind: str) -> http_server.ThreadingHTTPServer:
    class Quiet(http_server.SimpleHTTPRequestHandler):
        def log_message(self, fmt, *args):
            print("  served: " + fmt % args)

    server = http_server.ThreadingHTTPServer((bind, 0), functools.partial(Quiet, directory=str(directory)))
    threading.Thread(target=server.serve_forever, daemon=True).start()
    return server


def status(base: str) -> dict | None:
    try:
        code, body = http("GET", url(base, "/api/fw/status"), timeout=5.0)
    except SidekickError:
        return None
    if code != 200:
        die(f"GET /api/fw/status: HTTP {code} (this sidekick's firmware has no over-the-air update; flash it over USB once)")
    return json.loads(body)


def start(base: str, body: bytes, token: str) -> None:
    code, reply = http("POST", url(base, "/api/fw/start"), token=token, body=body, content_type="text/plain")
    if code == 401:
        raise AuthError()
    if code != 200:
        die(f"POST /api/fw/start: HTTP {code} {reply.decode('utf-8', 'replace')}")


def resume(base: str, token: str) -> None:
    code, reply = http("POST", url(base, "/api/fw/resume"), token=token, body=b"")
    if code == 401:
        raise AuthError()
    if code != 200:
        die(f"POST /api/fw/resume: HTTP {code} {reply.decode('utf-8', 'replace')}")


def follow(base: str, board: str, target: str) -> int:
    began = time.monotonic()
    last_line = ""
    seen_busy = False
    forgotten = 0
    while time.monotonic() - began < FOLLOW_S:
        st = status(base)
        # Answering, idle, and no word of this update: the sidekick lost power or restarted partway
        # and forgot it. Its own update's restart is not that: `last` names the target then.
        busy = st is not None and st["state"] not in ("idle", "done", "failed")
        seen_busy = seen_busy or busy
        idle_blank = st is not None and st["state"] == "idle" and st.get("last", {}).get("version") != target
        forgotten = forgotten + 1 if (seen_busy and idle_blank) else 0
        if forgotten >= 10:
            print("FAILED: the sidekick restarted during the update and has no record of it")
            return 1
        on = (st or {}).get("host" if board == "main" else "running", "?")
        if st is None:
            line = "no answer (restarting)"
        elif st["state"] == "failed":
            print(f"{'ROLLED BACK' if st['why'] == 'rolled_back' else 'FAILED'}: {st['why']} (detail {st['detail']}); on {on}")
            return 1
        elif board == "main" and st["state"] == "done" and st["board"] == board and st["version"] == target:
            print(f"UPDATED: running {on}, {time.monotonic() - began:.0f} s")
            return 0
        elif st["state"] != "idle":
            line = f"{st['board']}: {st['state']}" + (
                f" {st['got']}/{st['size']}" if st["state"] in ("downloading", "transferring") else "")
        elif st.get("last", {}).get("version") == target and st["last"]["result"] != "confirming":
            ok = st["last"]["result"] == "ok"
            print(f"{'UPDATED' if ok else 'ROLLED BACK'}: running {on} (from {st['last']['from']}), "
                  f"{time.monotonic() - began:.0f} s")
            return 0 if ok else 1
        else:
            line = "confirming"
        if line != last_line:
            # A transfer's count moves every poll; say it on one line.
            end = "\r" if line.startswith(("downloading", "transferring")) and last_line.split(" ")[0] == line.split(" ")[0] else "\n"
            print("  " + line + " " * 8, end=end, flush=True)
            last_line = line
        time.sleep(1.0)
    print("FAILED: no result within 8 minutes")
    return 1


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n", 1)[0])
    ap.add_argument("--host", default=DEFAULT_HOST)
    ap.add_argument("--board", default="sidekick", choices=sorted(IMAGES))
    ap.add_argument("--image", type=Path)
    ap.add_argument("--wrong-hash", action="store_true")
    ap.add_argument("--status", action="store_true")
    ap.add_argument("--resume", action="store_true")
    ap.add_argument("--both", action="store_true")
    args = ap.parse_args()

    try:
        base = resolve_base(args.host)
    except SidekickError as e:
        die(str(e))
    if args.status:
        print(json.dumps(status(base), indent=2))
        return 0
    if args.resume:
        try:
            with_token_retry(base, WAIT_S, lambda token: resume(base, token))
        except SidekickError as e:
            die(str(e))
        print("the main board is operating again")
        return 0

    before = status(base)
    if before is None:
        die(f"no answer from {base}")
    # --both is the plan the web app sends: the main board's image, a `next` line, the sidekick's.
    boards = ["main", "sidekick"] if args.both else [args.board]
    blocks, targets = [], {}
    for board in boards:
        image = args.image if (args.image and not args.both) else IMAGES[board]
        if not image.is_file():
            die(f"{image} not found; build it first")
        data = image.read_bytes()
        version = image_version(data)
        targets[board] = version
        running = before.get("host", "") if board == "main" else before["running"]
        print(f"{board}: running {running or 'unknown'}, installing {version} ({len(data)} bytes) from {image}")
        server = serve(image.parent, lan_address(base))
        image_url = f"http://{server.server_address[0]}:{server.server_address[1]}/{urllib.parse.quote(image.name)}"
        blocks.append(
            f"board={board}\nurl={image_url}\nsize={len(data)}\n"
            f"sha256={'0' * 64 if args.wrong_hash else hashlib.sha256(data).hexdigest()}\nversion={version}\n"
        )
    body = "next\n".join(blocks).encode()
    try:
        with_token_retry(base, WAIT_S, lambda token: start(base, body, token))
    except SidekickError as e:
        die(str(e))

    # The plan's last board is the one whose result ends it; a failure at any step ends it sooner.
    last = boards[-1]
    rc = follow(base, last, targets[last])
    if rc == 0 and args.both:
        st = status(base) or {}
        if st.get("host") != targets["main"]:
            print(f"FAILED: the main board is on {st.get('host')!r}, not {targets['main']}")
            rc = 1
        else:
            print(f"both boards updated: main {st.get('host')}, sidekick {st.get('running')}")
    print("the radio is stopped; Resume operating in Settings, or: python3 webapp/tools/install_firmware.py --resume")
    return rc


if __name__ == "__main__":
    sys.exit(main())
