// Dim WS2812 status lights: same strip, pin, and RMT driver as the badge
// (main/hal/hal_lights.cpp), but status-only — one color for the whole
// strip, driven far dimmer than any badge app.

#include "lights.h"

#include "esp_log.h"
#include "led_strip.h"
#include "led_strip_rmt.h"

// Same as the badge (main/pins.h): 6-LED strip, GPIO3. Sending 6 frames to
// a shorter strip is harmless (extra frames fall off the end).
#define PIN_LED_DIN 3
#define LED_COUNT 6

static const char *TAG = "lights";
static led_strip_handle_t s_strip;

// Badge curve (hal_lights.cpp): logical 0-255 maps onto ~26% duty,
// gamma-corrected (squared).
static uint8_t scale_badge(uint8_t logical)
{
    uint16_t v = (uint16_t)logical * logical;   // 0..65025
    return (uint8_t)((v * 66u + 32512u) / 65025u);
}

void lights_init(void)
{
    led_strip_config_t strip_cfg = {
        .strip_gpio_num = PIN_LED_DIN,
        .max_leds = LED_COUNT,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
        .flags = { .invert_out = 0 },
    };
    led_strip_rmt_config_t rmt_cfg = {};
    rmt_cfg.clk_src = RMT_CLK_SRC_DEFAULT;
    rmt_cfg.resolution_hz = 10 * 1000 * 1000;
    rmt_cfg.mem_block_symbols = 64;
    if (led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &s_strip) != ESP_OK) {
        ESP_LOGW(TAG, "no strip, running without lights");
        s_strip = NULL;
        return;
    }
    led_strip_clear(s_strip);
    led_strip_refresh(s_strip);
    ESP_LOGI(TAG, "lights up (dim)");
}

void lights_status(uint8_t r, uint8_t g, uint8_t b)
{
    if (!s_strip) return;
    // Extra divide-by-4 past the badge curve: status glow, not a lamp.
    uint8_t pr = (uint8_t)(scale_badge(r) >> 2);
    uint8_t pg = (uint8_t)(scale_badge(g) >> 2);
    uint8_t pb = (uint8_t)(scale_badge(b) >> 2);
    for (int i = 0; i < LED_COUNT; i++) {
        led_strip_set_pixel(s_strip, (uint32_t)i, pr, pg, pb);
    }
    led_strip_refresh(s_strip);
}
