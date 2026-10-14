# Badge sync station: BLE bulk receive -> HTTPS upload.
#
# This is a SEPARATE ESP-IDF project from the badge firmware at the repo root,
# and from tools/relay_firmware. Build it from inside this directory:
#
#   source ~/.espressif/tools/activate_idf_v5.5.3.sh
#   cd tools/sync_station && idf.py set-target esp32c3 && idf.py build
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# The station carries the same LCD as the badge and renders with the exact
# same graphics system (esp_lvgl_port + LVGL, vendored in the badge's
# managed_components). MINIMAL_BUILD keeps the badge's other vendored
# components (lua, NFC, LEDs, ...) out of this binary.
set(EXTRA_COMPONENT_DIRS ${CMAKE_CURRENT_LIST_DIR}/../../managed_components)
idf_build_set_property(MINIMAL_BUILD ON)
idf_build_set_property(COMPILE_OPTIONS "-Wno-error=narrowing" APPEND)
project(sync_station)
