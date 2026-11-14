#!/usr/bin/env python3
"""Host test for tools/dev_build.py: no network, no git.

  python3 tools/test_dev_build.py
"""
import os
import re
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dev_build as db  # noqa: E402

SHA = "c1a53b5" + "0" * 33
P4 = "20261114-microft8-poe-p4-ota-c1a53b5.bin"
SK = "20261114-microft8-sidekick-ota-c1a53b5.bin"


def mirror(*names):
    """A mirror holding `names`, and the URLs it was asked for."""
    asked = []

    def head(url):
        asked.append(url)
        return url in [db.MIRROR_OTA + n for n in names]
    return head, asked


class DevBuildTest(unittest.TestCase):
    def test_names_both_boards_dated_images(self):
        self.assertEqual(db.ota_names("20261114", SHA), [P4, SK])

    def test_the_names_are_the_ones_ci_uploads(self):
        # release-main names each OTA image in a shell line; the two must not drift apart.
        root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
        with open(os.path.join(root, ".github", "workflows", "ci.yml"), encoding="utf-8") as f:
            ci = f.read()
        made = re.findall(r'asset_\w+_ota="\$\{day\}-microft8-([a-z0-9-]+)-ota-\$\{short\}\.bin"', ci)
        self.assertEqual(sorted(made), sorted(db.BOARDS))
        self.assertIn('put "ota/${ASSET_P4_OTA}"', ci)
        self.assertIn('put "ota/${ASSET_SK_OTA}"', ci)

    def test_published_when_the_mirror_has_both(self):
        head, asked = mirror(P4, SK)
        self.assertEqual(db.missing("20261114", SHA, head), [])
        self.assertEqual(asked, [db.MIRROR_OTA + P4, db.MIRROR_OTA + SK])
        db.require_published(SHA, head, day="20261114")

    def test_refuses_when_either_is_missing(self):
        for have, gone in (((), [P4, SK]), ((P4,), [SK]), ((SK,), [P4])):
            head, _ = mirror(*have)
            self.assertEqual(db.missing("20261114", SHA, head), gone)
            with self.assertRaisesRegex(db.DevBuildError, "no dev build of c1a53b5 was published"):
                db.require_published(SHA, head, day="20261114")

    def test_another_days_or_commits_build_does_not_count(self):
        head, _ = mirror(P4, SK)
        self.assertEqual(db.missing("20261113", SHA, head), db.ota_names("20261113", SHA))
        other = "d18f9b2" + "0" * 33
        self.assertEqual(len(db.missing("20261114", other, head)), 2)

    def test_a_mirror_that_does_not_answer_is_not_a_yes(self):
        def down(url):
            raise db.DevBuildError(url + ": HTTP 503")
        with self.assertRaisesRegex(db.DevBuildError, "HTTP 503"):
            db.require_published(SHA, down, day="20261114")

    def test_refuses_bad_arguments(self):
        with self.assertRaisesRegex(db.DevBuildError, "40-hex"):
            db.ota_names("20261114", "c1a53b5")
        for day in ("2026-11-14", "", None):
            with self.assertRaisesRegex(db.DevBuildError, "YYYYMMDD"):
                db.ota_names(day, SHA)


if __name__ == "__main__":
    unittest.main()
