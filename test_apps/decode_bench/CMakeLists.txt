# Decode benchmark (docs/architecture/decode.md): times the firmware's decode steps on embedded corpus slots.
# One source for every board; pick the board with `idf.py set-target`. See README.md.
cmake_minimum_required(VERSION 3.16)

# Only the decoder, not the firmware: no USB, no UI, no radio.
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../components/ft8_lib")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(decode_bench)
