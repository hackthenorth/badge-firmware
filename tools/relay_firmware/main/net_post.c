#include "net_post.h"

#include <stdio.h>
#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "wifi_sta.h"

static const char *TAG = "net_post";
static SemaphoreHandle_t s_http_lock;

esp_err_t net_post_init(void)
{
    s_http_lock = xSemaphoreCreateMutex();
    return s_http_lock ? ESP_OK : ESP_ERR_NO_MEM;
}

bool net_post_try_lock(void)
{
    return xSemaphoreTake(s_http_lock, 0) == pdTRUE;
}

void net_post_unlock(void)
{
    xSemaphoreGive(s_http_lock);
}

static void set_error(net_post_result_t *out, uint8_t status, const char *msg)
{
    out->status   = status;
    out->flags    = 0;
    out->body_len = (size_t)snprintf(out->body, sizeof(out->body), "%s", msg);
}

// Transport detail, captured before cleanup: socket errno tells DNS apart
// from TCP timeouts/resets, tls_err from TLS alerts. Logged with heap + RSSI
// so a failure reads as one line: network cause, memory state, signal.
static void log_net_detail(esp_http_client_handle_t client, const char *ctx)
{
    int sock_errno = esp_http_client_get_errno(client);
    int tls_err = 0, tls_flags = 0;
    esp_http_client_get_and_clear_last_tls_error(client, &tls_err, &tls_flags);
    ESP_LOGW(TAG, "%s: errno %d tls_err 0x%x tls_flags 0x%x rssi %d heap free=%u largest=%u",
             ctx, sock_errno, (unsigned)tls_err, (unsigned)tls_flags,
             wifi_sta_rssi(),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
}

// One POST attempt. Caller holds s_http_lock.
static void do_post(const char *url, const char *body, size_t body_len,
                    net_post_result_t *out)
{
    esp_http_client_config_t cfg = {
        .url               = url,
        .method            = HTTP_METHOD_POST,
        .timeout_ms        = 8000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        set_error(out, BBX_ST_TRANSPORT, "client init failed");
        goto done;
    }

    // Declared up front so the error paths below can goto past them.
    int64_t content_len = 0;
    int     code = 0;
    int     n = 0;

    esp_http_client_set_header(client, "Content-Type", "application/json");

    // Streamed rather than using set_post_field + perform, so the reply can be
    // capped at BBX_MAX_BODY without ever buffering a large response.
    ESP_LOGI(TAG, "POST heap: free=%u largest=%u rssi %d",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             wifi_sta_rssi());
    esp_err_t err = esp_http_client_open(client, (int)body_len);
    if (err != ESP_OK) {
        log_net_detail(client, "open");
        set_error(out, BBX_ST_TRANSPORT, esp_err_to_name(err));
        goto done;
    }

    if (body_len > 0) {
        int written = esp_http_client_write(client, body, (int)body_len);
        if (written != (int)body_len) {
            log_net_detail(client, "write");
            ESP_LOGW(TAG, "short write: %d of %u", written, (unsigned)body_len);
            set_error(out, BBX_ST_TRANSPORT, "write failed");
            goto done;
        }
    }

    content_len = esp_http_client_fetch_headers(client);
    if (content_len < 0) {
        log_net_detail(client, "headers");
        set_error(out, BBX_ST_TRANSPORT, "no response");
        goto done;
    }

    code = esp_http_client_get_status_code(client);

    n = esp_http_client_read_response(client, out->body, BBX_MAX_BODY);
    if (n < 0) n = 0;
    out->body[n] = '\0';
    out->body_len = (size_t)n;

    // If bytes are still on the wire we hit the cap. This covers chunked
    // replies too, where fetch_headers reports a length of 0. Flag it so the
    // badge can say "reply was too long" rather than showing nothing.
    if (!esp_http_client_is_complete_data_received(client)) {
        out->flags |= BBX_FLAG_TRUNC;
    }

    if (code < 200 || code >= 300) {
        ESP_LOGW(TAG, "http %d", code);
        // Overwrite the body with the status — an error page is rarely useful
        // and rarely small.
        out->status   = BBX_ST_HTTP;
        out->flags    = 0;
        out->body_len = (size_t)snprintf(out->body, sizeof(out->body), "HTTP %d", code);
        goto done;
    }

    out->status = BBX_ST_OK;
    ESP_LOGI(TAG, "http %d, %u bytes%s", code, (unsigned)out->body_len,
             (out->flags & BBX_FLAG_TRUNC) ? " (truncated)" : "");

done:
    if (client) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
    }
}

void net_post_json(const char *url, const char *body, size_t body_len,
                   net_post_result_t *out)
{
    memset(out, 0, sizeof(*out));

    if (!wifi_sta_connected()) {
        set_error(out, BBX_ST_NO_WIFI, "no wifi");
        return;
    }

    xSemaphoreTake(s_http_lock, portMAX_DELAY);

    // One retry on transport failure only: HTTP answers (even errors) are
    // final, and the badge's ~12 s repeat window usually still covers a
    // second attempt.
    for (int attempt = 1; attempt <= 2; attempt++) {
        do_post(url, body, body_len, out);
        if (out->status != BBX_ST_TRANSPORT) break;
        if (attempt == 1) {
            ESP_LOGW(TAG, "transport failed, retrying once");
            vTaskDelay(pdMS_TO_TICKS(500));
        }
    }

    net_post_unlock();
}
