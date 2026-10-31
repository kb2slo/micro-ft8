# Decode benchmark (docs/architecture/decode.md): times the firmware's decode steps on embedded corpus slots.
# One source for every board; pick the board with `idf.py set-target`. See README.md.
cmake_minimum_required(VERSION 3.16)

# Only the decoder, not the firmware: no USB, no UI, no radio.
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../components/ft8_lib")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(decode_bench)

# Same waterfall sizing and placement as the P4 firmware (poe_p4/CMakeLists.txt;
# row p3x6). Without MONITOR_WF_STATIC_BYTES the monitor mallocs, which is not
# what the firmware times.
idf_component_get_property(ft8_lib_target ft8_lib COMPONENT_LIB)
target_compile_definitions(${ft8_lib_target} PRIVATE
    MONITOR_NFFT_MAX=1920
    MONITOR_WF_STATIC_BYTES=161076)
