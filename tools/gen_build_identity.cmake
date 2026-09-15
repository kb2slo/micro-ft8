# Generate build_identity.h and merged_bin_name.txt from git.
# Required: GIT_DIR, OUT_DIR
# Optional: MICROFT8_BUILD_KIND (default dev)
#           MICROFT8_RELEASE_VERSION (required when kind is rel)
#
# A `rel` build is a claim the device makes about itself on screen, so it is
# gated rather than merely labelled: it must come from CI, carry a version
# handed to it, and be built from a clean tree. Fail loudly on all three
# instead of quietly downgrading to dev -- a release that silently built as
# dev is exactly the outcome this exists to prevent. This stops an accident,
# not an adversary: anyone can set CI=1 locally. docs/RELEASE_PROCESS.md.

if(NOT GIT_DIR OR NOT OUT_DIR)
  message(FATAL_ERROR "gen_build_identity.cmake needs GIT_DIR and OUT_DIR")
endif()

if(NOT MICROFT8_BUILD_KIND)
  set(MICROFT8_BUILD_KIND "dev")
endif()

include("${CMAKE_CURRENT_LIST_DIR}/git_version.cmake")  # sets SHA, DIRTY

if(DIRTY)
  set(DIRTY_INFIX "-dirty")
  set(DIRTY_MARK "*")
else()
  set(DIRTY_INFIX "")
  set(DIRTY_MARK "")
endif()

set(KIND "${MICROFT8_BUILD_KIND}")
if(KIND STREQUAL "release" OR KIND STREQUAL "rel")
  set(KIND "rel")

  if(NOT "$ENV{CI}")
    message(FATAL_ERROR
      "rel builds come from CI only. Tag with tools/cut_release.py and let the "
      "workflow build it; a desk build cannot be a release.")
  endif()
  if(NOT MICROFT8_RELEASE_VERSION)
    message(FATAL_ERROR
      "rel build needs MICROFT8_RELEASE_VERSION -- the version comes from the "
      "v* tag, so the number on screen cannot disagree with the tag it was "
      "cut from.")
  endif()
  if(DIRTY)
    message(FATAL_ERROR
      "rel build refuses a dirty tree: '${MICROFT8_RELEASE_VERSION}' would name "
      "a commit that does not contain what was built.")
  endif()

  # A release states its version and nothing else. A dev build has no version
  # to state, so it names its kind and commit instead -- the presence or
  # absence of a version number is what tells the two apart on the screen.
  set(VERSION_LINE "${MICROFT8_RELEASE_VERSION}")
  set(MERGED_BIN_NAME "${SHA}-microft8-${MICROFT8_RELEASE_VERSION}.bin")
else()
  set(KIND "dev")
  set(VERSION_LINE "dev ${SHA}${DIRTY_MARK}")
  # Hash first so M5Launcher's truncated names still show the unique bit.
  set(MERGED_BIN_NAME "${SHA}${DIRTY_INFIX}-microft8-dev.bin")
endif()

file(MAKE_DIRECTORY "${OUT_DIR}")

# microft8_build_identity is an add_custom_target (main/CMakeLists.txt depends
# on it), so it reruns unconditionally on every build by design — that's the
# only way to catch a newly-dirtied tree. But a plain file(WRITE) rewrites
# the destination's mtime even when the content is byte-identical, and
# main.cpp #includes build_identity.h — so an unconditional WRITE meant
# ninja recompiled + relinked main.cpp on every single `idf.py build`, even
# with zero source changes, every time git state (SHA/dirty) happened to be
# unchanged. Write to a .tmp file and copy_if_different so the real output's
# mtime only moves when the content actually does.
file(WRITE "${OUT_DIR}/merged_bin_name.txt.tmp" "${MERGED_BIN_NAME}\n")
execute_process(COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${OUT_DIR}/merged_bin_name.txt.tmp" "${OUT_DIR}/merged_bin_name.txt")
file(REMOVE "${OUT_DIR}/merged_bin_name.txt.tmp")

file(WRITE "${OUT_DIR}/build_identity.h.tmp" "\
#pragma once

#define MICROFT8_GIT_SHA \"${SHA}\"
#define MICROFT8_GIT_DIRTY ${DIRTY}
#define MICROFT8_BUILD_KIND \"${KIND}\"
#define MICROFT8_VERSION_LINE \"${VERSION_LINE}\"
#define MICROFT8_MERGED_BIN_NAME \"${MERGED_BIN_NAME}\"
")
execute_process(COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${OUT_DIR}/build_identity.h.tmp" "${OUT_DIR}/build_identity.h")
file(REMOVE "${OUT_DIR}/build_identity.h.tmp")

message(STATUS "Micro-FT8 identity: ${VERSION_LINE} -> ${MERGED_BIN_NAME}")
