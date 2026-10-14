#pragma once

#include "esp_err.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ESP_SLEEP_WAKEUP_UNDEFINED = 0,
    ESP_SLEEP_WAKEUP_ALL,
    ESP_SLEEP_WAKEUP_EXT0,
    ESP_SLEEP_WAKEUP_EXT1,
    ESP_SLEEP_WAKEUP_TIMER,
    ESP_SLEEP_WAKEUP_TOUCHPAD,
    ESP_SLEEP_WAKEUP_ULP,
    ESP_SLEEP_WAKEUP_GPIO,
} esp_sleep_wakeup_cause_t;

esp_err_t esp_sleep_enable_gpio_wakeup(void);
esp_err_t esp_sleep_enable_timer_wakeup(uint64_t time_in_us);

// Cannot block the browser thread. Returns at once; the main loop then stops
// running LVGL until START is pressed or the timer wakeup elapses, which is
// what the real call's blocking amounts to.
esp_err_t esp_light_sleep_start(void);
esp_sleep_wakeup_cause_t esp_sleep_get_wakeup_cause(void);

#ifdef __cplusplus
}
#endif
