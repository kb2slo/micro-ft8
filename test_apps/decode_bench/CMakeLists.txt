# Decode benchmark (docs/architecture/decode.md): times the firmware's decode steps on embedded corpus slots.
# One source for every board; pick the board with `idf.py set-target`. See README.md.
cmake_minimum_required(VERSION 3.16)

# Only the decoder, not the firmware: no USB, no UI, no radio.
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../components/ft8_lib")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(decode_bench)

# freq_osr=2 needs nfft 1920. The P4 firmware sets the same define; this
# bench image does not use poe_p4/CMakeLists.txt.
idf_component_get_property(ft8_lib_target ft8_lib COMPONENT_LIB)
target_compile_definitions(${ft8_lib_target} PRIVATE MONITOR_NFFT_MAX=1920)
