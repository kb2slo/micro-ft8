# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

# Separate idf.py project from the ADV app (micro_ft8) and from sidekick,
# mirroring sidekick's own CMakeLists.txt: ESP-IDF locks target and sdkconfig
# per project, so a board on a different chip (M5Stack Unit PoE-P4, ESP32-P4 —
# not the ADV's/sidekick's ESP32-S3) cannot share either project's configure.
# Board facts: docs/architecture/main_board.md (P4).
set(IDF_TARGET esp32p4)

# Shared with the ADV rather than duplicated (docs/STYLE.md (Two boards, one source), docs/STYLE.md's
# module-placement rule: more than one idf.py project compiles it --
# the same pattern sidekick already uses for porta_proto). Only the specific
# components this project actually needs, not the whole components/ tree
# (which would also expose ADV-only ones like M5Unified/board_cardputer_adv
# that this board can't build).
set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/../components/porta_proto"
    "${CMAKE_CURRENT_LIST_DIR}/../components/porta"
    "${CMAKE_CURRENT_LIST_DIR}/../components/radio_control"
    "${CMAKE_CURRENT_LIST_DIR}/../components/dial_history"
    "${CMAKE_CURRENT_LIST_DIR}/../components/decode_types"
    "${CMAKE_CURRENT_LIST_DIR}/../components/decode_tx_state"
    "${CMAKE_CURRENT_LIST_DIR}/../components/callsign_hashtable"
    "${CMAKE_CURRENT_LIST_DIR}/../components/autoseq"
    "${CMAKE_CURRENT_LIST_DIR}/../components/protocol"
    "${CMAKE_CURRENT_LIST_DIR}/../components/debug_log"
    "${CMAKE_CURRENT_LIST_DIR}/../components/rtc_core"
    "${CMAKE_CURRENT_LIST_DIR}/../components/external_rtc"
    "${CMAKE_CURRENT_LIST_DIR}/../components/board_power"
    "${CMAKE_CURRENT_LIST_DIR}/../components/adif"
    "${CMAKE_CURRENT_LIST_DIR}/../components/resample"
    "${CMAKE_CURRENT_LIST_DIR}/../components/ft8_audio_pipeline"
    "${CMAKE_CURRENT_LIST_DIR}/../components/decode_pipeline"
    "${CMAKE_CURRENT_LIST_DIR}/../components/ft8_lib"
    "${CMAKE_CURRENT_LIST_DIR}/../components/storage_service"
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(poe_p4)

# Single flashable image, same shape as the ADV's own POST_BUILD merge step
# (root CMakeLists.txt) but for esp32p4 -- a release should be able to ship
# one file, not three offsets an operator has to know by hand.
add_custom_command(TARGET app POST_BUILD
    COMMAND ${PYTHON} -m esptool --chip esp32p4 merge_bin -o "poe_p4_merged.bin" @flash_args
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    COMMENT "Generating merged image poe_p4_merged.bin"
)
