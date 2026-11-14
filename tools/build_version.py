#!/usr/bin/env python3
"""The version a CI build reports (docs/RELEASE_PROCESS.md, An image carries its version).

  python3 tools/build_version.py --ref refs/heads/main --sha <40-hex>   ->  b1322+0c5ccec
  python3 tools/build_version.py --ref refs/tags/v0.1.0 --sha <40-hex>  ->  0.1.0+0c5ccec
  python3 tools/build_version.py --ref refs/pull/7/merge --sha <40-hex> ->  (an empty line)

Two channels. Stable is a `v*` tag, semver. Dev is CI's build of `main`,
numbered by the commits on `main` up to and including it, which anyone can
count again from a clone. Each carries its commit after a `+`. Any other
build gets no version here and reports its bare SHA
(tools/git_version.cmake), so only CI's builds of a tag or of `main` are
ever on a channel.

CI hands the line this prints to the build as MICROFT8_VERSION, and
tools/make_manifest.py checks each image against it.

Python's standard library only.
"""
import argparse
import re
import subprocess
import sys

MAIN_REF = "refs/heads/main"
TAG_PREFIX = "refs/tags/v"
SHORT = 7  # as tools/git_version.cmake cuts a SHA

_SEMVER = r"(?:0|[1-9]\d*)\.(?:0|[1-9]\d*)\.(?:0|[1-9]\d*)"
_STABLE = re.compile(rf"({_SEMVER})\+([0-9a-f]{{{SHORT}}})")
_DEV = re.compile(rf"b([1-9]\d*)\+([0-9a-f]{{{SHORT}}})")


class VersionError(Exception):
    pass


def version_for(ref, sha, count):
    """The version CI's build of `sha` at `ref` reports; '' for a build on no channel.

    `count` is the number of commits up to and including `sha`, used on `main` only."""
    if not re.fullmatch(r"[0-9a-f]{40}", sha or ""):
        raise VersionError(f"--sha wants a full 40-hex SHA, not '{sha}'")
    short = sha[:SHORT]
    if ref.startswith(TAG_PREFIX):
        number = ref[len(TAG_PREFIX):]
        if not re.fullmatch(_SEMVER, number):
            raise VersionError(f"tag 'v{number}' is not a release: a stable tag is v<major>.<minor>.<patch>")
        return f"{number}+{short}"
    if ref == MAIN_REF:
        if not isinstance(count, int) or count < 1:
            raise VersionError(f"no commit count for {short} on main")
        return f"b{count}+{short}"
    return ""


def parse(version):
    """{'channel': 'stable' or 'dev', 'label', 'sha', 'build'} for a channel's version, else None.

    `label` is what an operator reads ('0.1.0', 'b1322'); `build` is the dev build number, None
    for stable."""
    m = _STABLE.fullmatch(version or "")
    if m:
        return {"channel": "stable", "label": m.group(1), "sha": m.group(2), "build": None}
    m = _DEV.fullmatch(version or "")
    if m:
        return {"channel": "dev", "label": "b" + m.group(1), "sha": m.group(2), "build": int(m.group(1))}
    return None


def _git(*args):
    return subprocess.run(["git", *args], capture_output=True, text=True, check=True).stdout.strip()


def commit_count(sha):
    """Commits up to and including `sha`. Refuses a shallow clone, which would count only what it
    holds and number the build wrongly without a sign."""
    try:
        if _git("rev-parse", "--is-shallow-repository") != "false":
            raise VersionError("this clone is shallow, so its commits cannot be counted (CI: fetch-depth: 0)")
        return int(_git("rev-list", "--count", sha))
    except (OSError, subprocess.CalledProcessError, ValueError) as e:
        raise VersionError(f"could not count the commits up to {sha[:SHORT]}: {e}")


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    ap.add_argument("--ref", required=True, help="the ref CI is building, e.g. $GITHUB_REF")
    ap.add_argument("--sha", required=True, help="the commit CI is building, 40 hex")
    args = ap.parse_args(argv)
    try:
        count = commit_count(args.sha) if args.ref == MAIN_REF else None
        print(version_for(args.ref, args.sha, count))
    except VersionError as e:
        print(f"build_version: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
