idf_component_register(
    SRCS "main.c" "ble_link.c" "buttons.c" "heartbeat.c" "lights.c" "net_post.c" "screen.c" "wifi_sta.c"
    INCLUDE_DIRS "."
    REQUIRES bt esp_wifi esp_http_client esp-tls nvs_flash esp_timer esp_event driver esp_lcd esp_lvgl_port led_strip
)

# LVGL headers use `#include "lvgl.h"` when this is defined (same as the badge
# firmware's main component).
target_compile_definitions(${COMPONENT_LIB} PRIVATE LV_LVGL_H_INCLUDE_SIMPLE)
