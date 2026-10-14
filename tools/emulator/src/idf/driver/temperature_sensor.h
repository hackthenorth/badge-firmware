#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct EmuTemperatureSensor *temperature_sensor_handle_t;

typedef struct {
    int range_min;
    int range_max;
} temperature_sensor_config_t;

#define TEMPERATURE_SENSOR_CONFIG_DEFAULT(min, max) { .range_min = (min), .range_max = (max) }

esp_err_t temperature_sensor_install(const temperature_sensor_config_t *config,
                                     temperature_sensor_handle_t *out);
esp_err_t temperature_sensor_uninstall(temperature_sensor_handle_t sensor);
esp_err_t temperature_sensor_enable(temperature_sensor_handle_t sensor);
esp_err_t temperature_sensor_disable(temperature_sensor_handle_t sensor);
esp_err_t temperature_sensor_get_celsius(temperature_sensor_handle_t sensor, float *out);

#ifdef __cplusplus
}
#endif
