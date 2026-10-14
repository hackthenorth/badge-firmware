// Supabase heartbeat: proves this station has power, WiFi, and an uplink.
// Every HEARTBEAT_INTERVAL_MS we POST {"p_device_id": "<station id>"} to the
// touch_sync_heartbeat() RPC. The server stamps the row with its own clock,
// so the station needs no time sync. Failures just log; the BLE sync path is
// unaffected.

#include "heartbeat.h"

#include "buttons.h"

#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "wifi_sta.h"

static const char *TAG = "heartbeat";

#define HEARTBEAT_INTERVAL_MS (3 * 60 * 1000)
#define HEARTBEAT_STARTUP_DELAY_MS (10 * 1000)
#define HEARTBEAT_TIMEOUT_MS (10 * 1000)
#define HEARTBEAT_RPC_PATH "/rest/v1/rpc/touch_sync_heartbeat"

// Minimal JSON string escaping for the station id: quotes, backslash,
// controls.
static size_t json_escape(const char *in, char *out, size_t out_sz)
{
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)in;
         *p != '\0' && o + 1 < out_sz; p++) {
        if (*p == '"' || *p == '\\') {
            if (o + 2 >= out_sz) break;
            out[o++] = '\\';
            out[o++] = (char)*p;
        } else if (*p < 0x20) {
            if (o + 6 >= out_sz) break;
            o += (size_t)snprintf(out + o, out_sz - o, "\\u%04x", *p);
        } else {
            out[o++] = (char)*p;
        }
    }
    out[o] = '\0';
    return o;
}

static void send_once(void)
{
    if (!wifi_sta_connected()) {
        ESP_LOGI(TAG, "no wifi, skipping heartbeat");
        return;
    }

    // Copy, don't snprintf: the configured URL is unbounded, and a fixed
    // buffer plus %s trips -Werror=format-truncation.
    const char *base = CONFIG_SYNC_SUPABASE_URL;
    const char *rpc = HEARTBEAT_RPC_PATH;
    size_t base_len = strlen(base);
    size_t rpc_len = strlen(rpc);
    char url[256];
    if (base_len == 0 || base_len + rpc_len + 1 > sizeof(url)) {
        ESP_LOGW(TAG, "bad SUPABASE_URL, skipping heartbeat");
        return;
    }
    memcpy(url, base, base_len);
    memcpy(url + base_len, rpc, rpc_len + 1);

    // Station id is at most 39 chars; worst case (all escaped) fits with room.
    // Uses the runtime selection, so heartbeats track button changes.
    char id_esc[256];
    char body[320];
    json_escape(box_id_current(), id_esc, sizeof(id_esc));
    snprintf(body, sizeof(body), "{\"p_device_id\":\"%s\"}", id_esc);

    char auth[512];
    snprintf(auth, sizeof(auth), "Bearer %s", CONFIG_SYNC_SUPABASE_ANON_KEY);

    esp_http_client_config_t cfg = {
        .url               = url,
        .method            = HTTP_METHOD_POST,
        .timeout_ms        = HEARTBEAT_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        ESP_LOGW(TAG, "client init failed");
        return;
    }
    esp_http_client_set_header(client, "apikey", CONFIG_SYNC_SUPABASE_ANON_KEY);
    esp_http_client_set_header(client, "Authorization", auth);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, body, strlen(body));

    esp_err_t err = esp_http_client_perform(client);
    int code = (err == ESP_OK) ? esp_http_client_get_status_code(client) : -1;
    if (err == ESP_OK && code >= 200 && code < 300) {
        ESP_LOGI(TAG, "heartbeat ok (%s)", box_id_current());
    } else {
        ESP_LOGW(TAG, "heartbeat failed: %s http=%d",
                 esp_err_to_name(err), code);
    }
    esp_http_client_cleanup(client);
}

static void heartbeat_task(void *arg)
{
    (void)arg;
    if (CONFIG_SYNC_SUPABASE_URL[0] == '\0' ||
        CONFIG_SYNC_SUPABASE_ANON_KEY[0] == '\0') {
        ESP_LOGW(TAG, "not configured (set SUPABASE_URL/ANON_KEY), disabled");
        vTaskDelete(NULL);
        return;
    }
    // Let DHCP settle before the first beat.
    vTaskDelay(pdMS_TO_TICKS(HEARTBEAT_STARTUP_DELAY_MS));
    for (;;) {
        send_once();
        vTaskDelay(pdMS_TO_TICKS(HEARTBEAT_INTERVAL_MS));
    }
}

void heartbeat_start(void)
{
    // 8 KB: the TLS handshake in send_once() is the deepest thing this does.
    xTaskCreate(heartbeat_task, "heartbeat", 8192, NULL, 3, NULL);
}
