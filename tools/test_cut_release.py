#!/usr/bin/env python3
"""Host test for the checks in tools/cut_release.py that need no git and no network.

  python3 tools/test_cut_release.py
"""
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cut_release as cr  # noqa: E402


class CutReleaseTest(unittest.TestCase):
    def test_release_number_reads_only_release_tags(self):
        self.assertEqual(cr.release_number("v0.1.0"), (0, 1, 0))
        self.assertEqual(cr.release_number("v12.0.34"), (12, 0, 34))
        for tag in ("0.1.0", "V1.0.0", "v1.0", "v1.0.0-rc1", "v01.0.0", "dev", "corpus-2026-11-08", "v", ""):
            self.assertIsNone(cr.release_number(tag), tag)

    def test_the_first_release_is_any_number(self):
        cr.require_newer("0.1.0", [])
        cr.require_newer("0.1.0", ["corpus-2026-11-08", "V2.1b", "v1.0"])

    def test_a_release_is_greater_than_every_one_tagged(self):
        tags = ["v0.1.0", "v0.9.0", "v0.10.0", "corpus-2026-11-08"]
        for version in ("0.10.1", "0.11.0", "1.0.0"):
            cr.require_newer(version, tags)
        # By number, not as text: 0.9.1 is below 0.10.0.
        for version in ("0.10.0", "0.9.1", "0.1.1", "0.0.9"):
            with self.assertRaisesRegex(cr.Abort, "not greater than v0.10.0"):
                cr.require_newer(version, tags)

    def test_the_version_pattern_is_build_versions(self):
        # A tag this script accepts is one tools/build_version.py gives a stable version for.
        import build_version as bv
        sha = "0c5ccec" + "1" * 33
        for version in ("0.1.0", "10.20.30"):
            self.assertTrue(cr.SEMVER.match(version))
            self.assertEqual(bv.version_for("refs/tags/v" + version, sha, None), f"{version}+0c5ccec")
        for version in ("01.0.0", "1.0", "1.0.0-rc1", "1.0.0.0"):
            self.assertIsNone(cr.SEMVER.match(version), version)
            with self.assertRaises(bv.VersionError):
                bv.version_for("refs/tags/v" + version, sha, None)


if __name__ == "__main__":
    unittest.main()
