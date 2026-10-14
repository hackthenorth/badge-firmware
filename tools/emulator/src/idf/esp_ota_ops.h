#pragma once

#include "esp_partition.h"

#ifdef __cplusplus
extern "C" {
#endif

const esp_partition_t *esp_ota_get_running_partition(void);

#ifdef __cplusplus
}
#endif
