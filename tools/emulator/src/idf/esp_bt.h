#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ESP_BT_CONTROLLER_STATUS_IDLE = 0,
    ESP_BT_CONTROLLER_STATUS_INITED,
    ESP_BT_CONTROLLER_STATUS_ENABLED,
    ESP_BT_CONTROLLER_STATUS_NUM,
} esp_bt_controller_status_t;

// Tracks the emulated radio: idle until the first enable, then enabled for
// the rest of the boot (the firmware never tears NimBLE down).
esp_bt_controller_status_t esp_bt_controller_get_status(void);

#ifdef __cplusplus
}
#endif
