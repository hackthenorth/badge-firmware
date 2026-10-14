#pragma once

// Solana Scavenger Hunt wire protocol ("BBX1").
//
// Rides inside the existing hal_radio manufacturer-data channel (company
// 0xFFFF), alongside Badge Bump's "BMP2" frames — the 4-byte magic below is
// what tells them apart. Every listener on this channel sees every frame, so
// always check the magic before touching anything else.
//
// Flow:
//   badge --REQ--> relay --HTTP POST--> server
//   badge <-RESP-- relay <--HTTP 200--- server
//   badge --ACK--> relay          (frees the relay's response slot early)
//
// !! KEEP BYTE-IDENTICAL WITH tools/relay_firmware/main/bbx_proto.h !!
// The relay is a separate ESP-IDF project and cannot include from main/, so
// this header is duplicated. Editing one copy and not the other fails silently
// as "the badge never gets a response".

#include <stdint.h>

#define BBX_MAGIC_0 'B'
#define BBX_MAGIC_1 'B'
#define BBX_MAGIC_2 'X'
#define BBX_MAGIC_3 '1'

// Frame size is dictated by the radio, not chosen. Two HCI ceilings apply and
// the smaller one wins — getting this wrong twice is what makes it worth
// spelling out.
//
//   TX, 251. While advertising, the whole AD blob must fit one HCI LE Set
//   Extended Advertising Data command (BT 5.0 Vol 2, Part E, 7.8.54), enforced
//   by ble_gap_ext_adv_set_data_validate(). Over this, ble_gap_ext_adv_set_data
//   returns BLE_HS_EINVAL and nothing is transmitted at all.
//
//   RX, 229. A scanner hands advertising data to its host in HCI LE Extended
//   Advertising Report events, whose Parameter_Total_Length is one byte; the
//   per-report fixed fields eat 24 and the subevent header 2, leaving
//   255 - 26 = 229. Over this the controller splits the data across reports
//   with data_status INCOMPLETE — and NimBLE does not reassemble. It is
//   transmitted, and silently never arrives.
//
// RX binds, and the wrapper costs 4 bytes for
// [length][type=0xFF][company_lo][company_hi]. So 225.
//
// Both failure modes look exactly like weak signal: the frames that disappear
// are the big ones, which is also what path loss does. This was 250 (over TX),
// then 247 (under TX, over RX). Neither moved a full DATA frame. Do not raise
// it without both pausing advertising AND reassembling fragments on receive.
#define BBX_AD_OVERHEAD       4
#define BBX_HCI_MAX_ADV_DATA  251   // TX: one Set Adv Data command
#define BBX_HCI_MAX_ADV_RPT   229   // RX: one Extended Advertising Report
#define BBX_MAX_FRAME (BBX_HCI_MAX_ADV_RPT - BBX_AD_OVERHEAD)   // 225
#define BBX_HDR_SIZE  18
#define BBX_MAX_BODY  (BBX_MAX_FRAME - BBX_HDR_SIZE)   // 207

// Longest box id, matching the badge's box_id_ buffer (kBoxIdCap - 1). Applies
// to the BEACON body and to the id parsed out of a "box:<id>" NFC tag.
#define BBX_MAX_BOX_ID 39

// Frame types.
#define BBX_TYPE_REQ    0x01   // badge -> relay, body = request JSON
#define BBX_TYPE_RESP   0x02   // relay -> badge, body = response JSON (or error text)
#define BBX_TYPE_ACK    0x03   // badge -> relay, no body
// BEACON body is the relay's box id: ASCII, NOT nul-terminated, length implied
// by the frame length, at most BBX_MAX_BOX_ID bytes. May be empty on an
// unconfigured relay, which proximity mode must treat as "not a usable box".
#define BBX_TYPE_BEACON 0x04   // relay -> anyone, idle "I'm alive, I'm <box id>"

// Bulk transfer (0x10..0x1F). See "Transfer extension" at the bottom.
#define BBX_TYPE_XFER_OPEN   0x10   // badge -> station, body = bbx_xfer_open_t
#define BBX_TYPE_XFER_GRANT  0x11   // station -> badge, body = bbx_xfer_grant_t
#define BBX_TYPE_DATA        0x12   // badge -> station, body = one chunk
#define BBX_TYPE_XFER_STAT   0x13   // station -> badge, body = bbx_xfer_stat_t
#define BBX_TYPE_XFER_DONE   0x14   // station -> badge, terminal status
// A sync station deliberately does NOT use BBX_TYPE_BEACON. app_blindbox
// accepts any BEACON as a Solana Scavenger Hunt id, so a shared type would make a badge in
// proximity mode fire a REQ at a sync desk that nobody answers.
#define BBX_TYPE_PROFILE_DATA 0x16   // station -> badge, bounded profile chunk
#define BBX_TYPE_PROFILE_ACK  0x17   // badge -> station, echoes accepted seq
#define BBX_PROFILE_MAX_LEN 2048
#define BBX_PROFILE_PREFIX_LEN 6    // uint16 byte length + uint32 CRC32, little endian
#define BBX_TYPE_SYNC_BEACON 0x15   // station -> anyone, body = station id

// Reserved, not implemented.
#define BBX_TYPE_CEREMONY    0x20   // see docs/BADGE-EVENT-EXPERIENCE.md §12
#define BBX_TYPE_IDENT_REQ   0x30   // field restore of an unidentified badge

// flags bits.
#define BBX_FLAG_TRUNC      (1 << 0)   // RESP body was cut to BBX_MAX_BODY
#define BBX_FLAG_SWEEP_DONE (1 << 1)   // XFER_OPEN: sweep over, send me a STAT
#define BBX_FLAG_RESUME     (1 << 2)   // XFER_OPEN: re-open of a live session

// status codes (RESP and XFER_GRANT / XFER_DONE).
#define BBX_ST_OK        0   // HTTP 2xx, body is the server's response
#define BBX_ST_NO_WIFI   1   // relay not associated / no IP
#define BBX_ST_TRANSPORT 2   // DNS, TCP, or TLS failure
#define BBX_ST_HTTP      3   // non-2xx; body holds the numeric status as text
#define BBX_ST_BUSY      4   // relay's job queue was full
#define BBX_ST_XFER_BUSY 5   // GRANT denied; another badge holds the lease
#define BBX_ST_CRC       6   // reassembled payload failed its CRC32
#define BBX_ST_TOO_BIG   7   // total_len exceeds the station's capacity
#define BBX_ST_TIMEOUT   8   // session expired before it completed
#define BBX_ST_BAD_SESS  9   // frame for an unknown or stale session

typedef struct __attribute__((packed)) {
    char     magic[4];   // 'B','B','X','1'
    uint8_t  type;       // BBX_TYPE_*
    uint8_t  flags;      // BBX_FLAG_*
    uint16_t req_id;     // little-endian, random per request, never 0
    uint8_t  badge[6];   // requester identity; RESP/ACK echo it verbatim
    uint8_t  seq;        // frame index, 0..total-1. 0 when unfragmented.
    uint8_t  total;      // frame count; 1 = unfragmented (rev 1 compatible)
    uint8_t  status;     // BBX_ST_*, RESP / XFER_GRANT / XFER_DONE only
    uint8_t  sess;       // WAS _rsv. Transfer session id; 0 = no session.
} bbx_hdr_t;

// Badge side is C++, relay side is C.
#ifdef __cplusplus
static_assert(sizeof(bbx_hdr_t) == BBX_HDR_SIZE, "BBX1 header must be 18 bytes");
#else
_Static_assert(sizeof(bbx_hdr_t) == BBX_HDR_SIZE, "BBX1 header must be 18 bytes");
#endif

// req_id is naturally aligned at offset 6 and both ends are little-endian
// ESP32-C3, so frames memcpy straight to and from the wire — no serialisation.

static inline int bbx_magic_ok(const bbx_hdr_t *h)
{
    return h->magic[0] == BBX_MAGIC_0 && h->magic[1] == BBX_MAGIC_1
        && h->magic[2] == BBX_MAGIC_2 && h->magic[3] == BBX_MAGIC_3;
}

static inline void bbx_set_magic(bbx_hdr_t *h)
{
    h->magic[0] = BBX_MAGIC_0;
    h->magic[1] = BBX_MAGIC_1;
    h->magic[2] = BBX_MAGIC_2;
    h->magic[3] = BBX_MAGIC_3;
}

// ---------------------------------------------------------------------------
// Transfer extension — bulk badge -> station upload.
//
// Rev 1 senders all zero-init their header, so `seq`/`total`/`sess` arrive as
// 0/0/0 and rev 1 receivers ignore the new frame types outright. Adding this
// changed no bytes on the wire for REQ / RESP / ACK / BEACON.
//
// Sequence (selective repeat with a bitmap, not stop-and-wait — a per-frame
// round trip is where all the time would go):
//
//   badge   OPEN(kind, len, crc, chunk_len, N)   held until GRANT
//   station GRANT(sess, chunk_len)               or status=BBX_ST_XFER_BUSY
//   badge   DATA(sess, seq=0..N-1)               one sweep, dwelling per frame
//   badge   OPEN | BBX_FLAG_SWEEP_DONE           held; asks for a STAT
//   station STAT(sess, bitmap)                   what actually landed
//   badge   DATA for each clear bit              repair sweep, then STAT again
//   station DONE(sess, status)                   after the HTTP POST
//   badge   ACK(sess)                            frees the station early
//
// Two things that are easy to get wrong and fail silently:
//
//   * hal_radio_send() overwrites a *persistent* advertisement — it is a
//     register, not a queue. Every frame must be held for a dwell long enough
//     that at least one advertising event carries it. OPEN in particular
//     repeats tens of times per second, so a station MUST treat a repeated
//     OPEN matching the live (badge, req_id, sess) as idempotent: re-broadcast
//     the existing GRANT, never re-allocate and never reset the bitmap.
//
//   * Do NOT port the relay's dedupe LRU to a station. It keys on
//     (badge, req_id) and every DATA frame in a transfer shares one req_id, so
//     it drops frames 1..N-1 and presents as "uploads stall after the first
//     frame". The receive bitmap is the dedupe — setting a set bit is a no-op.
// ---------------------------------------------------------------------------

// Bit i of the bitmap = frame i received. 32 bytes covers 256 frames, but seq
// and total_frames are uint8_t, so the real ceiling is 255 frames. Going past
// BBX_XFER_MAX_LEN means widening seq/total to uint16_t, which IS a
// header-breaking change — that is the v2 boundary.
//
// Note total_frames, not total_len (uint16, 65535), is the binding limit.
#define BBX_XFER_BITMAP_BYTES 32
#define BBX_XFER_MAX_FRAMES   255
#define BBX_XFER_MAX_LEN      (BBX_XFER_MAX_FRAMES * BBX_MAX_BODY)   // 59160

// bbx_xfer_open_t.kind
#define BBX_XFER_KIND_UPLOAD  1
#define BBX_XFER_KIND_RESTORE 2   // reserved; downlink is not implemented

// bbx_xfer_open_t.enc
#define BBX_XFER_ENC_RAW      0
#define BBX_XFER_ENC_LZ4      1   // reserved

typedef struct __attribute__((packed)) {
    uint8_t  kind;          // BBX_XFER_KIND_*
    uint8_t  enc;           // BBX_XFER_ENC_*
    uint16_t total_len;     // payload bytes; <= BBX_XFER_MAX_LEN
    uint32_t crc32;         // esp_rom_crc32_le(0, payload, total_len).
                            // Seed 0 — this is the zlib-compatible convention,
                            // NOT the UINT32_MAX one also used inside IDF.
    uint16_t chunk_len;     // bytes per frame except the last; <= BBX_MAX_BODY
    uint8_t  total_frames;  // 1..BBX_XFER_MAX_FRAMES
    uint8_t  want_downlink; // 1: upload returns the latest profile, 0: legacy upload
} bbx_xfer_open_t;

typedef struct __attribute__((packed)) {
    uint16_t chunk_len;     // station's accepted chunk size (echoes or shrinks)
    uint8_t  backoff_ds;    // on deny: deciseconds to wait. Add jitter, or two
                            // denied badges retry in lockstep forever.
    uint8_t  queue_ahead;   // badges ahead of you, for a "2 ahead" message
} bbx_xfer_grant_t;

typedef struct __attribute__((packed)) {
    uint8_t  total;         // echo of total_frames
    uint8_t  recv_count;    // popcount(bitmap), so the sender can show progress
    uint8_t  bitmap[BBX_XFER_BITMAP_BYTES];
} bbx_xfer_stat_t;

#ifdef __cplusplus
#define BBX_STATIC_ASSERT(c, m) static_assert(c, m)
#else
#define BBX_STATIC_ASSERT(c, m) _Static_assert(c, m)
#endif

BBX_STATIC_ASSERT(sizeof(bbx_xfer_open_t)  == 12, "bbx_xfer_open_t must be 12 bytes");
BBX_STATIC_ASSERT(sizeof(bbx_xfer_grant_t) ==  4, "bbx_xfer_grant_t must be 4 bytes");
BBX_STATIC_ASSERT(sizeof(bbx_xfer_stat_t)  == 34, "bbx_xfer_stat_t must be 34 bytes");
// A full DATA frame is exactly BBX_MAX_FRAME.
BBX_STATIC_ASSERT(BBX_HDR_SIZE + BBX_MAX_BODY == BBX_MAX_FRAME, "DATA frame overflows");
// ...and a full DATA frame plus its AD wrapper survives BOTH directions:
// transmissible while advertising, and deliverable in one report at the peer.
BBX_STATIC_ASSERT(BBX_MAX_FRAME + BBX_AD_OVERHEAD <= BBX_HCI_MAX_ADV_DATA,
                  "frame + AD wrapper exceeds the single-HCI-command TX limit");
BBX_STATIC_ASSERT(BBX_MAX_FRAME + BBX_AD_OVERHEAD <= BBX_HCI_MAX_ADV_RPT,
                  "frame + AD wrapper would arrive fragmented and be dropped");
// Every extension frame must fit one advertisement alongside its header.
BBX_STATIC_ASSERT(sizeof(bbx_xfer_stat_t) <= BBX_MAX_BODY, "STAT body too large");

// Bit order is LSB-first within each byte. Both ends must agree, so always go
// through these rather than open-coding the shift.
static inline int bbx_bitmap_test(const uint8_t *bm, uint8_t seq)
{
    return (bm[seq >> 3] >> (seq & 7)) & 1;
}

static inline void bbx_bitmap_set(uint8_t *bm, uint8_t seq)
{
    bm[seq >> 3] |= (uint8_t)(1u << (seq & 7));
}

// Frames are fixed-size except the last, which carries the remainder.
static inline uint16_t bbx_chunk_size(uint16_t total_len, uint16_t chunk_len,
                                      uint8_t total_frames, uint8_t seq)
{
    if (chunk_len == 0 || seq >= total_frames) return 0;
    if (seq + 1 < total_frames) return chunk_len;
    return (uint16_t)(total_len - (uint16_t)seq * chunk_len);
}
