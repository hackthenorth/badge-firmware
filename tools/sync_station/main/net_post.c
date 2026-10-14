#include "net_post.h"
#include "cJSON.h"
#include <stdlib.h>
#include <ctype.h>

#include <stdio.h>
#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "wifi_sta.h"

static const char *TAG = "net_post";

// Stream the response in bounded chunks, retaining only a requested profile.
#define DRAIN_CHUNK 256

// Record a BBX_ST_TRANSPORT failure: log the underlying network error (socket
// errno plus any pending mbedTLS/TLS error) for the serial console and keep a
// short verdict-sized string in out->transport_err for the status screen and
// the one-line request summary. `err` is an esp_err_t from the call that
// failed, or ESP_OK when the call reports no esp_err (write/read return int).
static void note_transport_err(esp_http_client_handle_t client,
                               net_post_status_t *out, const char *ctx, esp_err_t err)
{
    int sock_errno = (client != NULL) ? esp_http_client_get_errno(client) : -1;
    int tls_err = 0, tls_flags = 0;
    if (client != NULL) {
        esp_http_client_get_and_clear_last_tls_error(client, &tls_err, &tls_flags);
    }
    if (tls_err != 0) {
        snprintf(out->transport_err, sizeof(out->transport_err),
                 "tls 0x%x", (unsigned)tls_err);
    } else if (sock_errno > 0) {
        snprintf(out->transport_err, sizeof(out->transport_err),
                 "errno %d", sock_errno);
    } else if (err != ESP_OK) {
        snprintf(out->transport_err, sizeof(out->transport_err),
                 "%.23s", esp_err_to_name(err));
    } else {
        snprintf(out->transport_err, sizeof(out->transport_err),
                 "%.23s", ctx);
    }
    ESP_LOGW(TAG, "%s failed: %s (errno %d, tls_err 0x%x, tls_flags 0x%x, rssi %d)",
             ctx, out->transport_err, sock_errno,
             (unsigned)tls_err, (unsigned)tls_flags, wifi_sta_rssi());
}

static void net_sync_request(const char *url, const char *body, size_t body_len,
                     const char *badge_hex, net_post_status_t *out, char *profile, size_t capacity)
{
    (void)badge_hex; // Radio addressing is local; the API uses claim ID + token.
    memset(out, 0, sizeof(*out));

    // HTNSYNC1 metadata becomes JSON request parameters, outside badge_data.
    // Reject legacy/unprovisioned badges before sending any request.
    char claim_id[24] = {0};
    char token[65] = {0};
    size_t pos = 9, claim_len = 0;
    out->status = BBX_ST_HTTP;
    if (body_len < 9 || memcmp(body, "HTNSYNC1\n", 9) != 0) return;
    while (pos < body_len && body[pos] >= '0' && body[pos] <= '9') {
        if (claim_len == sizeof(claim_id) - 1) return;
        claim_id[claim_len++] = body[pos++];
    }
    if (!claim_len || claim_id[0] == '0' || pos >= body_len || body[pos++] != '\n') return;
    if (body_len - pos < 66) return;
    for (size_t i = 0; i < 64; ++i) {
        char c = body[pos++];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return;
        token[i] = c;
    }
    if (body[pos++] != '\n' || body[pos] != '{') return;
    body += pos;
    body_len -= pos;
    if (sizeof(CONFIG_SYNC_AUTH_TOKEN) == 1) return;

    if (!wifi_sta_connected()) {
        out->status = BBX_ST_NO_WIFI;
        ESP_LOGW(TAG, "no wifi, dropping %u-byte upload", (unsigned)body_len);
        return;
    }

    // Validate the exact snapshot before embedding it, so a malformed badge
    // payload cannot escape badge_data and replace request credentials/target.
    const char *end = NULL;
    cJSON *snapshot = cJSON_ParseWithLengthOpts(body, body_len, &end, false);
    bool valid_snapshot = cJSON_IsObject(snapshot);
    cJSON_Delete(snapshot);
    if (!valid_snapshot || !end) return;
    while (end < body + body_len && isspace((unsigned char)*end)) ++end;
    if (end != body + body_len) return;

    cJSON *request = cJSON_CreateObject();
    if (!request) {
        out->status = BBX_ST_TRANSPORT;
        snprintf(out->transport_err, sizeof(out->transport_err), "no mem");
        ESP_LOGW(TAG, "request build failed: no mem");
        return;
    }
    bool built = cJSON_AddStringToObject(request, "password", CONFIG_SYNC_AUTH_TOKEN) &&
        cJSON_AddStringToObject(request, "claim_id", claim_id) &&
        cJSON_AddStringToObject(request, "badge_secure_token", token);
    char *prefix = built ? cJSON_PrintUnformatted(request) : NULL;
    cJSON_Delete(request);
    memset(token, 0, sizeof(token));
    if (!prefix) {
        out->status = BBX_ST_TRANSPORT;
        snprintf(out->transport_err, sizeof(out->transport_err), "no mem");
        ESP_LOGW(TAG, "request build failed: no mem");
        return;
    }
    prefix[strlen(prefix) - 1] = 0; // Replace the closing brace with badge_data below.
    const char *parts[] = {prefix, ",\"badge_data\":", body, "}"};
    const size_t lengths[] = {strlen(prefix), strlen(parts[1]), body_len, 1};
    const size_t request_len = lengths[0] + lengths[1] + lengths[2] + lengths[3];

    esp_http_client_config_t cfg = {
        .url               = url,
        .method            = HTTP_METHOD_POST,
        .timeout_ms        = CONFIG_SYNC_HTTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        out->status = BBX_ST_TRANSPORT;
        snprintf(out->transport_err, sizeof(out->transport_err), "init");
        ESP_LOGW(TAG, "client init failed");
        free(prefix);
        return;
    }

    // Declared up front so the error paths can goto past them.
    int64_t t0 = esp_timer_get_time();
    size_t  off = 0;
    int64_t content_len = 0;
    int     code = 0;
    char    drain[DRAIN_CHUNK];
    char    err_body[257];
    size_t  err_len = 0;

    esp_http_client_set_header(client, "Content-Type", "application/json");
    // Stream metadata and the original snapshot without allocating a second
    // full copy. The only HTTP header the caller needs is Content-Type.
    esp_err_t err = esp_http_client_open(client, (int)request_len);
    if (err != ESP_OK) {
        out->status = BBX_ST_TRANSPORT;
        note_transport_err(client, out, "open", err);
        goto done;
    }

    // THE loop. A 20 KB body does not go out in one write over TLS — the
    // record layer, the socket send buffer and the peer's window all cap what
    // one call can take. Treating a short write as an error, as the relay
    // does, would fail nearly every upload.
    for (size_t part = 0; part < sizeof(parts) / sizeof(parts[0]); ++part) {
        off = 0;
        while (off < lengths[part]) {
            int w = esp_http_client_write(client, parts[part] + off, (int)(lengths[part] - off));
            if (w <= 0) {
                out->status = BBX_ST_TRANSPORT;
                note_transport_err(client, out, "write", ESP_OK);
                goto done;
            }
            off += (size_t)w;
        }
    }

    content_len = esp_http_client_fetch_headers(client);
    if (content_len < 0) {
        out->status = BBX_ST_TRANSPORT;
        note_transport_err(client, out, "headers", ESP_OK);
        goto done;
    }

    code = esp_http_client_get_status_code(client);
    out->http_code = code;

    // Drain legacy/error replies; retain the bounded version-2 response.
    // For non-2xx, keep the head of the error body: the endpoint explains
    // itself there ("unknown claim", "bad password") and the numeric code
    // alone cannot tell those apart. Server text only, never our request.
    for (;;) {
        int received = esp_http_client_read(client, drain, sizeof(drain));
        if (received < 0) {
            out->status = BBX_ST_TRANSPORT;
            note_transport_err(client, out, "read", ESP_OK);
            goto done;
        }
        if (received == 0) break;
        if (profile && code == 200) {
            if (out->profile_len + (size_t)received >= capacity) { out->status = BBX_ST_TOO_BIG; goto done; }
            memcpy(profile + out->profile_len, drain, received);
            out->profile_len += received;
        } else if (code != 200 && err_len < sizeof(err_body) - 1) {
            size_t take = (size_t)received;
            if (take > sizeof(err_body) - 1 - err_len) take = sizeof(err_body) - 1 - err_len;
            memcpy(err_body + err_len, drain, take);
            err_len += take;
        }
    }
    if (code != 200) {
        err_body[err_len] = '\0';
        ESP_LOGW(TAG, "http %d for %u bytes: %.256s", code, (unsigned)body_len, err_body);
        out->status = BBX_ST_HTTP;
        goto done;
    }
    if (profile) {
        if (!esp_http_client_is_complete_data_received(client) || !out->profile_len) {
            out->status = BBX_ST_TRANSPORT;
            note_transport_err(client, out, "incomplete", ESP_OK);
            goto done;
        }
        profile[out->profile_len] = 0;
        cJSON *root = cJSON_ParseWithLengthOpts(profile, out->profile_len + 1, NULL, true);
        const cJSON *result = cJSON_GetObjectItemCaseSensitive(root, "status");
        const cJSON *value = cJSON_GetObjectItemCaseSensitive(root, "profile");
        char *json = cJSON_IsString(result) && strcmp(result->valuestring, "success") == 0 && cJSON_IsObject(value)
            ? cJSON_PrintUnformatted(value) : NULL;
        cJSON_Delete(root);
        if (!json || strlen(json) > BBX_PROFILE_MAX_LEN) {
            free(json); out->status = BBX_ST_HTTP; goto done;
        }
        out->profile_len = strlen(json);
        memcpy(profile, json, out->profile_len + 1);
        free(json);
    }

    out->status = BBX_ST_OK;
    ESP_LOGI(TAG, "http %d, uploaded %u bytes in %lld ms", code,
             (unsigned)body_len, (long long)((esp_timer_get_time() - t0) / 1000));

done:
    memset(prefix, 0, lengths[0]);
    free(prefix);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
}

void net_post_upload(const char *url, const char *body, size_t body_len,
                     const char *badge_hex, net_post_status_t *out)
{
    net_sync_request(url, body, body_len, badge_hex, out, NULL, 0);
}

void net_sync_upload(const char *url, const char *body, size_t body_len,
                     const char *badge_hex, char *profile, size_t capacity,
                     net_post_status_t *out)
{
    if (!profile || capacity < 2) { memset(out, 0, sizeof(*out)); out->status = BBX_ST_TOO_BIG; return; }
    net_sync_request(url, body, body_len, badge_hex, out, profile, capacity);
}
