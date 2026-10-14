#pragma once

#include "esp_err.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SOC_MOD_CLK_CPU = 1,
    SOC_MOD_CLK_APB,
    SOC_MOD_CLK_XTAL,
} soc_module_clk_t;

typedef enum {
    ESP_CLK_TREE_SRC_FREQ_PRECISION_CACHED = 0,
    ESP_CLK_TREE_SRC_FREQ_PRECISION_APPROX,
    ESP_CLK_TREE_SRC_FREQ_PRECISION_EXACT,
} esp_clk_tree_src_freq_precision_t;

esp_err_t esp_clk_tree_src_get_freq_hz(soc_module_clk_t clk,
                                       esp_clk_tree_src_freq_precision_t precision,
                                       uint32_t *freq_hz);

#ifdef __cplusplus
}
#endif
