#include "wifi_sta.h"

#include "buttons.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

static const char *TAG = "wifi";

#define GOT_IP_BIT BIT0

static EventGroupHandle_t s_events;
static volatile bool      s_connected;
// Held while the boot scan runs so the STA_START auto-connect doesn't claim
// the radio first (a scan during a connection attempt is refused).
static volatile bool      s_hold_connect;

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (!s_hold_connect) esp_wifi_connect();
        return;
    }
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_connected = false;
        xEventGroupClearBits(s_events, GOT_IP_BIT);
        // Retry forever. The relay sits unattended at an event; the AP going
        // away for a minute shouldn't need a power cycle.
        ESP_LOGW(TAG, "disconnected, retrying");
        vTaskDelay(pdMS_TO_TICKS(2000));
        esp_wifi_connect();
        return;
    }
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "got ip " IPSTR, IP2STR(&ev->ip_info.ip));
        s_connected = true;
        xEventGroupSetBits(s_events, GOT_IP_BIT);
        return;
    }
}

bool wifi_sta_connected(void)
{
    return s_connected;
}

int wifi_sta_rssi(void)
{
    wifi_ap_record_t info;
    if (!s_connected) return 0;
    if (esp_wifi_sta_get_ap_info(&info) != ESP_OK) return 0;
    return (int)info.rssi;
}

static const char *auth_name(wifi_auth_mode_t m)
{
    switch (m) {
        case WIFI_AUTH_OPEN:         return "open";
        case WIFI_AUTH_WEP:          return "wep";
        case WIFI_AUTH_WPA_PSK:      return "wpa";
        case WIFI_AUTH_WPA2_PSK:     return "wpa2";
        case WIFI_AUTH_WPA_WPA2_PSK: return "wpa/wpa2";
        case WIFI_AUTH_WPA3_PSK:     return "wpa3";
        case WIFI_AUTH_WPA2_WPA3_PSK: return "wpa2/wpa3";
        case WIFI_AUTH_WAPI_PSK:     return "wapi";
        default:                     return "?";
    }
}

// One active scan over all channels, logging every AP in the air. Runs at
// boot so the console shows whether the configured SSID is even visible.
static void log_visible_aps(void)
{
    wifi_scan_config_t sc = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = true,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time = { .active = { .min = 100, .max = 300 } },
    };
    if (esp_wifi_scan_start(&sc, true) != ESP_OK) {
        ESP_LOGW(TAG, "scan failed");
        return;
    }
    uint16_t n = 0;
    if (esp_wifi_scan_get_ap_num(&n) != ESP_OK || n == 0) {
        ESP_LOGI(TAG, "scan: no APs visible");
        return;
    }
    wifi_ap_record_t *aps = malloc(n * sizeof(*aps));
    if (!aps) return;
    uint16_t got = n;
    if (esp_wifi_scan_get_ap_records(&got, aps) == ESP_OK) {
        ESP_LOGI(TAG, "scan: %u APs visible", got);
        for (uint16_t i = 0; i < got; i++) {
            const char *ssid = (const char *)aps[i].ssid;
            if (ssid[0] == '\0') ssid = "(hidden)";
            ESP_LOGI(TAG, "  \"%.32s\" ch=%d rssi=%d %s", ssid,
                     aps[i].primary, aps[i].rssi, auth_name(aps[i].authmode));
        }
    }
    free(aps);
}

esp_err_t wifi_sta_start(void)
{
    s_events = xEventGroupCreate();
    if (!s_events) return ESP_ERR_NO_MEM;

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, on_wifi_event, NULL, NULL));

    wifi_config_t wc = {0};
    strlcpy((char *)wc.sta.ssid, CONFIG_RELAY_WIFI_SSID, sizeof(wc.sta.ssid));
    strlcpy((char *)wc.sta.password, CONFIG_RELAY_WIFI_PASSWORD, sizeof(wc.sta.password));
    if (wc.sta.password[0] == '\0') {
        // Open network: no key, accept the AP as-is.
        wc.sta.threshold.authmode = WIFI_AUTH_OPEN;
    } else {
        wc.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    // STA MAC precedence: runtime NVS override from the Down-button MAC
    // editor (compiled out while venue debugging), then the compile-time
    // override, then the chip efuse MAC. A UI-set MAC only takes effect
    // after a reboot (set_mac pre-dates start).
    bool have_rt = false;
    uint8_t rtmac[6];
#if CONFIG_RELAY_MAC_EDIT_ENABLE
    have_rt = box_mac_override(rtmac);
#endif
    if (have_rt) {
        if (esp_wifi_set_mac(WIFI_IF_STA, rtmac) == ESP_OK) {
            ESP_LOGI(TAG, "NVS STA MAC %02x:%02x:%02x:%02x:%02x:%02x",
                     rtmac[0], rtmac[1], rtmac[2],
                     rtmac[3], rtmac[4], rtmac[5]);
        } else {
            ESP_LOGW(TAG, "NVS esp_wifi_set_mac failed");
        }
    } else if (CONFIG_RELAY_WIFI_STA_MAC[0] != '\0') {
        uint8_t mac[6];
        unsigned tmp[6];
        if (sscanf(CONFIG_RELAY_WIFI_STA_MAC, "%02x:%02x:%02x:%02x:%02x:%02x",
                   &tmp[0], &tmp[1], &tmp[2], &tmp[3], &tmp[4],
                   &tmp[5]) == 6) {
            for (int i = 0; i < 6; i++) mac[i] = (uint8_t)tmp[i];
            // Reject multicastbit set (LSB of first octet).
            if ((mac[0] & 0x01) == 0) {
                esp_err_t merr = esp_wifi_set_mac(WIFI_IF_STA, mac);
                if (merr == ESP_OK) {
                    ESP_LOGI(TAG, "custom STA MAC %02x:%02x:%02x:%02x:%02x:%02x",
                             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
                } else {
                    ESP_LOGW(TAG, "esp_wifi_set_mac failed: %s",
                             esp_err_to_name(merr));
                }
            } else {
                ESP_LOGW(TAG, "ignoring multicast STA MAC \"%s\"",
                         CONFIG_RELAY_WIFI_STA_MAC);
            }
        } else {
            ESP_LOGW(TAG, "ignoring malformed STA MAC \"%s\", want xx:xx:xx:xx:xx:xx",
                     CONFIG_RELAY_WIFI_STA_MAC);
        }
    }
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));

    // Keep the radio hot. Modem sleep would add latency to every POST, and we
    // are not on a battery.
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    s_hold_connect = true;
    ESP_ERROR_CHECK(esp_wifi_start());

    log_visible_aps();
    s_hold_connect = false;
    ESP_ERROR_CHECK(esp_wifi_connect());

    ESP_LOGI(TAG, "connecting to \"%s\"", CONFIG_RELAY_WIFI_SSID);
    EventBits_t bits = xEventGroupWaitBits(s_events, GOT_IP_BIT, pdFALSE, pdTRUE,
                                           pdMS_TO_TICKS(20000));
    if (!(bits & GOT_IP_BIT)) {
        ESP_LOGW(TAG, "no IP yet; continuing, will keep retrying in background");
    }
    return ESP_OK;
}
