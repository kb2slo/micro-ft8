#!/usr/bin/env python3
"""Host test for tools/build_version.py: no git, no build.

  python3 tools/test_build_version.py
"""
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import build_version as bv  # noqa: E402

SHA = "0c5ccec" + "1" * 33


class BuildVersionTest(unittest.TestCase):
    def test_main_is_dev_numbered_by_its_commit_count(self):
        self.assertEqual(bv.version_for("refs/heads/main", SHA, 1322), "b1322+0c5ccec")

    def test_a_semver_tag_is_stable(self):
        self.assertEqual(bv.version_for("refs/tags/v0.1.0", SHA, None), "0.1.0+0c5ccec")
        self.assertEqual(bv.version_for("refs/tags/v12.0.34", SHA, None), "12.0.34+0c5ccec")

    def test_any_other_build_is_on_no_channel(self):
        # A pull request, another branch, and the tags that are not releases.
        for ref in ("refs/pull/7/merge", "refs/heads/version-in-every-image", "refs/heads/main2",
                    "refs/tags/continuous", "refs/tags/corpus-2026-11-08", "refs/tags/V1.0.0"):
            self.assertEqual(bv.version_for(ref, SHA, 1322), "", ref)

    def test_refuses_a_v_tag_that_is_not_a_release(self):
        for tag in ("v1", "v1.0", "v1.0.0-rc1", "v01.0.0", "v1.0.0.0", "vnext", "v"):
            with self.assertRaisesRegex(bv.VersionError, "not a release"):
                bv.version_for("refs/tags/" + tag, SHA, None)

    def test_refuses_main_without_a_count_and_a_short_sha(self):
        for count in (None, 0, -1, "1322"):
            with self.assertRaisesRegex(bv.VersionError, "no commit count"):
                bv.version_for("refs/heads/main", SHA, count)
        for sha in ("0c5ccec", "", None, SHA.upper()):
            with self.assertRaisesRegex(bv.VersionError, "40-hex"):
                bv.version_for("refs/heads/main", sha, 1322)

    def test_parse_reads_back_what_version_for_writes(self):
        self.assertEqual(bv.parse(bv.version_for("refs/heads/main", SHA, 1322)),
                         {"channel": "dev", "label": "b1322", "sha": "0c5ccec", "build": 1322})
        self.assertEqual(bv.parse(bv.version_for("refs/tags/v0.1.0", SHA, None)),
                         {"channel": "stable", "label": "0.1.0", "sha": "0c5ccec", "build": None})

    def test_parse_refuses_everything_else(self):
        # A bench build's bare SHA, a dirty tree, and near misses.
        for v in ("0c5ccec", "0c5ccec-dirty", "b1322+0c5ccec-dirty", "0.1.0+0c5ccec-dirty", "b0+0c5ccec",
                  "b1322", "0.1.0", "1322+0c5ccec", "v0.1.0+0c5ccec", "b1322+0C5CCEC", "b1322+0c5cce",
                  "0.1+0c5ccec", "unknown", "", None):
            self.assertIsNone(bv.parse(v), v)

    def test_a_version_fits_the_image_and_the_wire(self):
        # esp_app_desc_t's version[32] and FW_VERSION_MAX (sidekick/main/fw_policy.h): 31 characters.
        self.assertLess(len(bv.version_for("refs/heads/main", SHA, 9999999)), 32)
        self.assertLess(len(bv.version_for("refs/tags/v999.999.999", SHA, None)), 32)


if __name__ == "__main__":
    unittest.main()
