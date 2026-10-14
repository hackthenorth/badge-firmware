# Solana Scavenger Hunt BLE -> HTTP relay.
#
# This is a SEPARATE ESP-IDF project from the badge firmware at the repo root.
# Build it from inside this directory, not from the root:
#
#   source ~/.espressif/tools/activate_idf_v5.5.3.sh
#   cd tools/relay_firmware && idf.py build
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# The relay carries the same LCD as the badge and renders with the exact
# same graphics system (esp_lvgl_port + LVGL, vendored in the badge's
# managed_components). MINIMAL_BUILD keeps the badge's other vendored
# components (lua, NFC, LEDs, ...) out of this binary.
set(EXTRA_COMPONENT_DIRS ${CMAKE_CURRENT_LIST_DIR}/../../managed_components)
idf_build_set_property(MINIMAL_BUILD ON)
idf_build_set_property(COMPILE_OPTIONS "-Wno-error=narrowing" APPEND)
project(relay_firmware)
