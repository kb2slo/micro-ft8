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
import app_pack  # noqa: E402
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


class Pack(unittest.TestCase):
    """The bundle as one file. That the sidekick's reader agrees with this builder is
    host_mock/test_app_pack.cpp's, which reads `app_pack.py --sample`'s output."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name) / "app"
        self.root.mkdir()
        (self.root / "app.html").write_bytes(PAGE)
        (self.root / "tiny.js").write_bytes(TINY)
        build.compress_assets(self.root)
        self.doc = gen_manifest.write(self.root)
        build.write_build_json(self.root, "d18f9b2 2026-11-13", self.doc)
        self.out = Path(self.tmp.name) / "app.pack"
        self.data = build.write_pack(self.root, self.out, "d18f9b2 2026-11-13")

    def tearDown(self):
        self.tmp.cleanup()

    def test_the_pack_holds_the_manifests_assets_as_shipped(self):
        bid, assets = app_pack.unpack(self.out.read_bytes())
        self.assertEqual(bid, "d18f9b2 2026-11-13")
        self.assertEqual([name for name, _ in assets], [a["path"] for a in self.doc["assets"]])
        for (name, data), a in zip(assets, self.doc["assets"]):
            self.assertEqual(hashlib.sha256(data).hexdigest(), a["sha256"], name)
        # As shipped: the page is still gzipped inside it, for the sidekick to serve as it is.
        self.assertEqual(dict(assets)["app.html"][:2], build.GZIP_MAGIC)

    def test_neither_the_manifest_nor_build_json_is_in_it(self):
        names = [name for name, _ in app_pack.unpack(self.data)[1]]
        self.assertNotIn(gen_manifest.MANIFEST_NAME, names)
        self.assertNotIn(gen_manifest.BUILD_NAME, names)

    def test_same_input_packs_the_same_bytes_and_leaves_the_file_alone(self):
        before = self.out.stat().st_mtime_ns
        self.assertEqual(build.write_pack(self.root, self.out, "d18f9b2 2026-11-13"), self.data)
        self.assertEqual(self.out.stat().st_mtime_ns, before)
        self.assertNotEqual(build.write_pack(self.root, self.out, "e6e4d88 2026-11-14"), self.data)

    def test_the_sample_round_trips(self):
        data = app_pack.pack(app_pack.SAMPLE, app_pack.SAMPLE_BUILD)
        self.assertEqual(app_pack.unpack(data), (app_pack.SAMPLE_BUILD, sorted(app_pack.SAMPLE)))

    def test_refuses_what_the_reader_would(self):
        ok = [("app.html", b"x")]
        for bad in ([], [("a/b.js", b"")], [(".hidden", b"")], [("n" * 56, b"")], ok + ok):
            with self.assertRaises(app_pack.PackError):
                app_pack.pack(bad, "d18f9b2")
        for bid in ("", 'say "hi"', "back\\slash", "x" * 48, "caf\u00e9"):
            with self.assertRaises(app_pack.PackError):
                app_pack.pack(ok, bid)

    def test_unpack_refuses_a_pack_that_lies(self):
        for cut in (0, 10, len(self.data) - 1):
            with self.assertRaises(app_pack.PackError):
                app_pack.unpack(self.data[:cut])
        with self.assertRaises(app_pack.PackError):
            app_pack.unpack(b"X" + self.data[1:])


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
