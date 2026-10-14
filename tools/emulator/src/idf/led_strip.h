#pragma once

#include "esp_err.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct EmuLedStrip *led_strip_handle_t;

typedef enum { LED_MODEL_WS2812 = 0, LED_MODEL_SK6812 } led_model_t;
typedef enum { LED_STRIP_COLOR_COMPONENT_FMT_GRB = 0 } led_color_component_format_t;

typedef struct {
    int strip_gpio_num;
    uint32_t max_leds;
    led_model_t led_model;
    led_color_component_format_t color_component_format;
    struct {
        uint32_t invert_out : 1;
    } flags;
} led_strip_config_t;

// Values are the physical (post-gamma, post-cap) channel levels the WS2812s
// would receive; the page turns them back into visible brightness.
esp_err_t led_strip_set_pixel(led_strip_handle_t strip, uint32_t index,
                              uint32_t red, uint32_t green, uint32_t blue);
esp_err_t led_strip_clear(led_strip_handle_t strip);
esp_err_t led_strip_refresh(led_strip_handle_t strip);

#ifdef __cplusplus
}
#endif
