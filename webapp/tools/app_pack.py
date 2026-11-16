#!/usr/bin/env python3
"""The web app as one file: the builder (docs/architecture/sidekick.md, Web root).

The layout is in sidekick/main/app_pack.h, whose reader is tested against what this writes
(host_mock/test_app_pack.cpp reads `--sample`'s output). webapp/tools/build.py calls pack() on the
built app.

  python3 webapp/tools/app_pack.py --sample out.bin   # the fixed pack the C reader's test reads
  python3 webapp/tools/app_pack.py --list some.pack   # a pack's build id and assets

Python's standard library only.
"""

from __future__ import annotations

import argparse
import re
import struct
import sys
from pathlib import Path

MAGIC = b"MFT8PACK"
FORMAT = 1
BUILD_MAX = 48  # NUL included
NAME_MAX = 56   # NUL included
HEAD = struct.Struct("<8sHHI48s")
ENTRY = struct.Struct("<56sII")
NAME_RE = re.compile(r"[A-Za-z0-9_-][A-Za-z0-9._-]*")
BUILD_RE = re.compile(r"[ !#-\[\]-~]+")  # printable ASCII, no quote or backslash: it goes into JSON as it is

# What `--sample` packs. host_mock/test_app_pack.cpp holds the same names, sizes and bytes: change
# both together.
SAMPLE_BUILD = "b1322+d18f9b2"
SAMPLE = [
    ("app.html", b"\x1f\x8b" + bytes(range(256)) * 3),
    ("bands.js", b"var BANDS=1;\n"),
    ("empty.json", b""),
]


class PackError(Exception):
    pass


def pack(assets: list[tuple[str, bytes]], build: str) -> bytes:
    """One pack from (name, bytes) pairs, sorted by name, stamped `build`."""
    if not assets:
        raise PackError("no assets to pack")
    if not BUILD_RE.fullmatch(build) or len(build.encode()) >= BUILD_MAX:
        raise PackError(f"build id {build!r} is empty, too long or not plain ASCII")
    assets = sorted(assets)
    names = [name for name, _ in assets]
    if len(set(names)) != len(names):
        raise PackError("an asset is named twice")
    for name in names:
        if not NAME_RE.fullmatch(name) or len(name.encode()) >= NAME_MAX:
            raise PackError(f"asset name {name!r} is not a plain file name under {NAME_MAX} bytes")
    offset = HEAD.size + ENTRY.size * len(assets)
    index, body = [], []
    for name, data in assets:
        index.append(ENTRY.pack(name.encode(), offset, len(data)))
        body.append(data)
        offset += len(data)
    head = HEAD.pack(MAGIC, FORMAT, len(assets), offset, build.encode())
    return head + b"".join(index) + b"".join(body)


def unpack(data: bytes) -> tuple[str, list[tuple[str, bytes]]]:
    """A pack's build id and its (name, bytes) pairs, by the rules the sidekick's reader holds."""
    if len(data) < HEAD.size:
        raise PackError("short")
    magic, fmt, count, total, build = HEAD.unpack_from(data)
    if magic != MAGIC:
        raise PackError("magic")
    if fmt != FORMAT:
        raise PackError("format")
    if total > len(data):
        raise PackError("length")
    index_end = HEAD.size + ENTRY.size * count
    if count == 0 or index_end > total:
        raise PackError("index")
    out = []
    for i in range(count):
        raw, offset, size = ENTRY.unpack_from(data, HEAD.size + ENTRY.size * i)
        name = raw.split(b"\0", 1)[0].decode("ascii", "replace")
        if b"\0" not in raw or not NAME_RE.fullmatch(name):
            raise PackError("name")
        if offset < index_end or offset > total or size > total - offset:
            raise PackError("bounds")
        out.append((name, data[offset:offset + size]))
    text = build.split(b"\0", 1)[0].decode("ascii", "replace")
    if b"\0" not in build or not BUILD_RE.fullmatch(text):
        raise PackError("build")
    return text, out


def pack_dir(root: Path, build: str, skip: tuple[str, ...] = ()) -> bytes:
    """A pack of every file directly under `root` but dotfiles and the names in `skip`."""
    assets = [
        (p.name, p.read_bytes())
        for p in sorted(root.iterdir())
        if p.is_file() and p.name not in skip and not p.name.startswith(".")
    ]
    return pack(assets, build)


def write_if_changed(path: Path, data: bytes) -> bool:
    """Writes `data` to `path` unless it already holds it. A build that embeds the pack then relinks
    only when the app changed. True if it wrote."""
    if path.is_file() and path.read_bytes() == data:
        return False
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)
    return True


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--sample", type=Path, help="write the fixed sample pack here")
    ap.add_argument("--list", type=Path, help="print this pack's build id and assets")
    args = ap.parse_args()
    if args.sample:
        write_if_changed(args.sample, pack(SAMPLE, SAMPLE_BUILD))
    if args.list:
        try:
            build, assets = unpack(args.list.read_bytes())
        except PackError as e:
            print(f"{args.list}: not a pack ({e})", file=sys.stderr)
            return 1
        print(f"build {build}")
        for name, data in assets:
            print(f"{len(data):8d}  {name}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
