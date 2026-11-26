# QSO store bench (README.md): times the store's lookups on synthetic logs. Only the store and the ADIF
# formatter, not the firmware: no USB, no radio, no flash file system.
cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../components/qso_store"
                         "${CMAKE_CURRENT_LIST_DIR}/../../components/adif")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(qso_store_bench)
