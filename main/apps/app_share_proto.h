#pragma once

// App Share wire protocol ("APP1").
//
// Badge-to-badge bulk transfer for user Lua apps. Rides the shared hal_radio
// manufacturer-data channel (company 0xFFFF) alongside BMP2 / BBX1 / MPG1 /
// LUA1 frames — the 4-byte magic below is what tells them apart. Every
// listener on that channel sees every frame, so always check the magic first.
//
// Roles:
//   sender   picks an installed Lua app, packs /littlefs/apps/<slug>/ into a
//            bundle (see share_bundle.h) and immediately advertises an offer.
//   receiver listens, shows an accept prompt on a strong offer, then starts
//            collecting DATA on user acceptance and installs.
//
// Handshake (all frames carry req_id, a sender-chosen nonzero session id):
//   sender   OFFER(bundle_len, crc, total_frames, slug, name, version)
//   receiver ACCEPT(receiver display name)            — after user acceptance
//   sender   DATA(seq, chunk) * total_frames          — sweep, then repair
//   receiver STAT(total, recv_count, bitmap)          — held continuously
//   sender   DONE(status)                              — bitmap full, or abort
//
// THE REGISTER RULE (hal_radio.h:54-63): hal_radio_send() overwrites a
// persistent advertisement, it does not queue a transmit. So OFFER / ACCEPT /
// STAT are values the peer reads, not messages: every handler is idempotent,
// delivery is proven by seeing your own content echoed (STAT bitmap filling),
// and "stop talking" is hal_radio_adv_pause(). The sweep/repair discipline
// mirrors bbx_xfer: hold each frame for a dwell, then repair only the frames
// the bitmap says are missing.
//
// Frame size is dictated by the radio, not chosen: 225 bytes max payload
// (see hal_radio.h), so bodies are capped at 213. Do not raise these without
// re-proving both HCI directions.
//
// This header is deliberately free of LVGL and ESP-IDF so the host test in
// tools/share-test/ can verify it with plain g++.

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define SHARE_MAGIC_0 'A'
#define SHARE_MAGIC_1 'P'
#define SHARE_MAGIC_2 'P'
#define SHARE_MAGIC_3 '1'
#define SHARE_VERSION 1

#define SHARE_MAX_FRAME 225   // == kHalRadioMaxPayload
#define SHARE_HDR_SIZE  12
#define SHARE_MAX_BODY  (SHARE_MAX_FRAME - SHARE_HDR_SIZE)   // 213

// Frame types.
#define SHARE_TYPE_OFFER  0x01   // sender -> anyone, body = offer fields
#define SHARE_TYPE_ACCEPT 0x02   // receiver -> sender, body = display name
#define SHARE_TYPE_DATA   0x03   // sender -> receiver, body = one chunk
#define SHARE_TYPE_STAT   0x04   // receiver -> sender, body = share_stat_t
#define SHARE_TYPE_DONE   0x05   // sender -> receiver, body = one status byte

// DONE status.
#define SHARE_OK      0
#define SHARE_CANCEL  1

// Chunk payload per DATA frame. Full bodies keep airtime (and sweep time)
// minimal; 48 KiB needs at most 232 frames, inside the 255-frame bitmap.
#define SHARE_CHUNK_LEN SHARE_MAX_BODY

// Bitmap covers 256 frames; seq/total are uint8_t so 255 is the real ceiling.
// A bundle needing more frames is rejected at pack time, not mid-sweep.
#define SHARE_BITMAP_BYTES 32
#define SHARE_MAX_FRAMES   255

// Field caps mirror the loader (script_app.cpp) and identity widths so an
// offer round-trips without truncation.
#define SHARE_MAX_SLUG  32
#define SHARE_MAX_NAME  48
#define SHARE_MAX_VER   48
#define SHARE_MAX_PEER  39   // display name, sizeof(BadgeIdentity::display_name)-1

typedef struct __attribute__((packed)) {
    char     magic[4];   // 'A','P','P','1' — check FIRST, the channel is shared
    uint8_t  ver;        // SHARE_VERSION
    uint8_t  type;       // SHARE_TYPE_*
    uint16_t req_id;     // little-endian, sender-chosen per transfer, never 0
    uint8_t  seq;        // DATA frame index, 0..total-1
    uint8_t  total;      // DATA frame count (echoed in STAT)
    uint8_t  status;     // DONE only; SHARE_OK / SHARE_CANCEL
    uint8_t  rsv;        // zero
} share_hdr_t;

typedef struct __attribute__((packed)) {
    uint8_t total;                          // echo of total_frames
    uint8_t recv_count;                     // popcount(bitmap), for progress
    uint8_t bitmap[SHARE_BITMAP_BYTES];
} share_stat_t;

#ifdef __cplusplus
#define SHARE_STATIC_ASSERT(c, m) static_assert(c, m)
#else
#define SHARE_STATIC_ASSERT(c, m) _Static_assert(c, m)
#endif

SHARE_STATIC_ASSERT(sizeof(share_hdr_t) == SHARE_HDR_SIZE, "share header must be 12 bytes");
SHARE_STATIC_ASSERT(sizeof(share_stat_t) == 34, "share_stat_t must be 34 bytes");
SHARE_STATIC_ASSERT(sizeof(share_stat_t) <= SHARE_MAX_BODY, "STAT body too large");
SHARE_STATIC_ASSERT(SHARE_HDR_SIZE + SHARE_MAX_BODY == SHARE_MAX_FRAME, "frame overflows");

// Bit order is LSB-first within each byte. Both ends must agree, so always go
// through these rather than open-coding the shift.
static inline int share_bitmap_test(const uint8_t *bm, uint8_t seq)
{
    return (bm[seq >> 3] >> (seq & 7)) & 1;
}

static inline void share_bitmap_set(uint8_t *bm, uint8_t seq)
{
    bm[seq >> 3] |= (uint8_t)(1u << (seq & 7));
}

static inline int share_bitmap_popcount(const uint8_t *bm, uint8_t total)
{
    int n = 0;
    for (uint8_t i = 0; i < total; i++) n += share_bitmap_test(bm, i);
    return n;
}

static inline int share_magic_ok(const share_hdr_t *h)
{
    return h->magic[0] == SHARE_MAGIC_0 && h->magic[1] == SHARE_MAGIC_1
        && h->magic[2] == SHARE_MAGIC_2 && h->magic[3] == SHARE_MAGIC_3;
}

static inline void share_set_magic(share_hdr_t *h)
{
    h->magic[0] = SHARE_MAGIC_0;
    h->magic[1] = SHARE_MAGIC_1;
    h->magic[2] = SHARE_MAGIC_2;
    h->magic[3] = SHARE_MAGIC_3;
}

// Frames memcpy straight to and from the wire on the little-endian C3, so
// multi-byte fields are little-endian on the air. These helpers keep the one
// unaligned-safe access pattern in a single place.
static inline uint16_t share_rd_u16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static inline void share_wr_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)((v >> 8) & 0xff);
}

static inline uint32_t share_rd_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
        | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline void share_wr_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)((v >> 8) & 0xff);
    p[2] = (uint8_t)((v >> 16) & 0xff);
    p[3] = (uint8_t)((v >> 24) & 0xff);
}

// OFFER body: u16 total_len | u32 crc | u8 total_frames |
//             u8 slug_len | u8 name_len | u8 ver_len | slug | name | version.
// Returns the body length, or 0 if the strings do not fit one frame.
static inline size_t share_offer_build(uint8_t *out, size_t out_sz,
                                       uint16_t total_len, uint32_t crc,
                                       uint8_t total_frames,
                                       const char *slug, const char *name,
                                       const char *version)
{
    size_t sl = slug ? strlen(slug) : 0;
    size_t nl = name ? strlen(name) : 0;
    size_t vl = version ? strlen(version) : 0;
    if (sl == 0 || sl > SHARE_MAX_SLUG) return 0;
    if (nl > SHARE_MAX_NAME || vl > SHARE_MAX_VER) return 0;
    size_t need = 2 + 4 + 1 + 1 + 1 + 1 + sl + nl + vl;
    if (need > out_sz || need > SHARE_MAX_BODY) return 0;
    size_t pos = 0;
    share_wr_u16(out + pos, total_len); pos += 2;
    share_wr_u32(out + pos, crc); pos += 4;
    out[pos++] = total_frames;
    out[pos++] = (uint8_t)sl;
    out[pos++] = (uint8_t)nl;
    out[pos++] = (uint8_t)vl;
    memcpy(out + pos, slug, sl); pos += sl;
    if (nl) { memcpy(out + pos, name, nl); pos += nl; }
    if (vl) { memcpy(out + pos, version, vl); pos += vl; }
    return pos;
}

// Parse an OFFER body. String outputs are NUL-terminated and bounded; pass
// null/size 0 for any the caller does not need. Returns false on truncation.
static inline int share_offer_parse(const uint8_t *body, size_t len,
                                    uint16_t *out_total_len, uint32_t *out_crc,
                                    uint8_t *out_total_frames,
                                    char *slug_out, size_t slug_sz,
                                    char *name_out, size_t name_sz,
                                    char *ver_out, size_t ver_sz)
{
    if (!body || len < 10) return 0;
    size_t pos = 0;
    uint16_t total_len = share_rd_u16(body + pos); pos += 2;
    uint32_t crc = share_rd_u32(body + pos); pos += 4;
    uint8_t total_frames = body[pos++];
    uint8_t sl = body[pos++];
    uint8_t nl = body[pos++];
    uint8_t vl = body[pos++];
    if (sl == 0 || sl > SHARE_MAX_SLUG) return 0;
    if (nl > SHARE_MAX_NAME || vl > SHARE_MAX_VER) return 0;
    if (total_frames == 0 || total_frames > SHARE_MAX_FRAMES) return 0;
    if (pos + sl + nl + vl != len) return 0;
    if (out_total_len) *out_total_len = total_len;
    if (out_crc) *out_crc = crc;
    if (out_total_frames) *out_total_frames = total_frames;
    // Bounded copies: truncate rather than fail, the loader re-validates.
    if (slug_out && slug_sz) {
        size_t n = sl < slug_sz - 1 ? sl : slug_sz - 1;
        memcpy(slug_out, body + pos, n);
        slug_out[n] = '\0';
    }
    pos += sl;
    if (name_out && name_sz) {
        size_t n = nl < name_sz - 1 ? nl : name_sz - 1;
        if (n) memcpy(name_out, body + pos, n);
        name_out[n] = '\0';
    }
    pos += nl;
    if (ver_out && ver_sz) {
        size_t n = vl < ver_sz - 1 ? vl : ver_sz - 1;
        if (n) memcpy(ver_out, body + pos, n);
        ver_out[n] = '\0';
    }
    return 1;
}

// ACCEPT body: u8 name_len | display name (may be empty for unprovisioned).
static inline size_t share_accept_build(uint8_t *out, size_t out_sz,
                                        const char *display_name)
{
    size_t nl = display_name ? strlen(display_name) : 0;
    if (nl > SHARE_MAX_PEER) nl = SHARE_MAX_PEER;
    if (1 + nl > out_sz || 1 + nl > SHARE_MAX_BODY) return 0;
    out[0] = (uint8_t)nl;
    if (nl) memcpy(out + 1, display_name, nl);
    return 1 + nl;
}

static inline int share_accept_parse(const uint8_t *body, size_t len,
                                     char *name_out, size_t name_sz)
{
    if (!body || len < 1) return 0;
    uint8_t nl = body[0];
    if (nl > SHARE_MAX_PEER) return 0;
    if (1u + nl != len) return 0;
    if (name_out && name_sz) {
        size_t n = nl < name_sz - 1 ? nl : name_sz - 1;
        if (n) memcpy(name_out, body + 1, n);
        name_out[n] = '\0';
    }
    return 1;
}
