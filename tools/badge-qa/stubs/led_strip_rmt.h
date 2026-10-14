#pragma once

#include "led_strip.h"

constexpr int RMT_CLK_SRC_DEFAULT = 0;
struct led_strip_rmt_config_t {
    int clk_src;
    uint32_t resolution_hz;
    uint32_t mem_block_symbols;
};
esp_err_t led_strip_new_rmt_device(const led_strip_config_t *,
                                 const led_strip_rmt_config_t *, led_strip_handle_t *);
