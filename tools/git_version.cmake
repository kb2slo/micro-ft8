# Shared git SHA + dirty-flag computation, honoring the same MICROFT8_GIT_SHA
# override (CMake var or env var) CI uses to pin the reported SHA to the
# actual PR/commit head rather than trusting a bare `git rev-parse HEAD` —
# CI checks out a GitHub-synthesized merge commit for a PR, which is a
# different SHA than github.event.pull_request.head.sha, so a bare
# rev-parse there would silently disagree with the pinned value.
#
# Used by sidekick/CMakeLists.txt.
#
# Input:  GIT_DIR (required), MICROFT8_GIT_SHA (optional override)
# Output: SHA, DIRTY (0 or 1) — set in the caller's scope

if(NOT GIT_DIR)
  message(FATAL_ERROR "git_version.cmake needs GIT_DIR")
endif()

# Field-found 2026-09-20: SHA/DIRTY below only run at CMake *configure* time
# (that is the one hook ESP-IDF's build system gives PROJECT_VER before it
# falls back to `git describe` -- see sidekick/CMakeLists.txt's own comment).
# `ninja` correctly recompiles changed *source* files on every incremental
# `idf.py build` regardless, but without this, the version banner itself
# stays frozen at whatever commit/dirty-state existed the last time this
# build directory happened to reconfigure -- which cost real debugging time
# today when a sidekick's reported version sat two commits behind HEAD
# through several rebuilds, and looked exactly like a stale flash. Watching
# .git/HEAD and .git/index makes CMake automatically reconfigure (and thus
# recompute SHA/DIRTY) the next time `idf.py build` runs after any commit,
# checkout, or `git add` -- covering the case that actually bit us. Honest
# limit: editing an already-tracked file without staging it touches neither
# watched file, so DIRTY could still lag by one edit in that narrower case;
# SHA itself (which commit the build claims to be) does not have that gap,
# since HEAD only moves on a real commit or checkout.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${GIT_DIR}/.git/HEAD" "${GIT_DIR}/.git/index")

set(SHA "unknown")
set(_sha_in "")
if(MICROFT8_GIT_SHA)
  set(_sha_in "${MICROFT8_GIT_SHA}")
elseif(DEFINED ENV{MICROFT8_GIT_SHA} AND NOT "$ENV{MICROFT8_GIT_SHA}" STREQUAL "")
  set(_sha_in "$ENV{MICROFT8_GIT_SHA}")
endif()
if(_sha_in)
  string(SUBSTRING "${_sha_in}" 0 7 SHA)
else()
  execute_process(
    COMMAND git -C "${GIT_DIR}" rev-parse --short=7 HEAD
    RESULT_VARIABLE sha_rv
    OUTPUT_VARIABLE sha_out
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
  )
  if(sha_rv EQUAL 0 AND sha_out)
    set(SHA "${sha_out}")
  endif()
endif()

set(DIRTY 0)
execute_process(
  COMMAND git -C "${GIT_DIR}" status --porcelain
  RESULT_VARIABLE st_rv
  OUTPUT_VARIABLE porcelain
  OUTPUT_STRIP_TRAILING_WHITESPACE
  ERROR_QUIET
)
if(st_rv EQUAL 0 AND porcelain)
  set(DIRTY 1)
endif()
