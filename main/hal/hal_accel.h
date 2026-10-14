#pragma once

#include "esp_err.h"

struct AccelSample {
    float x_mg;
    float y_mg;
    float z_mg;
};

enum class AccelOutputRate {
    Hz100,
    Hz800,
};

esp_err_t hal_accel_init();
esp_err_t hal_accel_set_output_rate(AccelOutputRate rate);
esp_err_t hal_accel_read(AccelSample *out);

// Suspends/resumes the background polling task. While suspended no I2C
// traffic is generated (direct hal_accel_read() still works) and the cache
// reads back invalid. NFC enables this while scanning so the shared bus has
// a single master-side user; the registry's force-disable of NFC on every
// app switch resumes polling automatically.
void hal_accel_set_suspended(bool suspended);

// Returns the latest background sample without waiting for an I2C transaction.
// ESP_ERR_INVALID_STATE means the cache has not received a sample yet.
esp_err_t hal_accel_cached_read(AccelSample *out);
