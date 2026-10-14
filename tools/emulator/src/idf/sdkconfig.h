#pragma once

// Force-included into firmware and Lua sources, as ESP-IDF does. Holds only
// the options those sources read; LVGL's Kconfig mirror is ../lv_kconfig.h.
#define CONFIG_IDF_TARGET "esp32c3"
#define CONFIG_FREERTOS_HZ 100
#define CONFIG_PM_ENABLE 1
#define CONFIG_LUA_MAXSTACK 1000000

// musl's <limits.h> defines PAGE_SIZE (the wasm page size) as a macro, which
// collides with firmware constants of the same name. newlib has no such macro.
#include <limits.h>
#undef PAGE_SIZE
