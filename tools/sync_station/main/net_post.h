#pragma once

#include <stddef.h>
#include <stdint.h>

#include "bbx_proto.h"

typedef struct {
    uint8_t status;      // BBX_ST_*
    size_t  profile_len; // successful profile response bytes
    int     http_code;   // log and status screen only; the badge sees `status`
    // Short detail for BBX_ST_TRANSPORT failures (socket errno or TLS error),
    // for the serial log and the status screen. "" when there was no
    // transport failure. Never carries credentials — only error codes.
    char    transport_err[24];
} net_post_status_t;

// POST a badge snapshot to `url` as application/json.
//
// Deliberately different from the relay's net_post_json() in three ways, all
// of them consequences of the body being ~20 KB rather than ~200 bytes:
//
//   * The write is a loop. esp_http_client_write() returns how many bytes it
//     actually took, and over TLS a 20 KB write short-writes routinely. The
//     relay's single-shot `written != body_len` check is correct only for
//     bodies that fit one record and must not be copied here.
//   * Legacy response bodies are drained. Version 2 retains the bounded profile
//     from the same POST response for return to the badge; no second HTTP
//     request is made.
//   * A longer timeout. 20 KB over a venue AP, with WiFi and BLE sharing one
//     radio, exceeds the relay's 8 s often enough to matter.
//
// Never fails outright — every outcome is expressed through out->status so the
// badge always gets an answer.
//
// Blocking, and can take many seconds. Call it from a worker task, never from
// the NimBLE host callback.
void net_post_upload(const char *url, const char *body, size_t body_len,
                     const char *badge_hex, net_post_status_t *out);

// Version 2: POST the snapshot once and return the bounded profile in its response.
void net_sync_upload(const char *url, const char *body, size_t body_len,
                     const char *badge_hex, char *profile, size_t capacity,
                     net_post_status_t *out);
