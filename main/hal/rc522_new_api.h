#pragma once

#include "esp_err.h"
#include "rc522_driver.h"

#ifdef __cplusplus
extern "C" {
#endif

// Allocate an rc522_driver_handle_t backed by the new i2c_master API on
// hal_i2c's shared bus. hal_i2c_init() must have been called first.
esp_err_t rc522_new_api_create(uint8_t device_address, rc522_driver_handle_t *out);

#ifdef __cplusplus
}
#endif
