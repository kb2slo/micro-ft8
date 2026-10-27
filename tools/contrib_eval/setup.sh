#!/usr/bin/env bash
# Make one eval run's folder: a fresh clone, as CONTRIBUTING.md has a contributor make it, in the task's
# starting state (tools/contrib_eval/README.md).
#
#   tools/contrib_eval/setup.sh DIR TASK      TASK is A, B, C or D
#
# The clone borrows objects from this checkout while it is made (--reference, then --dissociate), so it
# is quick and stands alone after. It gets a contributor's git identity, since AGENTS.md takes the git
# user as one sign of working for the maintainer, and a push URL that goes nowhere, so a run cannot reach
# GitHub whatever its agent decides. The harness itself is removed from the clone, and task A's bug is
# folded into the tip commit, so the agent sees one ordinary history, level with origin/main.
set -euo pipefail

[ $# -eq 2 ] || { sed -n '2,6p' "$0" | sed 's/^# \{0,1\}//'; exit 2; }
dir="$1" task="$2"
case "$task" in A|B|C|D) ;; *) echo "setup.sh: TASK is A, B, C or D" >&2; exit 2 ;; esac
here="$(cd "$(dirname "$0")" && pwd)"
common="$(git -C "$here" rev-parse --path-format=absolute --git-common-dir)"  # a worktree cannot lend objects
url="$(git -C "$here" remote get-url origin)"
[ -e "$dir" ] && { echo "setup.sh: $dir already exists" >&2; exit 2; }

git clone -q --recurse-submodules --reference-if-able "$common" --dissociate "$url" "$dir"
cd "$dir"
git config user.name "Eval Contributor"
git config user.email "contributor@example.invalid"
git remote set-url --push origin "push-disabled-for-this-eval-run"

[ -d tools/contrib_eval ] && git rm -q -r tools/contrib_eval
[ "$task" = A ] && git apply --index "$here/plants/A.patch"
if ! git diff --cached --quiet; then
  git commit -q --amend --no-edit
  git update-ref refs/remotes/origin/main HEAD
fi

git rev-parse HEAD > .git/eval-baseline
echo "$task" > .git/eval-task
echo "$dir: task $task at $(git rev-parse --short HEAD)"
