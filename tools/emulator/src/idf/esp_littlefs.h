#pragma once

#include "esp_err.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Reports usage of the in-memory /littlefs tree against the real partition
// size, rounded to LittleFS's 4 KB blocks.
esp_err_t esp_littlefs_info(const char *partition_label, size_t *total_bytes,
                            size_t *used_bytes);

#ifdef __cplusplus
}
#endif
