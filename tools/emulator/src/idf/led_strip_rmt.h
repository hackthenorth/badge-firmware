#pragma once

#include "led_strip.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { RMT_CLK_SRC_DEFAULT = 0 } rmt_clock_source_t;

typedef struct {
    rmt_clock_source_t clk_src;
    uint32_t resolution_hz;
    uint32_t mem_block_symbols;
    struct {
        uint32_t with_dma : 1;
    } flags;
} led_strip_rmt_config_t;

esp_err_t led_strip_new_rmt_device(const led_strip_config_t *led_config,
                                   const led_strip_rmt_config_t *rmt_config,
                                   led_strip_handle_t *ret_strip);

#ifdef __cplusplus
}
#endif
