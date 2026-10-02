# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

# Separate idf.py project from sidekick, as in sidekick's own CMakeLists.txt:
# ESP-IDF locks target and sdkconfig per project, so a board on a different
# chip (M5Stack Unit PoE-P4, ESP32-P4 — not the sidekick's ESP32-S3) cannot
# share its configure.
# Board facts: docs/architecture/main_board.md (P4).
set(IDF_TARGET esp32p4)

# Only the specific components this project needs, not the whole components/
# tree.
set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/../components/porta_proto"
    "${CMAKE_CURRENT_LIST_DIR}/../components/porta"
    "${CMAKE_CURRENT_LIST_DIR}/../components/radio_control"
    "${CMAKE_CURRENT_LIST_DIR}/../components/swr_sweep"
    "${CMAKE_CURRENT_LIST_DIR}/../components/dial_history"
    "${CMAKE_CURRENT_LIST_DIR}/../components/decode_types"
    "${CMAKE_CURRENT_LIST_DIR}/../components/decode_tx_state"
    "${CMAKE_CURRENT_LIST_DIR}/../components/callsign_hashtable"
    "${CMAKE_CURRENT_LIST_DIR}/../components/autoseq"
    "${CMAKE_CURRENT_LIST_DIR}/../components/protocol"
    "${CMAKE_CURRENT_LIST_DIR}/../components/debug_log"
    "${CMAKE_CURRENT_LIST_DIR}/../components/rtc_core"
    "${CMAKE_CURRENT_LIST_DIR}/../components/adif"
    "${CMAKE_CURRENT_LIST_DIR}/../components/resample"
    "${CMAKE_CURRENT_LIST_DIR}/../components/ft8_audio_pipeline"
    "${CMAKE_CURRENT_LIST_DIR}/../components/decode_pipeline"
    "${CMAKE_CURRENT_LIST_DIR}/../components/ft8_lib"
    "${CMAKE_CURRENT_LIST_DIR}/../components/storage_service"
    "${CMAKE_CURRENT_LIST_DIR}/../components/station"
    "${CMAKE_CURRENT_LIST_DIR}/../components/qso_log"
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(poe_p4)

# Single flashable image -- a release should be able to ship
# one file, not three offsets an operator has to know by hand.
add_custom_command(TARGET app POST_BUILD
    COMMAND ${PYTHON} -m esptool --chip esp32p4 merge_bin -o "poe_p4_merged.bin" @flash_args
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    COMMENT "Generating merged image poe_p4_merged.bin"
)
