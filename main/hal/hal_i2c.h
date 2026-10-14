#pragma once

#include "driver/i2c_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t hal_i2c_init(void);
i2c_master_bus_handle_t hal_i2c_bus(void);

#ifdef __cplusplus
}
#endif
