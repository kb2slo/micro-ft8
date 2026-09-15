# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(micro_ft8)

# rel is CI-only and gated in tools/gen_build_identity.cmake; leave this alone
# and let the release workflow set it (docs/RELEASE_PROCESS.md).
set(MICROFT8_BUILD_KIND "$ENV{MICROFT8_BUILD_KIND}" CACHE STRING "Build kind: dev or rel")
if(NOT MICROFT8_BUILD_KIND)
  set(MICROFT8_BUILD_KIND "dev")
endif()
set(MICROFT8_GEN_DIR "${CMAKE_BINARY_DIR}/generated")

# Configure-time identity so the first compile finds build_identity.h;
# the custom target refreshes SHA/dirty on every build.
execute_process(
    COMMAND ${CMAKE_COMMAND}
        -DGIT_DIR=${CMAKE_SOURCE_DIR}
        -DOUT_DIR=${MICROFT8_GEN_DIR}
        -DMICROFT8_BUILD_KIND=${MICROFT8_BUILD_KIND}
        -DMICROFT8_RELEASE_VERSION=$ENV{MICROFT8_RELEASE_VERSION}
        -DMICROFT8_GIT_SHA=$ENV{MICROFT8_GIT_SHA}
        -P ${CMAKE_SOURCE_DIR}/tools/gen_build_identity.cmake
)
add_custom_target(microft8_build_identity
    COMMAND ${CMAKE_COMMAND} -E make_directory ${MICROFT8_GEN_DIR}
    COMMAND ${CMAKE_COMMAND}
        -DGIT_DIR=${CMAKE_SOURCE_DIR}
        -DOUT_DIR=${MICROFT8_GEN_DIR}
        -DMICROFT8_BUILD_KIND=${MICROFT8_BUILD_KIND}
        -DMICROFT8_RELEASE_VERSION=$ENV{MICROFT8_RELEASE_VERSION}
        -DMICROFT8_GIT_SHA=$ENV{MICROFT8_GIT_SHA}
        -P ${CMAKE_SOURCE_DIR}/tools/gen_build_identity.cmake
    COMMENT "Refreshing Micro-FT8 build identity"
    VERBATIM
    BYPRODUCTS
        ${MICROFT8_GEN_DIR}/build_identity.h
        ${MICROFT8_GEN_DIR}/merged_bin_name.txt
)

# Full merged image for the FATFS-only partition table. First boot formats
# the new 3 MB FATFS when needed. Also copy a hash-first name for Launcher.
add_custom_command(TARGET app POST_BUILD
    COMMAND ${PYTHON} -m esptool --chip esp32s3 merge_bin -o "MicroFT8_Merged_Auto.bin" @flash_args
    COMMAND ${CMAKE_COMMAND}
        -DBUILD_DIR=${CMAKE_BINARY_DIR}
        -P ${CMAKE_SOURCE_DIR}/tools/copy_named_merged_bin.cmake
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    COMMENT "Generating merged binaries MicroFT8_Merged_Auto.bin and hash-first name"
)
