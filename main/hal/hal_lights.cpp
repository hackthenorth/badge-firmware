#include "hal_lights.h"
#include "hal_identity.h"
#include "pins.h"
#include "led_strip.h"
#include "led_strip_rmt.h"
#include "esp_log.h"
#include "esp_timer.h"

#include <cstring>

static const char *TAG = "hal_lights";
static led_strip_handle_t s_strip = nullptr;
static uint8_t s_pixels[LED_COUNT][3] = {};
static bool s_flashing = false;
static int64_t s_flash_start_us = 0;
static int s_flash_phase = -1;
// Treat the full logical 0-255 range as 0-100%, but map logical 100% to a
// ~26% duty cap, gamma-corrected (squared). This doubles the previous
// physical maximum (33 -> 66) while preserving the dimming curve.
// Apply the cap here once for every app, including success flashes.
static constexpr uint8_t kPhysicalBrightnessLimit = (255u * 26u + 50u) / 100u;

static uint8_t scale_to_physical_limit(uint8_t logical)
{
    uint16_t v = (uint16_t)logical * logical; // 0..65025
    return (uint8_t)((v * kPhysicalBrightnessLimit + 32512u) / 65025u);
}

void hal_lights_init()
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
    esp_err_t err = led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &s_strip);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "led_strip_new_rmt_device failed: %s", esp_err_to_name(err));
        return;
    }
    led_strip_clear(s_strip);
    led_strip_refresh(s_strip);
}

void hal_lights_set(size_t idx, uint8_t r, uint8_t g, uint8_t b)
{
    if (!s_strip || idx >= LED_COUNT) return;
    s_pixels[idx][0] = r;
    s_pixels[idx][1] = g;
    s_pixels[idx][2] = b;
    if (!s_flashing) led_strip_set_pixel(s_strip, idx,
                        scale_to_physical_limit(r),
                        scale_to_physical_limit(g),
                        scale_to_physical_limit(b));
}

void hal_lights_clear()
{
    if (!s_strip) return;
    memset(s_pixels, 0, sizeof(s_pixels));
    if (!s_flashing) led_strip_clear(s_strip);
}

void hal_lights_show()
{
    if (!s_strip || s_flashing) return;
    led_strip_refresh(s_strip);
}

void hal_lights_get_role_color(uint8_t out[3])
{
    if (hal_identity_is_provisioned()) {
        const BadgeIdentity *id = hal_identity_get();
        memcpy(out, id->role_color, sizeof(id->role_color));
    } else {
        out[0] = 0x00;
        out[1] = 0xd9;
        out[2] = 0xff;
    }
    // Preserve hue while ensuring even dark role colours are visible.
    uint8_t peak = out[0];
    if (out[1] > peak) peak = out[1];
    if (out[2] > peak) peak = out[2];
    if (peak == 0) {
        out[0] = 0; out[1] = 0xd9; out[2] = 0xff;
    } else {
        for (int i = 0; i < 3; ++i)
            out[i] = (uint8_t)(((uint16_t)out[i] * 255) / peak);
    }
}

void hal_lights_flash_success()
{
    if (!s_strip) return;
    s_flashing = true;
    s_flash_start_us = esp_timer_get_time();
    s_flash_phase = -1;
    hal_lights_tick();
}

void hal_lights_tick()
{
    if (!s_strip || !s_flashing) return;
    const int64_t elapsed = esp_timer_get_time() - s_flash_start_us;
    // 250 ms on, 250 ms off: exactly three pulses over 1.5 seconds.
    const int phase = (int)(elapsed / 250000);
    if (phase == s_flash_phase) return;
    s_flash_phase = phase;
    if (phase >= 6) {
        s_flashing = false;
        for (size_t i = 0; i < LED_COUNT; ++i) {
            led_strip_set_pixel(s_strip, i,
                scale_to_physical_limit(s_pixels[i][0]),
                scale_to_physical_limit(s_pixels[i][1]),
                scale_to_physical_limit(s_pixels[i][2]));
        }
    } else {
        for (size_t i = 0; i < LED_COUNT; ++i)
            led_strip_set_pixel(s_strip, i, 0,
                phase % 2 == 0 ? scale_to_physical_limit(255) : 0, 0);
    }
    led_strip_refresh(s_strip);
}
