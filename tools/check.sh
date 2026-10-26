#!/usr/bin/env bash
# Every check CI runs, from one command. CI calls this script, one check per
# step, so the two cannot drift. What each check covers: docs/TEST_PLAN.md
# (Automated).
#
#   tools/check.sh                every check
#   tools/check.sh --changed      only the checks a change since BASE can affect
#   tools/check.sh --only a,b     the named checks
#   tools/check.sh --list         the checks, and which --changed would run
#
#   --base REV   what --changed, docs and commits compare against
#                (default: the merge base of HEAD and origin/main)
#   --head REV   the last commit the commits check reads (default: HEAD)
#   -v           every check's output as it runs (the default under CI);
#                otherwise it goes to build/check/<name>.log, and a failing
#                check prints its last lines
#
# The firmware builds (p4, sidekick) need idf.py on PATH (source
# esp-idf/export.sh). Without it they are reported SKIP, which is not a pass.
# CI builds firmware in its own jobs, in ESP-IDF's container.
set -euo pipefail

cd "$(git rev-parse --show-toplevel)"

CHECKS="web webapp host_mock sim tx_e2e decode_recall getting_started docs commits sidekick p4"
HOST_CHECKS="web webapp host_mock sim tx_e2e decode_recall getting_started"
LOG_DIR="build/check"
JOBS="$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)"
IDF_VERSION="v5.5.5"  # AGENTS.md, Local IDF build / flash

# What a changed path can affect, as case patterns (a * also crosses /). A
# changed .md file counts only for a pattern that names it. A tracked path no
# check claims, and not in NO_CHECK, runs every host check; an untracked one
# counts only where a check claims it, so local clutter does not run them all.
triggers() {
  case "$1" in
    web)             echo 'webapp/app/* webapp/tools/*' ;;
    webapp)          echo 'webapp/*' ;;
    host_mock)       echo 'components/* host_mock/* sidekick/main/*' ;;
    sim)             echo 'components/* host_mock/* sidekick/main/* webapp/tests/*' ;;
    tx_e2e)          echo 'components/* tests/tx_e2e/*' ;;
    decode_recall)   echo 'components/* tools/decode_recall/*' ;;
    getting_started) echo 'webapp/app/* webapp/tools/* webapp/getting-started.md' ;;
    # The paths filter of ci.yml's `changes` job, which gates CI's firmware jobs.
    sidekick|p4)     echo 'components/* sidekick/* poe_p4/* idf_component.yml */idf_component.yml tools/*.cmake tools/check_ram_budget.py .github/workflows/ci.yml' ;;
  esac
}
NO_CHECK='*.md docs/* LICENSE *.jpeg *.jpg *.png *.svg .gitignore */.gitignore .cursor/* YouTube_Video_Scripts/*'
ALL_HOST='.github/workflows/ci.yml'  # claimed by the firmware checks, and also what runs the host ones

usage() { sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'; exit 2; }

MODE=all ONLY="" BASE="" HEAD_REV=HEAD LIST=0 VERBOSE=0
[ -n "${CI:-}" ] && VERBOSE=1
while [ $# -gt 0 ]; do
  case "$1" in
    --changed) MODE=changed ;;
    --only) [ $# -ge 2 ] || usage; MODE=only ONLY="$(echo "$2" | tr ',' ' ')"; shift ;;
    --base) [ $# -ge 2 ] || usage; BASE="$2"; shift ;;
    --head) [ $# -ge 2 ] || usage; HEAD_REV="$2"; shift ;;
    --list) LIST=1 ;;
    -v|--verbose) VERBOSE=1 ;;
    -h|--help) usage ;;
    *) echo "check.sh: unknown argument $1" >&2; usage ;;
  esac
  shift
done

# A push that creates a branch has no `before`; GitHub sends all zeros.
case "$BASE" in *[!0]*) ;; *) BASE="" ;; esac
if [ -n "$BASE" ]; then
  git rev-parse --verify -q "$BASE^{commit}" >/dev/null || { echo "check.sh: --base $BASE is not a commit here" >&2; exit 2; }
elif git rev-parse --verify -q origin/main >/dev/null; then
  # A shallow clone (CI's host-tests checkout) may not reach the merge base.
  BASE="$(git merge-base HEAD origin/main 2>/dev/null || git rev-parse HEAD)"
else
  BASE="$(git rev-parse HEAD)"
fi

for name in $ONLY; do
  case " $CHECKS " in *" $name "*) ;; *) echo "check.sh: no check named $name (checks: $CHECKS)" >&2; exit 2 ;; esac
done

# ---- selection ------------------------------------------------------------

# "T<tab>path" for each tracked path changed since BASE, "U<tab>path" for each
# untracked one.
changed_files() {
  git diff --name-only --no-renames "$BASE" | sed 's/^/T	/'
  git ls-files --others --exclude-standard | sed 's/^/U	/'
}

SELECTED=" docs commits "
select_check() {  # name, the path that selected it
  case "$SELECTED" in *" $1 "*) return ;; esac
  SELECTED="$SELECTED$1 "
  printf -v "WHY_$1" '%s' "$2"
}

matches() {  # path, patterns...
  local path="$1" pat; shift
  for pat in "$@"; do
    case "$path" in *.md) case "$pat" in *.md) ;; *) continue ;; esac ;; esac
    # shellcheck disable=SC2254
    case "$path" in $pat) return 0 ;; esac
  done
  return 1
}

select_changed() {
  local kind path name claimed
  set -f
  while IFS=$'\t' read -r kind path; do
    claimed=0
    for name in $CHECKS; do
      case "$name" in docs|commits) continue ;; esac
      # shellcheck disable=SC2046
      if matches "$path" $(triggers "$name"); then
        select_check "$name" "$path"
        claimed=1
      fi
    done
    # shellcheck disable=SC2086
    if { [ "$claimed" = 0 ] && [ "$kind" = T ] && ! matches "$path" $NO_CHECK; } || matches "$path" $ALL_HOST; then
      for name in $HOST_CHECKS; do select_check "$name" "$path"; done
    fi
  done < <(changed_files)
  set +f
}

case "$MODE" in
  all) SELECTED=" $CHECKS " ;;
  only) SELECTED=" $ONLY " ;;
  changed) select_changed ;;
esac

if [ "$LIST" = 1 ]; then
  echo "base $(git rev-parse --short "$BASE")$( [ "$MODE" = changed ] && echo ", checks a change since it can affect")"
  for name in $CHECKS; do
    why="WHY_$name"
    case "$SELECTED" in
      *" $name "*) printf '  run   %-16s %s\n' "$name" "${!why:-}" ;;
      *)           printf '  --    %-16s\n' "$name" ;;
    esac
  done
  exit 0
fi

# ---- the checks -----------------------------------------------------------
# Each runs in a subshell under set -e. Exit 3 with a "SKIP: why" line means
# the check could not run here.

skip() { echo "SKIP: $*"; exit 3; }

need_submodule() {
  [ -e components/ft8_lib/vendor/ft8 ] ||
    { echo "components/ft8_lib/vendor is empty: run git submodule update --init (CONTRIBUTING.md, Setup)"; exit 1; }
}

need_npm() {
  local lock=webapp/tools/package-lock.json installed=webapp/tools/node_modules/.package-lock.json
  if [ ! -f "$installed" ] || [ "$lock" -nt "$installed" ]; then npm ci --prefix webapp/tools; fi
}

need_idf() {
  command -v idf.py >/dev/null || skip "idf.py not on PATH (source esp-idf/export.sh, ESP-IDF $IDF_VERSION)"
  case "$(idf.py --version 2>/dev/null)" in
    *"$IDF_VERSION"*) ;;
    *) echo "warning: $(idf.py --version 2>/dev/null), not ESP-IDF $IDF_VERSION as CI builds with" ;;
  esac
}

# The bundle pages.yml deploys (docs/architecture/webapp.md, Build): minify,
# compile-check every script, write the manifest, hold the size budget.
check_web() {
  need_npm
  python3 webapp/tools/build.py
}

# Every webapp/tests/test_*.js but the station simulator's (check sim). Globbed,
# not listed: a hand-kept list misses the test added next.
check_webapp() {
  local t ran=0 failed=0
  for t in webapp/tests/test_*.js; do
    case "$t" in */test_sim_*) continue ;; esac
    echo "===== $t ====="
    node "$t" || failed=1
    ran=$((ran + 1))
  done
  echo "ran $ran webapp tests"
  return "$failed"
}

# The scenario runner over every test_*.json, then every host_test_* binary the
# Makefile built, globbed for the same reason as check_webapp.
check_host_mock() {
  local f t ran=0 failed=0
  need_submodule
  make -C host_mock -j"$JOBS"
  cd host_mock
  for f in test_*.json; do
    echo "===== $f ====="
    ./host_test "$f" || failed=1
  done
  for t in host_test_*; do
    [ -x "$t" ] && [ ! -d "$t" ] || continue
    case "$t" in *.exe) continue ;; esac
    echo "===== $t ====="
    ./"$t" || failed=1
    ran=$((ran + 1))
  done
  echo "ran $ran host_test_* binaries"
  # A Makefile or glob change that stops running the suite fails loudly
  # rather than passing with no tests.
  if [ "$ran" -lt 15 ]; then
    echo "${CI:+::error::}expected at least 15 host_test_* binaries, ran $ran"
    failed=1
  fi
  return "$failed"
}

check_sim() {
  local t failed=0
  need_submodule
  make -C host_mock -j"$JOBS" sim_station
  for t in webapp/tests/test_sim_*.js; do
    echo "===== $t ====="
    node "$t" || failed=1
  done
  return "$failed"
}

check_tx_e2e() {
  need_submodule
  cmake -S tests/tx_e2e -B tests/tx_e2e/build -DCMAKE_BUILD_TYPE=Release
  cmake --build tests/tx_e2e/build -j"$JOBS"
  ctest --test-dir tests/tx_e2e/build --output-on-failure
}

# RFC 0006 P1. Truth is committed; this does not run jt9. The first run clones
# howard0su/ft8_lib at the pinned SHA for the 20m_busy WAVs.
check_decode_recall() {
  need_submodule
  python3 tools/decode_recall/report.py
}

# The sheet's on-screen-text check, and its page limit wherever Chrome is
# found. CI always prints it, and fails without Chrome.
check_getting_started() {
  need_npm
  if [ -n "${CI:-}" ]; then
    node webapp/tools/getting_started.mjs --pdf
  else
    node webapp/tools/getting_started.mjs --pdf-if-chrome
  fi
}

check_docs() {
  python3 tools/check_docs.py --base "$BASE"
}

check_commits() {
  python3 tools/check_commits.py "$BASE..$HEAD_REV"
}

check_sidekick() {
  need_idf
  need_submodule
  idf.py -C sidekick build
}

check_p4() {
  need_idf
  need_submodule
  idf.py -C poe_p4 build
}

# ---- run ------------------------------------------------------------------

mkdir -p "$LOG_DIR"
SUMMARY="" FAILED=0
for name in $CHECKS; do
  case "$SELECTED" in *" $name "*) ;; *) [ "$MODE" = changed ] && SUMMARY="$SUMMARY--    $name"$'\n'; continue ;; esac
  log="$LOG_DIR/$name.log"
  echo "== $name =="
  start=$SECONDS
  set +e
  if [ "$VERBOSE" = 1 ]; then
    ( set -e; "check_$name" ) 2>&1 | tee "$log"
    status=${PIPESTATUS[0]}
  else
    ( set -e; "check_$name" ) >"$log" 2>&1
    status=$?
  fi
  set -e
  took="$((SECONDS - start)) s"
  case "$status" in
    0) line="PASS  $name ($took)" ;;
    3) line="SKIP  $name: $(sed -n 's/^SKIP: //p' "$log" | tail -1)" ;;
    *) line="FAIL  $name ($took, $log)"; FAILED=1
       [ "$VERBOSE" = 1 ] || { tail -n 40 "$log"; echo; } ;;
  esac
  echo "$line"
  SUMMARY="$SUMMARY$line"$'\n'
done

echo
echo "== summary: base $(git rev-parse --short "$BASE")$( [ "$MODE" = changed ] && echo ", --changed") =="
printf '%s' "$SUMMARY"
exit "$FAILED"
