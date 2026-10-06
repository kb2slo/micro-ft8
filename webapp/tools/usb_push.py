#!/usr/bin/env python3
"""USB-C half of desk_push.py: pack webapp/dist/app into a LittleFS image and
write it to the sidekick's `web` partition with esptool, no network needed.

The bundle API (docs/architecture/sidekick.md (Installing the app bundle))
leaves plain files on the web filesystem and nothing else (no manifest is kept),
so an image holding the same files is the same install. The seed pages are left
out: web_fs_init() hydrates any that are missing on the next boot. Unlike the
bundle API this replaces the whole filesystem, so files from an earlier bundle
that the new build dropped are gone too. NVS (WiFi, pairing token) is untouched.

Needs `pip install littlefs-python` (esptool comes with the IDF environment).
"""

from __future__ import annotations

import csv
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
APP = ROOT / "webapp" / "dist" / "app"
PARTITIONS = ROOT / "sidekick" / "partitions.csv"
LABEL = "web"
BLOCK_SIZE = 4096  # esp_littlefs' erase-sector block size on SPI flash
# The firmware's CONFIG_LITTLEFS_* (sidekick/sdkconfig). name_max is recorded in
# the image and a device with a smaller one refuses to mount it, after which
# format_if_mount_failed wipes it, so it must not exceed OBJ_NAME_LEN.
NAME_MAX = 64
READ_SIZE = PROG_SIZE = 128
CACHE_SIZE = 512
LOOKAHEAD_SIZE = 128
MAX_FILE = 512 * 1024  # web_fs.c refuses a larger file


def web_partition() -> tuple[int, int]:
    """(offset, size) of the `web` partition, read from the firmware's own table."""
    rows = csv.reader(
        line for line in PARTITIONS.read_text().splitlines() if line.strip() and not line.startswith("#")
    )
    for row in rows:
        cols = [c.strip() for c in row]
        if cols[0] == LABEL:
            return int(cols[3], 0), int(cols[4], 0)
    raise SystemExit(f"no `{LABEL}` partition in {PARTITIONS}")


def build_image(size: int) -> bytes:
    try:
        from littlefs import LittleFS
    except ImportError:
        raise SystemExit("USB push needs littlefs-python: pip install littlefs-python")
    fs = LittleFS(
        block_size=BLOCK_SIZE,
        block_count=size // BLOCK_SIZE,
        name_max=NAME_MAX,
        read_size=READ_SIZE,
        prog_size=PROG_SIZE,
        cache_size=CACHE_SIZE,
        lookahead_size=LOOKAHEAD_SIZE,
    )
    files = sorted(p for p in APP.rglob("*") if p.is_file() and p.name != "manifest.json")
    if not files:
        raise SystemExit(f"{APP} has no files; run without --no-build")
    for p in files:
        rel = p.relative_to(APP).as_posix()
        data = p.read_bytes()
        if any(len(part) > NAME_MAX for part in rel.split("/")):
            raise SystemExit(f"{rel}: a name over {NAME_MAX} characters, the firmware's LITTLEFS_OBJ_NAME_LEN")
        if len(data) > MAX_FILE:
            raise SystemExit(f"{rel}: {len(data)} bytes, over the {MAX_FILE} the sidekick accepts")
        parent = ""
        for part in rel.split("/")[:-1]:
            parent = f"{parent}/{part}" if parent else part
            try:
                fs.mkdir(parent)
            except FileExistsError:
                pass
        with fs.open(rel, "wb") as f:
            f.write(data)
    print(f"packed {len(files)} files into a {size // 1024} KB LittleFS image")
    return bytes(fs.context.buffer)


def push(port: str) -> None:
    offset, size = web_partition()
    image = build_image(size)
    with tempfile.TemporaryDirectory() as tmp:
        img = Path(tmp) / "web.bin"
        img.write_bytes(image)
        cmd = [sys.executable, "-m", "esptool", "--chip", "esp32s3"]
        if port:
            cmd += ["--port", port]
        cmd += ["write_flash", hex(offset), str(img)]
        print("+", " ".join(cmd))
        if subprocess.run(cmd).returncode != 0:
            raise SystemExit("esptool failed; the web partition may be half-written, re-run to retry")
    print("done: the sidekick restarted; hydrate re-adds the seed pages on boot")
