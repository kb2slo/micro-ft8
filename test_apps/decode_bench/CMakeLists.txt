# Decode benchmark (RFC 0006 §5 P0, B87): times the firmware's decode steps on embedded corpus slots.
# One source for every board; pick the board with `idf.py set-target`. See README.md.
cmake_minimum_required(VERSION 3.16)

# Only the decoder, not the firmware: no USB, no UI, no radio.
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../components/ft8_lib")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(decode_bench)

# freq_osr=2 needs nfft 1920; the firmware's monitor.c defaults to 960 (RFC 0006 §4.2).
idf_component_get_property(ft8_lib_target ft8_lib COMPONENT_LIB)
target_compile_definitions(${ft8_lib_target} PRIVATE MONITOR_NFFT_MAX=1920)
