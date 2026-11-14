#!/usr/bin/env python3
"""Whether a commit's dev build was published (docs/RELEASE_PROCESS.md, Cutting a versioned release).

  python3 tools/dev_build.py --sha <40-hex>

A stable tag goes only on a commit whose dev build was published, so a
release is code that could be installed as a dev build first. Published
means both boards' OTA images are on the mirror under their dated names,
which CI's `release-main` uploads and nothing ever overwrites or deletes.

Exits 0 when both are there, 1 naming what is missing or why it could not
be checked. tools/cut_release.py asks before it tags, and the tag's CI job
asks again before it publishes, so a tag pushed by hand does not pass.

Python's standard library only.
"""
import argparse
import datetime
import re
import subprocess
import sys
import urllib.error
import urllib.request

MIRROR_OTA = "https://micro-ft8.kalikstein.com/download/ota/"
BOARDS = ("poe-p4", "sidekick")
SHORT = 7  # as tools/git_version.cmake cuts a SHA


class DevBuildError(Exception):
    pass


def ota_names(day, sha):
    """Both boards' dated OTA file names for the commit `sha`, committed on `day` (YYYYMMDD, UTC).

    The names `release-main` gives them in .github/workflows/ci.yml; tools/test_dev_build.py holds
    the two together."""
    if not re.fullmatch(r"\d{8}", day or ""):
        raise DevBuildError(f"a commit's day is YYYYMMDD, not '{day}'")
    if not re.fullmatch(r"[0-9a-f]{40}", sha or ""):
        raise DevBuildError(f"--sha wants a full 40-hex SHA, not '{sha}'")
    return [f"{day}-microft8-{board}-ota-{sha[:SHORT]}.bin" for board in BOARDS]


def _head(url):
    """True if the mirror has it, False on a 404. Anything else is not an answer."""
    # Named: the mirror's host answers Python's default User-Agent with 403.
    req = urllib.request.Request(url, method="HEAD", headers={"User-Agent": "micro-ft8-dev-build"})
    try:
        with urllib.request.urlopen(req, timeout=20) as r:
            return r.status == 200
    except urllib.error.HTTPError as e:
        if e.code == 404:
            return False
        raise DevBuildError(f"{url}: HTTP {e.code}")
    except (urllib.error.URLError, OSError) as e:
        raise DevBuildError(f"{url}: {e}")


def missing(day, sha, head=_head):
    """The dated OTA names of `sha`'s dev build that the mirror does not have; [] when it was published."""
    return [name for name in ota_names(day, sha) if not head(MIRROR_OTA + name)]


def commit_day(sha):
    """The commit's date in UTC, YYYYMMDD, as CI names its files."""
    try:
        out = subprocess.run(["git", "show", "-s", "--format=%ct", sha], capture_output=True, text=True, check=True)
        stamp = int(out.stdout.strip())
    except (OSError, subprocess.CalledProcessError, ValueError) as e:
        raise DevBuildError(f"could not read the date of commit {sha[:SHORT]}: {e}")
    return datetime.datetime.fromtimestamp(stamp, datetime.timezone.utc).strftime("%Y%m%d")


def require_published(sha, head=_head, day=None):
    """Raise DevBuildError unless `sha`'s dev build is on the mirror."""
    gone = missing(day or commit_day(sha), sha, head)
    if gone:
        raise DevBuildError(
            f"no dev build of {sha[:SHORT]} was published: the mirror has no {' or '.join(gone)}. "
            "A stable tag goes only on a commit whose dev build was published. If the commit changed no "
            "firmware, CI built none: `gh workflow run CI --ref main` builds and publishes one."
        )


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    ap.add_argument("--sha", required=True, help="the commit to be tagged, 40 hex")
    args = ap.parse_args(argv)
    try:
        require_published(args.sha)
    except DevBuildError as e:
        print(f"dev_build: {e}", file=sys.stderr)
        return 1
    print(f"dev build of {args.sha[:SHORT]}: published")
    return 0


if __name__ == "__main__":
    sys.exit(main())
