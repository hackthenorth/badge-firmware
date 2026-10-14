#pragma once

#include "esp_err.h"
#include <cstddef>

esp_err_t hal_config_init();

int  hal_config_get_int(const char *slug, const char *key, int def);
void hal_config_set_int(const char *slug, const char *key, int v);

bool hal_config_get_string(const char *slug, const char *key,
                           char *out, size_t out_sz, const char *def);
void hal_config_set_string(const char *slug, const char *key, const char *v);
