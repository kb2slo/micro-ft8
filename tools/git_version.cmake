# Shared git SHA + dirty-flag computation, honoring the same MICROFT8_GIT_SHA
# override (CMake var or env var) CI uses to pin the reported SHA to the
# actual PR/commit head rather than trusting a bare `git rev-parse HEAD` —
# CI checks out a GitHub-synthesized merge commit for a PR, which is a
# different SHA than github.event.pull_request.head.sha, so a bare
# rev-parse there would silently disagree with the pinned value.
#
# Used by sidekick/CMakeLists.txt and poe_p4/CMakeLists.txt.
#
# Input:  GIT_DIR (required), MICROFT8_GIT_SHA (optional override),
#         MICROFT8_VERSION (optional: the version CI's build of a tag or of
#         main reports, from tools/build_version.py)
# Output: SHA, DIRTY (0 or 1), BUILD_VERSION — set in the caller's scope

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

# What the image reports as its version (PROJECT_VER). A build on a channel
# reports the version CI hands it, "0.1.0+<sha>" or "b1322+<sha>"
# (docs/RELEASE_PROCESS.md, An image carries its version); any other build its
# SHA. A changed tree never takes a channel's version, so nothing built at a
# bench is taken for a release; and a version naming another commit stops the
# build, since it would label this code as that one.
set(_ver_in "")
if(MICROFT8_VERSION)
  set(_ver_in "${MICROFT8_VERSION}")
elseif(DEFINED ENV{MICROFT8_VERSION} AND NOT "$ENV{MICROFT8_VERSION}" STREQUAL "")
  set(_ver_in "$ENV{MICROFT8_VERSION}")
endif()
if(DIRTY)
  set(BUILD_VERSION "${SHA}-dirty")
elseif(_ver_in)
  string(FIND "${_ver_in}" "+${SHA}" _ver_at REVERSE)
  string(LENGTH "${_ver_in}" _ver_len)
  string(LENGTH "+${SHA}" _sha_len)
  math(EXPR _ver_want "${_ver_len} - ${_sha_len}")
  if(_ver_at LESS 1 OR NOT _ver_at EQUAL _ver_want)
    message(FATAL_ERROR "MICROFT8_VERSION '${_ver_in}' does not end in this commit, +${SHA}")
  endif()
  set(BUILD_VERSION "${_ver_in}")
else()
  set(BUILD_VERSION "${SHA}")
endif()
