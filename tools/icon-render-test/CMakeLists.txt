cmake_minimum_required(VERSION 3.16)
project(icon_render_test C CXX)

# Match the firmware decoder default. File thumbnails must draw correctly
# with row-at-a-time decoding and no on-device scaling.
file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/../../sdkconfig.defaults" ram_default
     REGEX "^CONFIG_LV_BIN_DECODER_RAM_LOAD=y$")
if(ram_default)
    set(default_ram_load 1)
else()
    set(default_ram_load 0)
endif()
set(RAM_LOAD ${default_ram_load} CACHE STRING "Load complete file images")
set(LV_BUILD_CONF_PATH "${CMAKE_CURRENT_LIST_DIR}/lv_conf.h" CACHE PATH "" FORCE)
set(CONFIG_LV_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_BUILD_DEMOS OFF CACHE BOOL "" FORCE)
add_compile_definitions(TEST_RAM_LOAD=${RAM_LOAD})
add_subdirectory(../../managed_components/lvgl__lvgl lvgl)
add_executable(icon-render-test main.c)
target_link_libraries(icon-render-test lvgl::lvgl)
enable_testing()
add_test(NAME native-size-file-thumbnail COMMAND icon-render-test)
