#!/usr/bin/env python3
"""Host test for tools/make_manifest.py, on synthetic images: no build needed.

  python3 tools/test_make_manifest.py
"""
import hashlib
import json
import os
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import make_manifest as mm  # noqa: E402

COMMIT = "c6374c9" + "0" * 33
STAMP = "2026-11-10T00:00:00Z"


def app_image(version, project, size=4096):
    """The start of an ESP-IDF app image: image header, segment header, esp_app_desc_t."""
    b = bytearray(size)
    b[0] = 0xE9
    struct.pack_into("<I", b, 32, 0xABCD5432)
    b[48:48 + len(version)] = version.encode()
    b[80:80 + len(project)] = project.encode()
    return bytes(b)


class ManifestTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)

    def put(self, name, data):
        path = os.path.join(self.tmp.name, name)
        with open(path, "wb") as f:
            f.write(data)
        return path

    def build(self, assets, otas, commit=COMMIT):
        return mm.build(commit, "20261110", assets, otas, 14, STAMP)

    def test_full_manifest(self):
        p4_ota = app_image("c6374c9", "poe_p4", 5000)
        sk_ota = app_image("c6374c9", "sidekick", 6000)
        m = self.build(
            {
                "poe_p4": self.put("20261110-microft8-poe-p4-c6374c9.bin", b"merged p4"),
                "sidekick": self.put("20261110-microft8-sidekick-c6374c9.bin", b"merged sk"),
                "source": self.put("20261110-microft8-source-c6374c9.tar.gz", b"src"),
                "getting_started": self.put("20261110-microft8-getting-started-c6374c9.pdf", b"pdf"),
            },
            {
                "poe_p4": self.put("20261110-microft8-poe-p4-ota-c6374c9.bin", p4_ota),
                "sidekick": self.put("20261110-microft8-sidekick-ota-c6374c9.bin", sk_ota),
            },
        )
        self.assertEqual((m["commit"], m["short"], m["date"], m["generated_at"]), (COMMIT, "c6374c9", "20261110", STAMP))
        a = m["assets"]
        # What downloads.html reads today is unchanged.
        self.assertEqual(a["poe_p4"]["stable"], "microft8-poe-p4-continuous.bin")
        self.assertEqual(a["poe_p4"]["filename"], "20261110-microft8-poe-p4-c6374c9.bin")
        self.assertEqual(a["poe_p4"]["sha256"], hashlib.sha256(b"merged p4").hexdigest())
        self.assertEqual((a["poe_p4"]["chip"], a["poe_p4"]["flash_offset"]), ("esp32p4", "0x0"))
        self.assertEqual((a["sidekick"]["chip"], a["sidekick"]["flash_offset"]), ("esp32s3", "0x0"))
        self.assertEqual(a["source"], {
            "stable": "microft8-source-continuous.tar.gz",
            "filename": "20261110-microft8-source-c6374c9.tar.gz",
            "sha256": hashlib.sha256(b"src").hexdigest(),
        })
        self.assertEqual(a["getting_started"]["stable"], "microft8-getting-started-continuous.pdf")
        self.assertEqual(a["poe_p4"]["ota"], {
            "stable": "microft8-poe-p4-continuous-ota.bin",
            "filename": "20261110-microft8-poe-p4-ota-c6374c9.bin",
            "size": 5000,
            "sha256": hashlib.sha256(p4_ota).hexdigest(),
            "version": "c6374c9",
            "proto": 14,
        })
        self.assertEqual(a["sidekick"]["ota"]["stable"], "microft8-sidekick-continuous-ota.bin")
        self.assertEqual(a["sidekick"]["ota"]["size"], 6000)
        self.assertNotIn("ota", a["source"])

    def one_ota(self, image, board="poe_p4"):
        return self.build({board: self.put("merged.bin", b"m")}, {board: self.put("ota.bin", image)})

    def test_refuses_a_merged_image(self):
        # A merged 0x0 image starts with the bootloader's header, and its app description is elsewhere.
        with self.assertRaisesRegex(mm.ManifestError, "not an ESP-IDF app image"):
            self.one_ota(b"\xff" * 4096)
        merged = bytearray(app_image("c6374c9", "poe_p4"))
        struct.pack_into("<I", merged, 32, 0)
        with self.assertRaisesRegex(mm.ManifestError, "no app description"):
            self.one_ota(bytes(merged))

    def test_refuses_another_boards_image(self):
        with self.assertRaisesRegex(mm.ManifestError, "built as 'sidekick'"):
            self.one_ota(app_image("c6374c9", "sidekick"))

    def test_refuses_a_version_that_is_not_the_commit(self):
        for version in ("e6e4d88", "c6374c9-dirty", "unknown"):
            with self.assertRaisesRegex(mm.ManifestError, "reports version"):
                self.one_ota(app_image(version, "poe_p4"))

    def test_refuses_an_image_too_big_to_stage(self):
        self.one_ota(app_image("c6374c9", "poe_p4", mm.OTA_MAX_BYTES))
        with self.assertRaisesRegex(mm.ManifestError, "will not fit"):
            self.one_ota(app_image("c6374c9", "poe_p4", mm.OTA_MAX_BYTES + 1))

    def test_refuses_bad_arguments(self):
        with self.assertRaisesRegex(mm.ManifestError, "40-hex"):
            self.build({}, {}, commit="c6374c9")
        with self.assertRaisesRegex(mm.ManifestError, "unknown asset kind"):
            self.build({"adv": self.put("a", b"a")}, {})
        with self.assertRaisesRegex(mm.ManifestError, "has no OTA image"):
            self.build({"source": self.put("s", b"s")}, {"source": self.put("o", b"o")})
        with self.assertRaisesRegex(mm.ManifestError, "without --asset"):
            self.build({}, {"poe_p4": self.put("o", app_image("c6374c9", "poe_p4"))})

    def test_staging_limit_is_the_sidekicks_slot(self):
        sizes = {}
        with open(os.path.join(mm.ROOT, "sidekick", "partitions.csv"), encoding="utf-8") as f:
            for line in f:
                cols = [c.strip() for c in line.split("#", 1)[0].split(",")]
                if len(cols) >= 5 and cols[0] in ("ota_0", "ota_1"):
                    sizes[cols[0]] = int(cols[4], 0)
        self.assertEqual(sizes, {"ota_0": mm.OTA_MAX_BYTES, "ota_1": mm.OTA_MAX_BYTES})

    def test_reads_this_trees_protocol_version(self):
        self.assertGreaterEqual(mm.proto_version(), 14)

    def test_command_line_writes_the_file(self):
        out = os.path.join(self.tmp.name, "manifest.json")
        rc = mm.main([
            "--commit", COMMIT, "--date", "20261110", "--out", out,
            "--asset", "sidekick=" + self.put("sk.bin", b"m"),
            "--ota", "sidekick=" + self.put("sk-ota.bin", app_image("c6374c9", "sidekick")),
        ])
        self.assertEqual(rc, 0)
        with open(out, encoding="utf-8") as f:
            m = json.load(f)
        self.assertEqual(m["assets"]["sidekick"]["ota"]["proto"], mm.proto_version())
        self.assertEqual(mm.main(["--commit", "bad", "--date", "x", "--out", out]), 1)


if __name__ == "__main__":
    unittest.main()
