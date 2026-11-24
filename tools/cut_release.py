#!/usr/bin/env python3
"""Cut a versioned micro-FT8 release.

Creates and pushes an annotated `v<version>` tag. CI does the rest: the
firmware jobs build on the tag and the release job publishes both boards'
images under the release's number, to GitHub and to the stable channel.

This script edits no source file, so there is no number to bump and no bump
commit for the tag to land on the wrong side of.

Everything here is a precondition check -- see docs/RELEASE_PROCESS.md. The
ones that need no git or network are host-tested: tools/test_cut_release.py.

The maintainer approves each release's note at the cut, one of two ways:

  cut_release.py 0.4.0                       at a terminal: prints the note and tags once the
                                             version is typed
  cut_release.py 0.4.0 --review              runs every check, prints the tag, the commit and the
                                             note, and creates nothing
  cut_release.py 0.4.0 --approved-at <sha>   cuts with no prompt, only if <sha> is the commit
                                             checked out: the one a --review showed the maintainer
"""

from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dev_build  # noqa: E402
import make_manifest  # noqa: E402

SEMVER = re.compile(r"^(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)$")
FULL_SHA = re.compile(r"^[0-9a-f]{40}$")
REMOTE = "origin"
BRANCH = "main"


class Abort(Exception):
    pass


def run(*args: str, check: bool = True) -> str:
    proc = subprocess.run(args, capture_output=True, text=True)
    if check and proc.returncode != 0:
        raise Abort(f"{' '.join(args)}\n{proc.stderr.strip()}")
    return proc.stdout.strip()


def require_clean_tree() -> None:
    if run("git", "status", "--porcelain"):
        raise Abort(
            "working tree is dirty. A release must name a commit that contains "
            "exactly what was built; commit or stash first."
        )


def require_on_branch() -> str:
    branch = run("git", "rev-parse", "--abbrev-ref", "HEAD")
    if branch != BRANCH:
        raise Abort(f"on branch '{branch}', expected '{BRANCH}'.")
    return branch


def require_up_to_date() -> str:
    run("git", "fetch", REMOTE, BRANCH, "--quiet")
    local = run("git", "rev-parse", "HEAD")
    remote = run("git", "rev-parse", f"{REMOTE}/{BRANCH}")
    if local != remote:
        behind = run("git", "rev-list", "--count", f"HEAD..{REMOTE}/{BRANCH}")
        ahead = run("git", "rev-list", "--count", f"{REMOTE}/{BRANCH}..HEAD")
        raise Abort(
            f"HEAD does not match {REMOTE}/{BRANCH} ({ahead} ahead, {behind} "
            "behind). Tagging a commit that is not what main points at is how a "
            "release ends up containing something nobody reviewed."
        )
    return local


def require_tag_free(tag: str) -> None:
    if run("git", "tag", "--list", tag):
        raise Abort(f"tag '{tag}' already exists locally.")
    if run("git", "ls-remote", "--tags", REMOTE, f"refs/tags/{tag}"):
        raise Abort(
            f"tag '{tag}' already exists on {REMOTE}. Version tags are "
            "immovable by convention -- cut the next version instead."
        )


def release_number(tag: str) -> tuple[int, int, int] | None:
    """(major, minor, patch) of a release tag, `v1.2.3`; None for any other tag."""
    m = SEMVER.match(tag[1:]) if tag.startswith("v") else None
    return tuple(int(g) for g in m.groups()) if m else None


def require_newer(version: str, tags: list[str]) -> None:
    """Refuse a version that is not greater than every release already tagged.

    Stable versions only rise: a station on a release is offered the next one, and a number
    below one already out would never be offered to anyone.
    """
    want = release_number(f"v{version}")
    out = [(n, t) for t in tags if (n := release_number(t)) is not None]
    if out and want <= max(out)[0]:
        raise Abort(f"{version} is not greater than {max(out)[1]}, the newest release tagged.")


def remote_tags() -> list[str]:
    lines = run("git", "ls-remote", "--tags", REMOTE, "refs/tags/v*").splitlines()
    return [line.split("refs/tags/", 1)[1] for line in lines if "refs/tags/" in line and not line.endswith("^{}")]


def require_note(version: str, notes_dir: str = make_manifest.NOTES_DIR) -> None:
    """Refuse a release with no note, or a malformed one, before it is tagged: the tag's CI job would
    refuse it anyway (tools/make_manifest.py), after building both boards. The tree is clean and up to
    date by now, so the note read here is the one the tag carries."""
    try:
        make_manifest.release_notes(version, notes_dir)
    except make_manifest.ManifestError as exc:
        raise Abort(str(exc))


def approve_note(version: str, note: str, changes: list[str], ask, is_tty: bool) -> None:
    """The maintainer approves the release's note, at a terminal (docs/RELEASE_PROCESS.md, Release
    notes). `note` is the file as written, `changes` the commit subjects since the last release, `ask`
    reads one line. Refused when input is not a terminal, so no script or agent can answer, and
    approved only by typing the version: --yes never skips this."""
    if not is_tty:
        raise Abort("the release note is approved at a terminal by the maintainer; this input is not one "
                    "(--review prints it for the maintainer, --approved-at cuts what they then approve)")
    print_review(version, note, changes)
    answer = ask(f"\napprove this note? type {version} to approve: ").strip()
    if answer != version:
        raise Abort("the release note was not approved")


def print_review(version: str, note: str, changes: list[str]) -> None:
    """What the maintainer approves: the note as written, and the commit subjects since the last
    release beside it."""
    print(f"\n--- the note operators will read before they update to {version} ---")
    print(note.rstrip())
    print(f"\n--- what changed since the last release ({len(changes)} commits) ---")
    for subject in changes:
        print(f"  {subject}")


def require_approved_commit(approved: str, head: str) -> None:
    """--approved-at: the commit the maintainer approved, from a --review, is the one about to be
    tagged. The tree is clean and the note is in it, so the commit fixes the note too; if main moved
    or the note changed since the review, the approval is for something else."""
    approved = approved.strip().lower()
    if not FULL_SHA.match(approved):
        raise Abort("--approved-at takes the full 40-character commit a --review printed")
    if approved != head:
        raise Abort(
            f"the commit approved ({approved[:7]}) is not the one checked out ({head[:7]}): "
            "run --review again and ask the maintainer to approve that"
        )


def changes_since_last_release(tags: list[str]) -> list[str]:
    """The commit subjects on HEAD since the newest release tag, or all of them before the first."""
    numbered = sorted((n, t) for t in tags if (n := release_number(t)))
    since = f"{numbered[-1][1]}..HEAD" if numbered else "HEAD"
    out = run("git", "log", "--format=%s", since)
    return out.splitlines() if out else []


def require_dev_build(sha: str) -> None:
    """Refuse a commit whose dev build was not published (tools/dev_build.py)."""
    try:
        dev_build.require_published(sha)
    except dev_build.DevBuildError as exc:
        raise Abort(str(exc))


def require_ci_green(sha: str) -> None:
    """Refuse a release from a commit CI has not passed.

    Absence of a run is treated as failure, not as permission: a release built
    from an unverified commit is the exact thing this check exists to stop.
    """
    try:
        raw = run(
            "gh", "run", "list", "--commit", sha, "--workflow", "CI",
            "--json", "conclusion,status,url", "--limit", "20",
        )
    except Abort as exc:
        raise Abort(
            f"could not ask GitHub about CI for {sha[:7]} (is `gh` installed "
            f"and authenticated?).\n{exc}\nRe-run with --skip-ci-check if you "
            "have confirmed the commit another way."
        )

    runs = json.loads(raw or "[]")
    if not runs:
        raise Abort(
            f"no CI run found for {sha[:7]}. Push it and let CI finish before "
            "tagging, or re-run with --skip-ci-check."
        )
    unfinished = [r for r in runs if r.get("status") != "completed"]
    if unfinished:
        raise Abort(f"CI is still running for {sha[:7]}: {unfinished[0]['url']}")
    failed = [r for r in runs if r.get("conclusion") != "success"]
    if failed:
        raise Abort(
            f"CI is not green for {sha[:7]}: {failed[0]['conclusion']} "
            f"{failed[0]['url']}"
        )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("version", help="release version, e.g. 3.0.0")
    parser.add_argument("--yes", action="store_true", help="skip the confirmation")
    how = parser.add_mutually_exclusive_group()
    how.add_argument(
        "--review", action="store_true",
        help="run every check and print the tag, the commit and the note; create nothing",
    )
    how.add_argument(
        "--approved-at", metavar="SHA",
        help="cut with no prompt: the full commit a --review showed the maintainer, who approved it",
    )
    parser.add_argument(
        "--skip-ci-check", action="store_true",
        help="do not require a green CI run on the commit being tagged",
    )
    args = parser.parse_args()

    version = args.version.lstrip("v")
    if not SEMVER.match(version):
        print(f"error: '{args.version}' is not MAJOR.MINOR.PATCH", file=sys.stderr)
        return 2
    tag = f"v{version}"

    try:
        require_clean_tree()
        branch = require_on_branch()
        sha = require_up_to_date()
        if args.approved_at:
            require_approved_commit(args.approved_at, sha)
        require_tag_free(tag)
        require_newer(version, remote_tags())
        require_note(version)
        require_dev_build(sha)
        if args.skip_ci_check:
            print("! skipping the CI check")
        else:
            require_ci_green(sha)
    except Abort as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1

    subject = run("git", "log", "-1", "--format=%s", sha)
    print(f"  tag     {tag}")
    print(f"  commit  {sha}  {subject}")
    print(f"  branch  {branch}")
    print(f"  CI      builds both boards and publishes {version} to GitHub and the stable channel")

    try:
        with open(os.path.join(make_manifest.NOTES_DIR, f"{version}.md"), encoding="utf-8") as f:
            note = f.read()
        changes = changes_since_last_release(remote_tags())
        if args.review or args.approved_at:
            print_review(version, note, changes)
        else:
            approve_note(version, note, changes, input, sys.stdin.isatty())
    except Abort as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1

    if args.review:
        print(f"\nreviewed; nothing was created. Once the maintainer approves {version} at this commit:")
        print(f"  python3 tools/cut_release.py {version} --approved-at {sha}")
        return 0

    if not args.yes and not args.approved_at:
        if input("\npush this tag? [y/N] ").strip().lower() not in ("y", "yes"):
            print("aborted")
            return 1

    try:
        run("git", "tag", "-a", tag, "-m", f"micro-FT8 {version}")
        run("git", "push", REMOTE, tag)
    except Abort as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1

    slug = run("git", "config", "--get", f"remote.{REMOTE}.url")
    slug = re.sub(r"^.*github\.com[:/]|\.git$", "", slug)
    print(f"\npushed {tag}. Release will appear at:")
    print(f"  https://github.com/{slug}/releases/tag/{tag}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
