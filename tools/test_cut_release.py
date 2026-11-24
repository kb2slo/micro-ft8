#!/usr/bin/env python3
"""Host test for the checks in tools/cut_release.py that need no git and no network.

  python3 tools/test_cut_release.py
"""
import builtins
import contextlib
import io
import os
import sys
import tempfile
import unittest
from unittest import mock

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

    def test_a_release_needs_its_note_before_it_is_tagged(self):
        with tempfile.TemporaryDirectory() as notes:
            with self.assertRaisesRegex(cr.Abort, "has no note"):
                cr.require_note("0.2.0", notes)
            with open(os.path.join(notes, "0.2.0.md"), "w", encoding="utf-8") as f:
                f.write("# 0.2.0\n")
            with self.assertRaisesRegex(cr.Abort, "says nothing"):
                cr.require_note("0.2.0", notes)
            with open(os.path.join(notes, "0.2.0.md"), "w", encoding="utf-8") as f:
                f.write("# 0.2.0\n## New\n- FT4 CQ.\n")
            cr.require_note("0.2.0", notes)

    def test_the_note_is_approved_only_by_typing_its_version_at_a_terminal(self):
        note, changes = "# 0.2.0\n## New\n- FT4 CQ.\n", ["Add FT4 CQ"]
        # What it prints for the maintainer is not what is tested here.
        self.enterContext(contextlib.redirect_stdout(io.StringIO()))
        answers = lambda *a: (lambda prompt: a[0])
        cr.approve_note("0.2.0", note, changes, answers("0.2.0"), True)
        cr.approve_note("0.2.0", note, changes, answers("  0.2.0\n"), True)
        for typed in ("y", "yes", "", "0.2", "v0.2.0"):
            with self.assertRaisesRegex(cr.Abort, "not approved"):
                cr.approve_note("0.2.0", note, changes, answers(typed), True)
        # No terminal: refused before anything is asked, whatever would have been answered.
        asked = []
        with self.assertRaisesRegex(cr.Abort, "at a terminal"):
            cr.approve_note("0.2.0", note, changes, lambda p: asked.append(p) or "0.2.0", False)
        self.assertEqual(asked, [])

    def test_a_commit_approved_from_a_review_is_the_one_tagged(self):
        head = "0c5ccec" + "a" * 33
        cr.require_approved_commit(head, head)
        cr.require_approved_commit("  " + head.upper() + "\n", head)
        # Main moved, or the note was edited, since the review: the approval is for another commit.
        with self.assertRaisesRegex(cr.Abort, "not the one checked out"):
            cr.require_approved_commit("b" * 40, head)
        # A short commit could name two, and nothing but a commit is taken.
        for given in (head[:7], "", "0.2.0", "HEAD", head + "0"):
            with self.assertRaisesRegex(cr.Abort, "full 40-character commit"):
                cr.require_approved_commit(given, head)

    def test_a_review_prints_the_note_and_the_commits_and_asks_nothing(self):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            cr.print_review("0.2.0", "# 0.2.0\n\n## New\n- A thing.\n", ["Add a thing (#7)", "Fix another (#8)"])
        shown = out.getvalue()
        for part in ("update to 0.2.0", "- A thing.", "(2 commits)", "Add a thing (#7)", "Fix another (#8)"):
            self.assertIn(part, shown)

    def cut(self, *argv, head="0c5ccec" + "a" * 33):
        """main() with every git and network check stood in for: what it ran through git, its exit
        code and what it printed. A prompt fails the test: these paths ask nothing."""
        ran = []

        def run(*args, check=True):
            ran.append(args)
            return "git@github.com:kb2slo/micro-ft8.git" if args[1] == "config" else "A subject"

        def no_prompt(_prompt=""):
            raise AssertionError("asked at a prompt")

        with tempfile.TemporaryDirectory() as notes, contextlib.ExitStack() as stack:
            with open(os.path.join(notes, "0.2.0.md"), "w", encoding="utf-8") as f:
                f.write("# 0.2.0\n\n## New\n- A thing.\n")
            patch = lambda target, value: stack.enter_context(mock.patch.object(*target, value))
            for name in ("require_clean_tree", "require_tag_free", "require_newer", "require_note",
                         "require_dev_build", "require_ci_green"):
                patch((cr, name), lambda *a, **k: None)
            patch((cr, "require_on_branch"), lambda: "main")
            patch((cr, "require_up_to_date"), lambda: head)
            patch((cr, "remote_tags"), lambda: ["v0.1.0"])
            patch((cr, "changes_since_last_release"), lambda tags: ["Add a thing (#7)"])
            patch((cr, "run"), run)
            patch((cr.make_manifest, "NOTES_DIR"), notes)
            patch((builtins, "input"), no_prompt)
            patch((sys, "argv"), ["cut_release.py", *argv])
            out, err = io.StringIO(), io.StringIO()
            with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
                code = cr.main()
        return ran, code, out.getvalue() + err.getvalue()

    def test_a_review_shows_the_release_and_creates_nothing(self):
        head = "0c5ccec" + "a" * 33
        ran, code, shown = self.cut("0.2.0", "--review")
        self.assertEqual(code, 0)
        self.assertEqual([args for args in ran if args[1] in ("tag", "push")], [])
        for part in ("v0.2.0", head, "- A thing.", "Add a thing (#7)", f"--approved-at {head}"):
            self.assertIn(part, shown)

    def test_the_approved_commit_is_tagged_and_pushed_without_a_prompt(self):
        head = "0c5ccec" + "a" * 33
        ran, code, shown = self.cut("0.2.0", "--approved-at", head)
        self.assertEqual(code, 0, shown)
        self.assertIn(("git", "tag", "-a", "v0.2.0", "-m", "micro-FT8 0.2.0"), ran)
        self.assertIn(("git", "push", "origin", "v0.2.0"), ran)
        self.assertIn("- A thing.", shown)

    def test_another_commit_than_the_approved_one_is_not_tagged(self):
        ran, code, shown = self.cut("0.2.0", "--approved-at", "b" * 40)
        self.assertEqual(code, 1)
        self.assertEqual(ran, [])
        self.assertIn("not the one checked out", shown)

    def test_without_either_the_note_is_still_approved_at_a_terminal(self):
        # The test's input is no terminal: refused before the prompt, and nothing tagged.
        with mock.patch.object(sys.stdin, "isatty", lambda: False):
            ran, code, shown = self.cut("0.2.0")
        self.assertEqual(code, 1)
        self.assertEqual([args for args in ran if args[1] in ("tag", "push")], [])
        self.assertIn("at a terminal", shown)


if __name__ == "__main__":
    unittest.main()
