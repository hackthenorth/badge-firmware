#pragma once

#include "esp_err.h"
#include <cstdint>

using led_strip_handle_t = void *;
constexpr int LED_MODEL_WS2812 = 0;
constexpr int LED_STRIP_COLOR_COMPONENT_FMT_GRB = 0;
struct led_strip_config_t {
    int strip_gpio_num;
    uint32_t max_leds;
    int led_model;
    int color_component_format;
    struct { unsigned invert_out; } flags;
};
esp_err_t led_strip_set_pixel(led_strip_handle_t, uint32_t, uint32_t, uint32_t, uint32_t);
esp_err_t led_strip_clear(led_strip_handle_t);
esp_err_t led_strip_refresh(led_strip_handle_t);
