#pragma once

// BBX1 bulk-transfer receiver: reassembles a badge's snapshot from ~90
// advertising frames, then hands the bytes to the caller to upload.
//
// One session at a time. Both sides have MAX_EXT_ADV_INSTANCES=1, so there is
// exactly one advertising register per side and no way to interleave two
// transfers. The GRANT is a lease over that register.
//
// This module owns the advertisement while a session is live — it broadcasts
// GRANT, STAT, PROFILE_DATA and DONE itself. When it goes idle the caller should put the
// beacon back up.
//
// Threading, matching the relay's split:
//   NimBLE host task   xfer_rx_on_frame()    validate, copy, never block
//   worker task        xfer_rx_take_upload() / xfer_rx_finish_upload()
//   ticker task        xfer_rx_tick()        timeouts and expiry

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "bbx_proto.h"

void xfer_rx_init(void);

// Feed one BBX1 frame heard on the manufacturer-data channel. Magic is
// re-checked here, so the caller may pass anything that is at least a header.
void xfer_rx_on_frame(const uint8_t *data, size_t len);

// Age out a stalled session and retire a DONE that was never ACKed. Call every
// ~200 ms.
void xfer_rx_tick(void);

// True while a session holds the lease. The caller uses this to decide whether
// it may broadcast its idle beacon.
bool xfer_rx_busy(void);

// Point-in-time view of the session for the status screen. Cheap (one lock),
// called from the ticker — never from the frame path.
typedef struct {
    bool     idle;          // no session holds the lease
    uint8_t  sess;
    uint8_t  recv_count;
    uint8_t  total_frames;
    uint16_t total_len;     // bytes; valid unless idle
    bool     uploading;     // ST_READY or ST_POSTING: bytes landed, HTTP in flight
    bool     downlink;      // ST_DOWNLINK: sending the profile back to the badge
    uint8_t  down_seq;      // profile frames acked so far; valid when downlink
    uint8_t  down_frames;   // profile frames total; valid when downlink
    bool     done;          // ST_DONE: XFER_DONE on the air, waiting for ACK
    uint8_t  done_status;   // BBX_ST_*, valid when done
    int      http_code;     // endpoint's HTTP status for the last upload (0 when unknown)
    char     transport_err[24]; // short network-error detail for BBX_ST_TRANSPORT
} xfer_snapshot_t;

void xfer_rx_snapshot(xfer_snapshot_t *out);

// Claim a completed payload for upload. Returns false if nothing is ready.
//
// On true the buffer is stable until finish_upload() or finish_profile(): the
// session is parked in a posting state where no frame can free or overwrite it.
bool xfer_rx_take_upload(char **body_out, size_t *len_out,
                         uint8_t badge_out[6], uint8_t *sess_out, uint8_t *downlink_out);

// Record the endpoint's HTTP status for the in-flight upload, so the status
// screen can show *which* failure happened (401 vs 429 vs 503). Call after
// xfer_rx_take_upload() and before the finish call; 0 clears it. Never carries
// credentials — only the numeric code.
void xfer_rx_note_http(int http_code);

// Record the underlying network error for the in-flight upload (socket errno
// or TLS error, as produced by net_post), so the status screen can show *why*
// a transport failure happened instead of a bare "net err". Call alongside
// xfer_rx_note_http(); NULL or "" clears it. Error codes only, never bodies,
// tokens, or profiles.
void xfer_rx_note_transport(const char *detail);

// Report the upload's outcome. Broadcasts XFER_DONE carrying `status` and
// starts the ACK wait; the session is released by xfer_rx_tick() once the badge
// ACKs or the hold expires.
void xfer_rx_finish_upload(uint8_t status);

// Copy a <=2 KiB profile into a CRC-protected, acknowledged reverse transfer.
void xfer_rx_finish_profile(const char *profile, size_t len, uint8_t status);
