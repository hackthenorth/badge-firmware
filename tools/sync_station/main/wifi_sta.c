#include "wifi_sta.h"

#include "buttons.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
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
        // Retry forever. A station sits unattended at an event; the AP going
        // away for a minute shouldn't need a power cycle.
        wifi_event_sta_disconnected_t *d = (wifi_event_sta_disconnected_t *)data;
        if (d) {
            char ssid[33];
            size_t n = d->ssid_len < sizeof(ssid) - 1 ? d->ssid_len : sizeof(ssid) - 1;
            memcpy(ssid, d->ssid, n);
            ssid[n] = '\0';
            ESP_LOGW(TAG, "disconnected ssid=\"%s\" bssid=%02x:%02x:%02x:%02x:%02x:%02x "
                          "reason=%d rssi=%d, retrying",
                     ssid, d->bssid[0], d->bssid[1], d->bssid[2],
                     d->bssid[3], d->bssid[4], d->bssid[5], d->reason, d->rssi);
        } else {
            ESP_LOGW(TAG, "disconnected (no event data), retrying");
        }
        vTaskDelay(pdMS_TO_TICKS(2000));
        esp_err_t cerr = esp_wifi_connect();
        if (cerr != ESP_OK) {
            ESP_LOGW(TAG, "esp_wifi_connect failed: %s", esp_err_to_name(cerr));
        }
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

static const char *cipher_name(wifi_cipher_type_t c)
{
    switch (c) {
        case WIFI_CIPHER_TYPE_NONE:            return "none";
        case WIFI_CIPHER_TYPE_WEP40:           return "wep40";
        case WIFI_CIPHER_TYPE_WEP104:          return "wep104";
        case WIFI_CIPHER_TYPE_TKIP:            return "tkip";
        case WIFI_CIPHER_TYPE_CCMP:            return "ccmp";
        case WIFI_CIPHER_TYPE_TKIP_CCMP:       return "tkip+ccmp";
        case WIFI_CIPHER_TYPE_AES_CMAC128:     return "aes-cmac128";
        case WIFI_CIPHER_TYPE_SMS4:            return "sms4";
        case WIFI_CIPHER_TYPE_GCMP:            return "gcmp";
        case WIFI_CIPHER_TYPE_GCMP256:         return "gcmp256";
        default:                               return "?";
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
            ESP_LOGI(TAG, "  \"%.32s\" " MACSTR " ch=%d,%d rssi=%d %s pair=%s group=%s%s%s%s",
                     ssid, MAC2STR(aps[i].bssid),
                     aps[i].primary, aps[i].second, aps[i].rssi, auth_name(aps[i].authmode),
                     cipher_name(aps[i].pairwise_cipher), cipher_name(aps[i].group_cipher),
                     aps[i].phy_11b ? " b" : "", aps[i].phy_11g ? " g" : "",
                     aps[i].phy_11n ? " n" : "", aps[i].wps ? " wps" : "");
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
    strlcpy((char *)wc.sta.ssid, CONFIG_SYNC_WIFI_SSID, sizeof(wc.sta.ssid));
    strlcpy((char *)wc.sta.password, CONFIG_SYNC_WIFI_PASSWORD, sizeof(wc.sta.password));
    if (wc.sta.password[0] == '\0') {
        // Open network: no key, accept the AP as-is.
        wc.sta.threshold.authmode = WIFI_AUTH_OPEN;
    } else {
        wc.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    // The ID-derived override uses the event prefix 24:0A:C4:C8:46 with
    // sync-01 -> B2 and sync-02 -> B3 (compiled out while venue debugging).
    // It takes precedence over the compile-time/chip defaults. Switching IDs
    // needs a reboot to apply.
    bool have_rt = false;
    uint8_t rtmac[6];
#if CONFIG_SYNC_MAC_DERIVE_ENABLE
    have_rt = box_mac_override(rtmac);
#endif
    if (have_rt) {
        if (esp_wifi_set_mac(WIFI_IF_STA, rtmac) == ESP_OK) {
            ESP_LOGI(TAG, "ID STA MAC %02x:%02x:%02x:%02x:%02x:%02x",
                     rtmac[0], rtmac[1], rtmac[2],
                     rtmac[3], rtmac[4], rtmac[5]);
        } else {
            ESP_LOGW(TAG, "ID esp_wifi_set_mac failed");
        }
    } else if (CONFIG_SYNC_WIFI_STA_MAC[0] != '\0') {
        uint8_t mac[6];
        unsigned tmp[6];
        if (sscanf(CONFIG_SYNC_WIFI_STA_MAC, "%02x:%02x:%02x:%02x:%02x:%02x",
                   &tmp[0], &tmp[1], &tmp[2], &tmp[3], &tmp[4],
                   &tmp[5]) == 6) {
            for (int i = 0; i < 6; i++) mac[i] = (uint8_t)tmp[i];
            // Reject multicast bit set (LSB of first octet).
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
                         CONFIG_SYNC_WIFI_STA_MAC);
            }
        } else {
            ESP_LOGW(TAG, "ignoring malformed STA MAC \"%s\", want xx:xx:xx:xx:xx:xx",
                     CONFIG_SYNC_WIFI_STA_MAC);
        }
    }
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));

    // Full effective-config dump (password length only, never the value) so
    // a join failure can be diagnosed from the serial log alone.
    {
        uint8_t mac[6];
        wifi_country_t country;
        uint8_t proto = 0;
        wifi_bandwidth_t bw;
        int8_t max_pwr = 0;
        esp_wifi_get_mac(WIFI_IF_STA, mac);
        esp_wifi_get_country(&country);
        esp_wifi_get_protocol(WIFI_IF_STA, &proto);
        esp_wifi_get_bandwidth(WIFI_IF_STA, &bw);
        esp_wifi_get_max_tx_power(&max_pwr);
        ESP_LOGI(TAG, "cfg ssid=\"%s\" pwlen=%u thresh=%s bssid_set=%d ch=%d pmf=%d,%d",
                 (const char *)wc.sta.ssid, (unsigned)strlen((const char *)wc.sta.password),
                 auth_name(wc.sta.threshold.authmode), wc.sta.bssid_set,
                 wc.sta.channel, wc.sta.pmf_cfg.capable, wc.sta.pmf_cfg.required);
        ESP_LOGI(TAG, "cfg mac=" MACSTR " country=%.2s proto=0x%x bw=%d maxpwr=%d listen=%d",
                 MAC2STR(mac), country.cc, proto, bw, max_pwr, wc.sta.listen_interval);
    }

    // Keep the radio hot. Modem sleep would add latency to every POST, and we
    // are not on a battery.
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    s_hold_connect = true;
    ESP_ERROR_CHECK(esp_wifi_start());

    log_visible_aps();
    s_hold_connect = false;
    ESP_ERROR_CHECK(esp_wifi_connect());

    ESP_LOGI(TAG, "connecting to \"%s\"", CONFIG_SYNC_WIFI_SSID);
    EventBits_t bits = xEventGroupWaitBits(s_events, GOT_IP_BIT, pdFALSE, pdTRUE,
                                           pdMS_TO_TICKS(20000));
    if (!(bits & GOT_IP_BIT)) {
        ESP_LOGW(TAG, "no IP yet; continuing, will keep retrying in background");
    }
    return ESP_OK;
}
