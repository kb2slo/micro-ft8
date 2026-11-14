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

    def test_build_json_sits_beside_the_manifest_and_is_not_an_asset(self):
        info = build.write_build_json(self.root, "d18f9b2 2026-11-13", self.doc)
        on_disk = json.loads((self.root / gen_manifest.BUILD_NAME).read_text())
        self.assertEqual(on_disk, info)
        self.assertEqual(on_disk["build"], "d18f9b2 2026-11-13")
        self.assertEqual(on_disk["size"], sum(a["size"] for a in self.on_disk["assets"]))
        # Written after the manifest, and a rebuild of the manifest still leaves it out.
        again = gen_manifest.write(self.root)
        self.assertNotIn(gen_manifest.BUILD_NAME, [a["path"] for a in again["assets"]])
        # Never gzipped: Pages serves it as plain JSON, with no Content-Encoding.
        fresh = Path(self.tmp.name) / "fresh"
        fresh.mkdir()
        (fresh / gen_manifest.BUILD_NAME).write_text(json.dumps({"build": "x " * 400, "size": 1}))
        build.compress_assets(fresh)
        self.assertEqual((fresh / gen_manifest.BUILD_NAME).read_bytes()[:1], b"{")

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


class BuildId(unittest.TestCase):
    def test_a_build_is_stamped_with_its_commit(self):
        self.assertEqual(build.stamp("d18f9b2", False, "2026-11-13"), "d18f9b2 2026-11-13")
        self.assertEqual(build.stamp("d18f9b2", True, "2026-11-13"), "d18f9b2-dirty 2026-11-13")

    def test_a_release_is_stamped_with_its_version(self):
        self.assertEqual(build.stamp("d18f9b2", False, "2026-11-13", "0.1.0+d18f9b2"), "0.1.0+d18f9b2 2026-11-13")
        # git's short SHA grows with the repository; the version's is always seven.
        self.assertEqual(build.stamp("d18f9b2c4", False, "2026-11-13", "0.1.0+d18f9b2"), "0.1.0+d18f9b2 2026-11-13")

    def test_a_changed_tree_never_takes_a_version(self):
        self.assertEqual(build.stamp("d18f9b2", True, "2026-11-13", "0.1.0+d18f9b2"), "d18f9b2-dirty 2026-11-13")

    def test_refuses_a_version_that_is_not_a_release_of_the_commit(self):
        for version in ("0.1.0+e6e4d88", "b1322+d18f9b2", "0.1.0", "d18f9b2"):
            with self.assertRaises(SystemExit):
                build.stamp("d18f9b2", False, "2026-11-13", version)


if __name__ == "__main__":
    unittest.main()
