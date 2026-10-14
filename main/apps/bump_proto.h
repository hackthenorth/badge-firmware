#pragma once

#include <cstddef>
#include <cstdint>

// Badge Bump wire format ("BMP2").
//
// Rides the shared hal_radio manufacturer-data channel (company 0xFFFF)
// alongside BBX1 frames — the 4-byte magic is what tells them apart. Every
// listener on that channel sees every frame, so always check the magic first.
//
// Variable-length TLV rather than a fixed struct. A fixed struct carrying
// every field at maximum width would be ~340 bytes, over the 250-byte frame
// cap; most attendees fill three or four networking fields, so TLV typically
// lands around 110-160 bytes. Shorter frames also carry further, which
// matters for a feature that has to work reliably in a crowded room.
//
// PRIVACY: this payload is the attendee's contact card in the clear. It must
// only ever be advertised in the brief window after a detected bump — see
// app_badge_bump. hal_radio_send() installs a *persistent* advertisement, so
// forgetting to pause afterwards broadcasts PII until the app is closed.

#define BUMP_MAGIC   "BMP2"
#define BUMP_VERSION 2

// hal_radio_send()'s cap (kHalRadioMaxPayload). The AD wrapper adds 4 bytes on
// top, and the whole blob must fit one HCI Set Extended Advertising Data
// report (229) at the receiver — so 225. See hal_radio.h. A larger packet is
// rejected by the NimBLE host with EINVAL and never reaches the air, which
// looks like a bump that silently didn't land.
#define BUMP_MAX_FRAME 225
#define BUMP_HDR_SIZE  6

enum BumpTag : uint8_t {
    BT_BADGE_ID      = 1,
    BT_ACK_ID        = 2,   // badge_id of a peer heard within the sync window
    BT_DISPLAY_NAME  = 3,
    BT_NET_EMAIL     = 4,
    BT_NET_LINKEDIN  = 5,
    BT_NET_DISCORD   = 6,
    BT_NET_INSTAGRAM = 7,
    BT_NET_X         = 8,
    BT_NET_PHONE     = 9,
    BT_RIPPLE_DEPTH  = 10,  // optional one-byte propagation depth
};

// Field widths match BadgeIdentity so a card round-trips without truncation.
struct BumpCard {
    char    badge_id[64];
    char    display_name[40];
    char    net_email[64];
    char    net_phone[20];
    char    net_linkedin[40];
    char    net_discord[40];
    char    net_instagram[32];
    char    net_x[32];
    uint8_t role;
    bool    ripple_active;
    uint8_t ripple_depth;
};

// Serialise `card` into `out`. `ack_id` may be nullptr or empty.
//
// Fields are emitted in a fixed priority order — badge_id, ack_id,
// display_name, email, then the remaining socials — so that if the frame
// fills up, what gets dropped is the least important field rather than an
// arbitrary one. A record is never emitted partially: encoding stops cleanly
// at the last record that fits.
//
// Returns the number of bytes written, or 0 if `out_sz` cannot even hold the
// header plus badge_id.
size_t bump_encode(uint8_t *out, size_t out_sz,
                   const BumpCard &card, const char *ack_id);

// Parse a received frame. Returns false if the magic or version don't match,
// the frame is truncated mid-record, or badge_id is absent (a card with no
// identity is not storable). Unknown tags are skipped, so a future firmware
// can add fields without breaking older badges.
//
// `out_ack_id` receives the peer's BT_ACK_ID if present, else an empty
// string. Pass ack_sz >= sizeof(BumpCard::badge_id).
bool bump_decode(const uint8_t *data, size_t len,
                 BumpCard *out_card, char *out_ack_id, size_t ack_sz);
