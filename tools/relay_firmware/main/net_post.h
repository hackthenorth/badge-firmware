#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "bbx_proto.h"
#include "esp_err.h"

// Initialize before starting network tasks. The C3 has heap for one TLS
// session; net_post_json holds this lock for its entire request. Background
// heartbeats use try_lock and defer while a badge request is active.
esp_err_t net_post_init(void);
bool net_post_try_lock(void);
void net_post_unlock(void);

typedef struct {
    uint8_t status;                   // BBX_ST_*
    uint8_t flags;                    // BBX_FLAG_TRUNC if the reply was cut
    size_t  body_len;
    char    body[BBX_MAX_BODY + 1];   // response bytes, or a short error string
} net_post_result_t;

// POST `body` (raw bytes, Content-Type: application/json) to `url` and capture
// at most BBX_MAX_BODY bytes of the reply. Never fails outright — every outcome
// is expressed through `out->status` so the badge always gets an answer.
//
// Blocking, and can take seconds. Call it from a worker task, never from the
// NimBLE host callback.
void net_post_json(const char *url, const char *body, size_t body_len,
                   net_post_result_t *out);
