#!/usr/bin/env python3
"""Build the web app bundle: webapp/app (hand-edited sources) -> webapp/dist/app (what ships).

docs/architecture/webapp.md (Build). Steps, in order:
  1. copy the sources (never manifest.json or dotfiles);
  2. stamp the build id into app.html (its __APP_BUILD__ placeholder);
  3. minify HTML and JS and compile-check every script (minify.mjs);
  4. compact the JSON data files;
  5. write manifest.json from the output;
  6. fail if a file or the whole bundle breaks the size budget.

Pages, CI and desk_push.py all run this, so what is tested is what ships.

Usage:
  npm ci --prefix webapp/tools      # once per clone, and after package-lock.json changes
  python3 webapp/tools/build.py
"""

from __future__ import annotations

import datetime
import json
import shutil
import subprocess
import sys
from pathlib import Path

TOOLS = Path(__file__).resolve().parent
ROOT = TOOLS.parents[1]
SRC = ROOT / "webapp" / "app"
OUT = ROOT / "webapp" / "dist" / "app"

sys.path.insert(0, str(TOOLS))
import gen_manifest  # noqa: E402

BUILD_PLACEHOLDER = "__APP_BUILD__"

# web_fs.c refuses a file over this (sidekick/main/web_fs.c).
FILE_MAX = 512 * 1024
# The sidekick's web partition is 0x1E0000 (1,920 KB, sidekick/partitions.csv).
# A bundle install stages a second copy before it renames into place, and the
# reference store's proposed cap is 256 KB (RFC 0008 §4.2), so two bundles plus the
# store plus the bootstrap pages and LittleFS overhead must fit:
# 2 x 800 + 256 = 1,856 KB leaves 64 KB. Raising this means shrinking the store cap.
BUNDLE_MAX = 800 * 1024


def die(msg: str) -> None:
    print(f"build: {msg}", file=sys.stderr)
    raise SystemExit(1)


def build_id() -> str:
    """'<short sha>[-dirty] <UTC date>'; 'unknown' outside a git checkout."""
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
    return f"{sha}{'-dirty' if dirty else ''} {date}"


def compact_json(path: Path) -> None:
    text = path.read_text(encoding="utf-8")
    compact = json.dumps(json.loads(text), separators=(",", ":"), ensure_ascii=False)
    if len(compact.encode("utf-8")) < len(text.encode("utf-8")):
        path.write_text(compact, encoding="utf-8")


def build() -> dict:
    if not (TOOLS / "node_modules" / "html-minifier-terser").is_dir():
        die("minifier not installed; run: npm ci --prefix webapp/tools")

    if OUT.exists():
        shutil.rmtree(OUT)
    OUT.mkdir(parents=True)
    for src in sorted(SRC.iterdir()):
        if src.is_file() and src.name != gen_manifest.MANIFEST_NAME and not src.name.startswith("."):
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

    doc = gen_manifest.write(OUT)

    src_total = sum(p.stat().st_size for p in SRC.iterdir() if p.is_file() and p.name != gen_manifest.MANIFEST_NAME)
    out_total = sum(p.stat().st_size for p in OUT.iterdir() if p.is_file())
    too_big = [a for a in doc["assets"] if a["size"] > FILE_MAX]
    for a in too_big:
        print(f"build: {a['path']} is {a['size']} bytes; the sidekick refuses files over {FILE_MAX}", file=sys.stderr)
    print(f"build {bid}: {len(doc['assets'])} assets, {src_total} -> {out_total} bytes "
          f"({out_total * 100 // max(1, src_total)}% of source; budget {BUNDLE_MAX})")
    if too_big:
        die("a file is over the sidekick's per-file limit")
    if out_total > BUNDLE_MAX:
        die(f"bundle is {out_total} bytes, over the {BUNDLE_MAX}-byte budget (see BUNDLE_MAX)")
    return doc


if __name__ == "__main__":
    build()
