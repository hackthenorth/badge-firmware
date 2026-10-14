#pragma once

#include "esp_err.h"

esp_err_t hal_fs_init();

bool hal_fs_mkdir_p(const char *path);

// Bytes used / total on the LittleFS partition. Returns false and leaves the
// outputs untouched if the partition can't be queried. Wraps esp_littlefs_info
// so the partition label stays in one place.
bool hal_fs_usage(size_t *used_out, size_t *total_out);
