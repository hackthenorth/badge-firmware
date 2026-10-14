idf_component_register(
    SRC_DIRS "." "hal" "apps" "apps/icons_generated"
    INCLUDE_DIRS "." "hal" "apps"
    PRIV_REQUIRES nvs_flash driver esp_lcd spi_flash console lua esp_event app_update bootloader_support bt json libsodium esp_app_format
)

# LVGLImage.py-generated icons use `#ifdef LV_LVGL_H_INCLUDE_SIMPLE` to pick
# `#include "lvgl.h"` over `#include "lvgl/lvgl.h"`. Match the include style
# used elsewhere in this component.
target_compile_definitions(${COMPONENT_LIB} PRIVATE LV_LVGL_H_INCLUDE_SIMPLE)

# LUAI_MAXCCALLS for the managed lua component is set from the root
# CMakeLists (after project()), where the component target exists. Setting
# it here would fail: main processes before the lua target is registered.

# Engineer/factory firmware. When BADGE_FACTORY_TEST=1 is set in the environment
# at configure time, app_main boots into the factory hardware test instead of
# the launcher (see FACTORY_TEST_BUILD in main.cpp). Read from the environment
# rather than a Kconfig option so it can be flipped with a single env var and no
# source changes (used locally to build the committed factory image). NOTE: env
# vars aren't tracked by CMake, so switching the value on an existing build dir
# needs `idf.py fullclean` (or reconfigure) before rebuilding.
if("$ENV{BADGE_FACTORY_TEST}" STREQUAL "1")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE FACTORY_TEST_BUILD=1)
    message(STATUS "BADGE_FACTORY_TEST=1 -> factory-test boot enabled")
endif()

# Overnight / soak testing: BADGE_NO_SLEEP=1 compiles out idle sleep
# entirely (hal_sleep_tick becomes a no-op). Test builds only — never ship
# with this set. Same reconfigure caveat as above when toggling.
if("$ENV{BADGE_NO_SLEEP}" STREQUAL "1")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE BADGE_NO_SLEEP=1)
    message(STATUS "BADGE_NO_SLEEP=1 -> idle sleep disabled (test only)")
endif()

# Monotonic build number. Onboarding replays whenever the number stored on the
# badge is older than the one compiled in, so bumping this is how we force
# every badge to re-run the tutorial after a meaningful change.
#
# Set BADGE_BUILD in the environment for release builds. Left unset it stays 0,
# which means a dev rebuild does NOT re-trigger onboarding on every flash —
# that would be unusable day to day.
#
# NOTE: re-flashing a badge with a HIGHER number replays onboarding for that
# attendee. That is accepted; for an event-week hotfix, keep BADGE_BUILD the
# same as the build already in the field.
if(DEFINED ENV{BADGE_BUILD})
    target_compile_definitions(${COMPONENT_LIB} PRIVATE BADGE_BUILD_NUMBER=$ENV{BADGE_BUILD})
    message(STATUS "BADGE_BUILD=$ENV{BADGE_BUILD}")
endif()
