#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

uint32_t esp_rom_crc32_le(uint32_t crc, const uint8_t *buf, uint32_t len);

#ifdef __cplusplus
}
#endif
