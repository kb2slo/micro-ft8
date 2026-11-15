#!/usr/bin/env python3
"""Build the web app: webapp/app (hand-edited sources) -> webapp/dist/app and its pack (what ships).

docs/architecture/webapp.md (Build). Steps, in order:
  1. copy the sources (never dotfiles);
  2. stamp the build id into app.html (its __APP_BUILD__ placeholder);
  3. minify HTML and JS and compile-check every script (minify.mjs);
  4. compact the JSON data files;
  5. gzip each asset where that shrinks it, keeping its name;
  6. fail if the app breaks the size budget (compressed bytes);
  7. write webapp/dist/app.pack: the assets as one file (app_pack.py; sidekick/main/app_pack.h).

The sidekick's build, CI and install_app.py all run this, so what is tested is what ships.

Usage:
  npm ci --prefix webapp/tools      # once per clone, and after package-lock.json changes
  python3 webapp/tools/build.py
  python3 webapp/tools/build.py --version V --pack OUT
      # as the sidekick's build runs it (sidekick/main/CMakeLists.txt): the app stamped V, the
      # version the image reports, and the pack written to OUT too, for the image to embed

MICROFT8_APP_PACK names a pack already built, by a path from the repository's root: --pack then
copies it to OUT and builds nothing, and refuses one that is not stamped V. CI sets it, since
ESP-IDF's container has no Node: the workflow builds the pack on the runner first, from the same
checkout (.github/workflows/ci.yml).
"""

from __future__ import annotations

import datetime
import gzip
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

TOOLS = Path(__file__).resolve().parent
ROOT = TOOLS.parents[1]
SRC = ROOT / "webapp" / "app"
OUT = ROOT / "webapp" / "dist" / "app"
# The bundle as one file, beside it and never inside it: it is not one of its own assets.
PACK = ROOT / "webapp" / "dist" / "app.pack"

sys.path.insert(0, str(TOOLS))
import app_pack  # noqa: E402

sys.path.insert(0, str(TOOLS.parents[1] / "tools"))
import build_version  # noqa: E402

BUILD_PLACEHOLDER = "__APP_BUILD__"

# The pack is part of the sidekick's image, which must fit a firmware slot: 0x300000 (3,072 KB,
# sidekick/partitions.csv), with the sidekick's own code about 1,100 KB of it (measured
# 2026-11-15: an image of 1,288,160 bytes, 186,869 of them the pack). 800 KB keeps the image
# under 2 MB, a third of the slot spare for the code to grow. Raising this is a choice about the
# slot, not about the app alone.
BUNDLE_MAX = 800 * 1024
# gzip's magic. The pack carries no flag for it, so the sidekick tells a compressed asset by these
# first two bytes and serves it with Content-Encoding: gzip (sidekick/main/app_serve.c).
GZIP_MAGIC = b"\x1f\x8b"


def die(msg: str) -> None:
    print(f"build: {msg}", file=sys.stderr)
    raise SystemExit(1)


def stamp(sha: str, dirty: bool, date: str, version: str = "") -> str:
    """The build id: '<short sha>[-dirty] <UTC date>', or for a release '<version> <UTC date>'.

    `version` is what CI hands a release's build as MICROFT8_VERSION, '0.1.0+<sha>'
    (tools/build_version.py). As for the firmware (tools/git_version.cmake), a changed tree never
    takes it, and one that is not a release of this commit stops the build."""
    if dirty:
        return f"{sha}-dirty {date}"
    if version:
        info = build_version.parse(version)
        if not info or info["channel"] != "stable" or not sha.startswith(info["sha"]):
            die(f"MICROFT8_VERSION '{version}' is not a release of this commit, {sha}")
        return f"{version} {date}"
    return f"{sha} {date}"


def build_id() -> str:
    """stamp() for this checkout; 'unknown' outside a git checkout."""
    try:
        sha = subprocess.run(
            ["git", "rev-parse", "--short", "HEAD"], cwd=ROOT, capture_output=True, text=True, check=True
        ).stdout.strip()
        dirty = subprocess.run(
            ["git", "status", "--porcelain", "--", str(SRC)], cwd=ROOT, capture_output=True, text=True, check=True
        ).stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        return "unknown"
    date = datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%d")
    return stamp(sha, bool(dirty), date, os.environ.get("MICROFT8_VERSION", ""))


def compact_json(path: Path) -> None:
    text = path.read_text(encoding="utf-8")
    compact = json.dumps(json.loads(text), separators=(",", ":"), ensure_ascii=False)
    if len(compact.encode("utf-8")) < len(text.encode("utf-8")):
        path.write_text(compact, encoding="utf-8")


def compress_assets(root: Path) -> None:
    """gzip every asset in place, keeping its name; one compressing would not shrink ships as it is.

    mtime=0 and no file name in the header, so the same input builds the same
    bytes. A plain asset that already starts with gzip's magic would be served
    as compressed, so the build refuses one."""
    for path in sorted(root.iterdir()):
        if not path.is_file():
            continue
        data = path.read_bytes()
        if data[:2] == GZIP_MAGIC:
            die(f"{path.name} starts with gzip's magic bytes; the sidekick would serve it as compressed")
        packed = gzip.compress(data, compresslevel=9, mtime=0)
        if len(packed) < len(data):
            path.write_bytes(packed)


def build(version: str = "") -> None:
    """Builds the bundle and its pack. `version` stamps the app with exactly that, the version of
    the sidekick image it is built into; without it the stamp is build_id()'s."""
    if not (TOOLS / "node_modules" / "html-minifier-terser").is_dir():
        die("minifier not installed; run: npm ci --prefix webapp/tools")

    if OUT.exists():
        shutil.rmtree(OUT)
    OUT.mkdir(parents=True)
    for src in sorted(SRC.iterdir()):
        if src.is_file() and not src.name.startswith("."):
            shutil.copy2(src, OUT / src.name)

    app = OUT / "app.html"
    text = app.read_text(encoding="utf-8")
    if text.count(BUILD_PLACEHOLDER) != 1:
        die(f"app.html must contain {BUILD_PLACEHOLDER} exactly once")
    bid = version or build_id()
    app.write_text(text.replace(BUILD_PLACEHOLDER, bid), encoding="utf-8")

    try:
        result = subprocess.run(["node", str(TOOLS / "minify.mjs"), str(OUT)])
    except FileNotFoundError:
        die("node not on PATH: the web build needs Node (CONTRIBUTING.md, Setup)")
    if result.returncode != 0:
        die("minify failed")

    for path in sorted(OUT.glob("*.json")):
        compact_json(path)

    compress_assets(OUT)

    src_total = sum(p.stat().st_size for p in SRC.iterdir() if p.is_file())
    shipped = [p for p in OUT.iterdir() if p.is_file()]
    out_total = sum(p.stat().st_size for p in shipped)
    print(f"build {bid}: {len(shipped)} assets, {src_total} -> {out_total} bytes as shipped, gzipped "
          f"({out_total * 100 // max(1, src_total)}% of source; budget {BUNDLE_MAX})")
    if out_total > BUNDLE_MAX:
        die(f"the app is {out_total} bytes, over the {BUNDLE_MAX}-byte budget (see BUNDLE_MAX)")
    write_pack(OUT, PACK, bid)


def write_pack(root: Path, out: Path, bid: str) -> bytes:
    """The pack: the assets, as shipped, in one file stamped with the build id. Left alone when it
    would not change, so a build that embeds it relinks only when the app did."""
    try:
        data = app_pack.pack_dir(root, bid)
    except app_pack.PackError as e:
        die(str(e))
    app_pack.write_if_changed(out, data)
    return data


def prebuilt_pack() -> Path | None:
    """The pack MICROFT8_APP_PACK names, or None when it is unset."""
    named = os.environ.get("MICROFT8_APP_PACK", "")
    if not named:
        return None
    path = Path(named) if os.path.isabs(named) else ROOT / named
    if not path.is_file():
        die(f"MICROFT8_APP_PACK names {path}, which is not there")
    return path


def main() -> int:
    import argparse

    ap = argparse.ArgumentParser(description="Build the web app and its pack.")
    ap.add_argument("--pack", type=Path, help="also write the pack here, only if its bytes changed")
    ap.add_argument("--version", default="", help="stamp the app with this, the sidekick image's version")
    args = ap.parse_args()
    ready = prebuilt_pack() if args.pack else None
    if ready:
        data = ready.read_bytes()
        try:
            bid, assets = app_pack.unpack(data)
        except app_pack.PackError as e:
            die(f"{ready} is not a pack ({e})")
        # The image would serve an app that calls itself something the image does not.
        if args.version and bid != args.version:
            die(f"{ready} is stamped '{bid}', and this build is '{args.version}': build the pack with --version")
        print(f"build {bid}: {len(assets)} assets, {len(data)} bytes, as already built at {ready}")
    else:
        build(args.version)
        data = PACK.read_bytes()
    if args.pack:
        app_pack.write_if_changed(args.pack, data)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
