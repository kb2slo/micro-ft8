#!/usr/bin/env python3
"""Build the web app bundle: webapp/app (hand-edited sources) -> webapp/dist/app (what ships).

docs/architecture/webapp.md (Build). Steps, in order:
  1. copy the sources (never manifest.json or dotfiles);
  2. stamp the build id into app.html (its __APP_BUILD__ placeholder);
  3. minify HTML and JS and compile-check every script (minify.mjs);
  4. compact the JSON data files;
  5. gzip each asset where that shrinks it, keeping its name;
  6. write manifest.json from the output, so it lists the bytes as shipped;
  7. fail if a file or the whole bundle breaks the size budget (compressed bytes);
  8. write build.json beside it: the build id and the bundle's size, for the page's update check.
  9. write webapp/dist/app.pack: the same assets as one file (app_pack.py; sidekick/main/app_pack.h).

Pages, CI and install_app.py all run this, so what is tested is what ships.

Usage:
  npm ci --prefix webapp/tools      # once per clone, and after package-lock.json changes
  python3 webapp/tools/build.py
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
import gen_manifest  # noqa: E402

sys.path.insert(0, str(TOOLS.parents[1] / "tools"))
import build_version  # noqa: E402

BUILD_PLACEHOLDER = "__APP_BUILD__"

# web_fs.c refuses a file over this (sidekick/main/web_fs.c).
FILE_MAX = 512 * 1024
# The sidekick's web partition is 0x1E0000 (1,920 KB, sidekick/partitions.csv).
# A bundle install stages a second copy before it renames into place, and the
# reference store's proposed cap is 256 KB (docs/roadmap/activator-mode.md), so two bundles plus the
# store plus the bootstrap pages and LittleFS overhead must fit:
# 2 x 800 + 256 = 1,856 KB leaves 64 KB. Raising this means shrinking the store cap.
BUNDLE_MAX = 800 * 1024
# gzip's magic. The sidekick keeps no manifest, so web_fs.c tells a compressed
# asset by these first two bytes and serves it with Content-Encoding: gzip.
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
        if not path.is_file() or path.name in gen_manifest.NOT_ASSETS:
            continue
        data = path.read_bytes()
        if data[:2] == GZIP_MAGIC:
            die(f"{path.name} starts with gzip's magic bytes; the sidekick would serve it as compressed")
        packed = gzip.compress(data, compresslevel=9, mtime=0)
        if len(packed) < len(data):
            path.write_bytes(packed)


def build() -> dict:
    if not (TOOLS / "node_modules" / "html-minifier-terser").is_dir():
        die("minifier not installed; run: npm ci --prefix webapp/tools")

    if OUT.exists():
        shutil.rmtree(OUT)
    OUT.mkdir(parents=True)
    for src in sorted(SRC.iterdir()):
        if src.is_file() and src.name not in gen_manifest.NOT_ASSETS and not src.name.startswith("."):
            shutil.copy2(src, OUT / src.name)

    app = OUT / "app.html"
    text = app.read_text(encoding="utf-8")
    if text.count(BUILD_PLACEHOLDER) != 1:
        die(f"app.html must contain {BUILD_PLACEHOLDER} exactly once")
    bid = build_id()
    app.write_text(text.replace(BUILD_PLACEHOLDER, bid), encoding="utf-8")

    result = subprocess.run(["node", str(TOOLS / "minify.mjs"), str(OUT)])
    if result.returncode != 0:
        die("minify failed")

    for path in sorted(OUT.glob("*.json")):
        compact_json(path)

    compress_assets(OUT)
    doc = gen_manifest.write(OUT)

    src_total = sum(p.stat().st_size for p in SRC.iterdir() if p.is_file() and p.name != gen_manifest.MANIFEST_NAME)
    out_total = sum(p.stat().st_size for p in OUT.iterdir() if p.is_file())
    too_big = [a for a in doc["assets"] if a["size"] > FILE_MAX]
    for a in too_big:
        print(f"build: {a['path']} is {a['size']} bytes; the sidekick refuses files over {FILE_MAX}", file=sys.stderr)
    print(f"build {bid}: {len(doc['assets'])} assets, {src_total} -> {out_total} bytes as shipped, gzipped "
          f"({out_total * 100 // max(1, src_total)}% of source; budget {BUNDLE_MAX})")
    if too_big:
        die("a file is over the sidekick's per-file limit")
    if out_total > BUNDLE_MAX:
        die(f"bundle is {out_total} bytes, over the {BUNDLE_MAX}-byte budget (see BUNDLE_MAX)")
    write_build_json(OUT, bid, doc)
    write_pack(OUT, PACK, bid)
    return doc


def write_pack(root: Path, out: Path, bid: str) -> bytes:
    """The pack: the manifest's assets, as shipped, in one file stamped with the build id. Neither
    the manifest nor build.json is in it. Left alone when it would not change, so a build that
    embeds it relinks only when the app did."""
    try:
        data = app_pack.pack_dir(root, bid, skip=gen_manifest.NOT_ASSETS)
    except app_pack.PackError as e:
        die(str(e))
    app_pack.write_if_changed(out, data)
    return data


def write_build_json(root: Path, bid: str, doc: dict) -> dict:
    """build.json: what the page reads from the release host to tell whether its app is the
    published one (webapp/app/updates.js, appOffer()), and the bundle's size for its progress."""
    info = {"build": bid, "size": sum(a["size"] for a in doc["assets"])}
    (root / gen_manifest.BUILD_NAME).write_text(json.dumps(info) + "\n", encoding="utf-8")
    return info


if __name__ == "__main__":
    build()
