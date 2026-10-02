# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

# Separate idf.py project from poe_p4: ESP-IDF locks target and sdkconfig per
# project. Pinned here so `idf.py build` works without a manual `set-target`
# step.
set(IDF_TARGET esp32s3)

# Exact git SHA + dirty flag, not ESP-IDF's default `git describe` (tag-
# relative, fuzzy) — via the shared, override-aware logic in
# tools/git_version.cmake. Same repo, same commit, so "does this sidekick
# match the build" is an exact string comparison (docs/architecture/sidekick.md (Port A: the link to the main board)/
# §5.2c) — including under CI's MICROFT8_GIT_SHA pin, which a bare
# `git rev-parse HEAD` here would not otherwise honor. Must be set before
# project() — that's the only hook ESP-IDF's build system gives PROJECT_VER
# before it falls back to `git describe`.
set(GIT_DIR "${CMAKE_CURRENT_LIST_DIR}/..")
include("${CMAKE_CURRENT_LIST_DIR}/../tools/git_version.cmake")  # sets SHA, DIRTY
set(PROJECT_VER "${SHA}")
if(DIRTY)
    set(PROJECT_VER "${PROJECT_VER}-dirty")
endif()

# The frame codec is shared with the P4 rather than duplicated: two idf.py
# projects compiling one file is prong 1 of docs/STYLE.md's
# module-placement rule, and a second copy of a wire format is how the two ends drift.
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../components/porta_proto")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(sidekick)
