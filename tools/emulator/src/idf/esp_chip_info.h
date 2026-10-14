#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { CHIP_ESP32C3 = 5 } esp_chip_model_t;

typedef struct {
    esp_chip_model_t model;
    uint32_t features;
    uint16_t revision;
    uint8_t cores;
} esp_chip_info_t;

void esp_chip_info(esp_chip_info_t *out);

#ifdef __cplusplus
}
#endif
