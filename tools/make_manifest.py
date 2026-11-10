#!/usr/bin/env python3
"""Write /download/manifest.json for the rolling release (docs/RELEASE_PROCESS.md).

  python3 tools/make_manifest.py --commit <40-hex> --date YYYYMMDD --out dist/manifest.json \\
      --asset poe_p4=dist/<dated>.bin --ota poe_p4=dist/<dated-ota>.bin \\
      --asset sidekick=dist/<dated>.bin --ota sidekick=dist/<dated-ota>.bin \\
      --asset source=dist/<dated>.tar.gz --asset getting_started=dist/<dated>.pdf

Each asset's SHA-256 is computed here, from the file that is uploaded. An
`ota` entry describes a board's app image, the one an over-the-air update
writes (docs/roadmap/ota-updates.md, Manifest): its size, hash, the version
string built into it, and the Port A protocol version this tree speaks.

It refuses to write a manifest for an OTA image a board would refuse or could
not hold: not an app image, built for another board, a version other than the
commit's (a board checks the image's own version against the one it was told
to expect), or larger than the sidekick's staging slot. CI runs it before
anything is published, so a bad image publishes nothing.

Python's standard library only.
"""
import argparse
import datetime
import hashlib
import json
import os
import re
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PROTO_HEADER = os.path.join(ROOT, "components", "porta_proto", "include", "porta_proto.h")

# kind -> (stable name's stem, extension, extra fields). The stable names are
# the R2 keys the site and the printed sheet link to; they never change.
ASSETS = {
    "poe_p4": ("microft8-poe-p4-continuous", ".bin", {"chip": "esp32p4", "flash_offset": "0x0"}),
    "sidekick": ("microft8-sidekick-continuous", ".bin", {"chip": "esp32s3", "flash_offset": "0x0"}),
    "source": ("microft8-source-continuous", ".tar.gz", {}),
    "getting_started": ("microft8-getting-started-continuous", ".pdf", {}),
}
# board -> the project name its app image carries (project() in its CMakeLists.txt).
OTA_PROJECT = {"poe_p4": "poe_p4", "sidekick": "sidekick"}

# Every image is staged in the sidekick's spare OTA slot before it goes
# anywhere, a main board's included, so that slot bounds them all
# (sidekick/partitions.csv: ota_0 and ota_1 are 0x300000 each).
OTA_MAX_BYTES = 0x300000

# An ESP-IDF app image: a 24-byte image header whose first byte is 0xE9, an
# 8-byte segment header, then esp_app_desc_t (esp_app_desc.h).
IMAGE_MAGIC = 0xE9
APP_DESC_OFFSET = 32
APP_DESC_MAGIC = 0xABCD5432
APP_DESC_VERSION = APP_DESC_OFFSET + 16  # char version[32]
APP_DESC_PROJECT = APP_DESC_OFFSET + 48  # char project_name[32]


class ManifestError(Exception):
    pass


def sha256_of(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 16), b""):
            h.update(block)
    return h.hexdigest()


def stable_name(kind, ota=False):
    stem, ext, _ = ASSETS[kind]
    return stem + ("-ota" if ota else "") + ext


def proto_version(header_path=PROTO_HEADER):
    with open(header_path, encoding="utf-8") as f:
        m = re.search(r"^#define\s+PORTA_PROTO_VERSION\s+(\d+)u?\s*$", f.read(), re.M)
    if not m:
        raise ManifestError(f"no PORTA_PROTO_VERSION in {header_path}")
    return int(m.group(1))


def _cstr(buf, offset):
    return buf[offset:offset + 32].split(b"\0", 1)[0].decode("ascii", "replace")


def app_image_info(path):
    """(version, project name) from an app image's esp_app_desc_t."""
    with open(path, "rb") as f:
        head = f.read(APP_DESC_OFFSET + 256)
    if len(head) < APP_DESC_OFFSET + 256 or head[0] != IMAGE_MAGIC:
        raise ManifestError(f"{path}: not an ESP-IDF app image (a merged 0x0 image starts with the bootloader)")
    if struct.unpack_from("<I", head, APP_DESC_OFFSET)[0] != APP_DESC_MAGIC:
        raise ManifestError(f"{path}: no app description where an app image has one")
    return _cstr(head, APP_DESC_VERSION), _cstr(head, APP_DESC_PROJECT)


def ota_entry(board, path, short, proto):
    version, project = app_image_info(path)
    if project != OTA_PROJECT[board]:
        raise ManifestError(f"{path}: built as '{project}', not {board}'s '{OTA_PROJECT[board]}'")
    if version != short:
        raise ManifestError(f"{path}: reports version '{version}', but this release is '{short}'")
    size = os.path.getsize(path)
    if size > OTA_MAX_BYTES:
        raise ManifestError(f"{path}: {size} bytes will not fit the sidekick's {OTA_MAX_BYTES}-byte staging slot")
    return {
        "stable": stable_name(board, ota=True),
        "filename": os.path.basename(path),
        "size": size,
        "sha256": sha256_of(path),
        "version": version,
        "proto": proto,
    }


def build(commit, date, assets, otas, proto, generated_at):
    """The manifest as a dict. `assets` and `otas` map a kind to a file path."""
    if not re.fullmatch(r"[0-9a-f]{40}", commit):
        raise ManifestError(f"--commit wants a full 40-hex SHA, not '{commit}'")
    short = commit[:7]
    for kind in list(assets) + list(otas):
        if kind not in ASSETS:
            raise ManifestError(f"unknown asset kind '{kind}'")
    for board in otas:
        if board not in OTA_PROJECT:
            raise ManifestError(f"'{board}' has no OTA image")
        if board not in assets:
            raise ManifestError(f"--ota {board} without --asset {board}")
    out = {}
    for kind, path in assets.items():
        entry = {"stable": stable_name(kind), "filename": os.path.basename(path), "sha256": sha256_of(path)}
        entry.update(ASSETS[kind][2])
        if kind in otas:
            entry["ota"] = ota_entry(kind, otas[kind], short, proto)
        out[kind] = entry
    return {"commit": commit, "short": short, "date": date, "generated_at": generated_at, "assets": out}


def _pairs(values, flag):
    out = {}
    for v in values or []:
        kind, sep, path = v.partition("=")
        if not sep or not path:
            raise ManifestError(f"{flag} wants kind=path, not '{v}'")
        if kind in out:
            raise ManifestError(f"{flag} {kind} given twice")
        out[kind] = path
    return out


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    ap.add_argument("--commit", required=True)
    ap.add_argument("--date", required=True, help="the commit's date, YYYYMMDD, UTC")
    ap.add_argument("--asset", action="append", metavar="KIND=PATH")
    ap.add_argument("--ota", action="append", metavar="BOARD=PATH")
    ap.add_argument("--out", required=True)
    args = ap.parse_args(argv)
    try:
        manifest = build(
            args.commit, args.date, _pairs(args.asset, "--asset"), _pairs(args.ota, "--ota"), proto_version(),
            datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        )
    except (ManifestError, OSError) as e:
        print(f"make_manifest: {e}", file=sys.stderr)
        return 1
    with open(args.out, "w", encoding="utf-8", newline="\n") as f:
        json.dump(manifest, f, indent=2)
        f.write("\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
