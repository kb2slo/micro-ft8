#!/usr/bin/env python3
"""Host test for the bundle's compress step and the manifest written after it
(webapp/tools/build.py, gen_manifest.py; docs/architecture/webapp.md, Build).

Synthetic assets in a temporary directory: no npm, no minifier, no sidekick.

  python3 webapp/tools/test_build.py
"""
import gzip
import hashlib
import json
import os
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import build  # noqa: E402
import gen_manifest  # noqa: E402

PAGE = ("<!doctype html><meta charset=utf-8><title>t</title>" + "<p>CQ KB2SLO EN52</p>" * 400).encode()
TINY = b"x"  # gzip's header alone is bigger than this


class CompressedBuild(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        (self.root / "app.html").write_bytes(PAGE)
        (self.root / "tiny.js").write_bytes(TINY)
        build.compress_assets(self.root)
        self.doc = gen_manifest.write(self.root)
        self.on_disk = json.loads((self.root / gen_manifest.MANIFEST_NAME).read_text())

    def tearDown(self):
        self.tmp.cleanup()

    def test_manifest_has_only_the_fields_the_sidekick_parses(self):
        # web_manifest.c refuses a field it does not know.
        self.assertEqual(set(self.on_disk), {"api_min", "assets"})
        for a in self.on_disk["assets"]:
            self.assertEqual(set(a), {"path", "sha256", "size"})

    def test_api_min_refuses_firmware_that_would_serve_gzip_raw(self):
        self.assertEqual(self.on_disk["api_min"], 3)

    def test_manifest_lists_the_bytes_as_shipped(self):
        for a in self.on_disk["assets"]:
            data = (self.root / a["path"]).read_bytes()
            self.assertEqual(a["size"], len(data), a["path"])
            self.assertEqual(a["sha256"], hashlib.sha256(data).hexdigest(), a["path"])

    def test_an_asset_that_shrinks_ships_gzipped_under_its_own_name(self):
        data = (self.root / "app.html").read_bytes()
        self.assertEqual(data[:2], build.GZIP_MAGIC)
        self.assertEqual(gzip.decompress(data), PAGE)
        self.assertLess(len(data), len(PAGE))

    def test_an_asset_that_would_grow_ships_as_it_was(self):
        self.assertEqual((self.root / "tiny.js").read_bytes(), TINY)

    def test_the_manifest_itself_is_not_compressed_or_listed(self):
        self.assertNotIn(gen_manifest.MANIFEST_NAME, [a["path"] for a in self.on_disk["assets"]])
        self.assertEqual((self.root / gen_manifest.MANIFEST_NAME).read_bytes()[:1], b"{")

    def test_same_input_builds_the_same_bytes(self):
        again = Path(self.tmp.name) / "again"
        again.mkdir()
        (again / "app.html").write_bytes(PAGE)
        build.compress_assets(again)
        self.assertEqual((again / "app.html").read_bytes(), (self.root / "app.html").read_bytes())


class RefusesAPlainAssetThatLooksCompressed(unittest.TestCase):
    def test_refused(self):
        with tempfile.TemporaryDirectory() as tmp:
            (Path(tmp) / "odd.json").write_bytes(build.GZIP_MAGIC + b"not gzip")
            with self.assertRaises(SystemExit):
                build.compress_assets(Path(tmp))


if __name__ == "__main__":
    unittest.main()
