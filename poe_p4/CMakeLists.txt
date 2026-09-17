# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

# Separate idf.py project from the ADV app (micro_ft8) and from sidekick,
# mirroring sidekick's own CMakeLists.txt: ESP-IDF locks target and sdkconfig
# per project, so a board on a different chip (M5Stack Unit PoE-P4, ESP32-P4 —
# not the ADV's/sidekick's ESP32-S3) cannot share either project's configure.
# See docs/rfcs/0005-p4-poe-headless-port.md ("RFC 0005"), Phase 0.
set(IDF_TARGET esp32p4)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(poe_p4)
