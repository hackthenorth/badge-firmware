#pragma once

#include <stdbool.h>

#include "esp_err.h"

// Bring up WiFi in station mode using the credentials from menuconfig and keep
// it connected (auto-reconnect on drop). Returns once we either have an IP or
// the initial attempt window expires — the station stays useful either way, it
// just answers requests with BBX_ST_NO_WIFI until the link comes back.
esp_err_t wifi_sta_start(void);

bool wifi_sta_connected(void);

// Current AP RSSI in dBm (negative), or 0 when not connected.
int wifi_sta_rssi(void);
