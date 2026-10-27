#!/usr/bin/env python3
"""Write manifest.json for a built web bundle (docs/architecture/sidekick.md (Installing the app bundle)).

The manifest is part of the build's output, never committed: webapp/tools/build.py
calls write() on webapp/dist/app as its last step, so the digests always match
the exact bytes that ship.

Usage (normally via build.py):
  python3 webapp/tools/gen_manifest.py webapp/dist/app
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path

API_MIN = 2
MANIFEST_NAME = "manifest.json"


def assets(root: Path) -> list[dict]:
    out = []
    for path in sorted(root.iterdir()):
        if not path.is_file() or path.name == MANIFEST_NAME or path.name.startswith("."):
            continue
        data = path.read_bytes()
        out.append(
            {
                "path": path.name,
                "sha256": hashlib.sha256(data).hexdigest(),
                "size": len(data),
            }
        )
    if not out:
        raise SystemExit(f"no assets under {root}")
    return out


def build(root: Path) -> dict:
    return {"api_min": API_MIN, "assets": assets(root)}


def write(root: Path) -> dict:
    doc = build(root)
    (root / MANIFEST_NAME).write_text(json.dumps(doc, indent=2) + "\n")
    return doc


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("root", type=Path, help="bundle directory, e.g. webapp/dist/app")
    args = ap.parse_args()
    doc = write(args.root)
    print(f"wrote {args.root / MANIFEST_NAME} ({len(doc['assets'])} assets)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
