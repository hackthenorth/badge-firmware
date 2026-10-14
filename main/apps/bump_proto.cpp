#include "bump_proto.h"

#include <cstring>

namespace {

// One TLV: [tag:1][len:1][bytes]. Empty values are skipped entirely rather
// than emitted as zero-length records — an absent field and a blank one mean
// the same thing here, and skipping saves 2 bytes each.
bool put_tlv(uint8_t *out, size_t out_sz, size_t *pos, uint8_t tag, const char *val)
{
    if (!val || !*val) return true;            // nothing to send, not a failure

    size_t vlen = strlen(val);
    if (vlen > 255) vlen = 255;
    if (*pos + 2 + vlen > out_sz) return false;  // caller stops here

    out[(*pos)++] = tag;
    out[(*pos)++] = (uint8_t)vlen;
    memcpy(out + *pos, val, vlen);
    *pos += vlen;
    return true;
}

bool put_u8(uint8_t *out, size_t out_sz, size_t *pos, uint8_t tag,
            bool present, uint8_t value)
{
    if (!present) return true;
    if (*pos + 3 > out_sz) return false;
    out[(*pos)++] = tag;
    out[(*pos)++] = 1;
    out[(*pos)++] = value;
    return true;
}

void copy_bounded(char *dst, size_t dst_sz, const uint8_t *src, size_t len)
{
    if (len >= dst_sz) len = dst_sz - 1;
    memcpy(dst, src, len);
    dst[len] = '\0';
}

} // namespace

size_t bump_encode(uint8_t *out, size_t out_sz, const BumpCard &card, const char *ack_id)
{
    if (!out || out_sz < BUMP_HDR_SIZE) return 0;

    size_t pos = 0;
    memcpy(out, BUMP_MAGIC, 4);
    pos = 4;
    out[pos++] = BUMP_VERSION;
    out[pos++] = card.role;

    // badge_id is mandatory: a card the receiver can't key on is useless.
    if (!put_tlv(out, out_sz, &pos, BT_BADGE_ID, card.badge_id)) return 0;
    if (card.badge_id[0] == '\0') return 0;

    // ack_id comes second and is small: it's what lets the peer stop
    // advertising early, so it must survive truncation.
    put_tlv(out, out_sz, &pos, BT_ACK_ID, ack_id);

    // Ripple is an optional, non-PII experiment bit. Keep it ahead of contact
    // fields so a full card can still propagate its tiny state.
    put_u8(out, out_sz, &pos, BT_RIPPLE_DEPTH,
           card.ripple_active, card.ripple_depth);

    // Remaining fields in descending order of importance. put_tlv returning
    // false just means "this one didn't fit"; keep trying the shorter ones
    // rather than aborting, since a later field may still have room.
    put_tlv(out, out_sz, &pos, BT_DISPLAY_NAME,  card.display_name);
    put_tlv(out, out_sz, &pos, BT_NET_EMAIL,     card.net_email);
    put_tlv(out, out_sz, &pos, BT_NET_LINKEDIN,  card.net_linkedin);
    put_tlv(out, out_sz, &pos, BT_NET_DISCORD,   card.net_discord);
    put_tlv(out, out_sz, &pos, BT_NET_INSTAGRAM, card.net_instagram);
    put_tlv(out, out_sz, &pos, BT_NET_X,         card.net_x);
    put_tlv(out, out_sz, &pos, BT_NET_PHONE,     card.net_phone);

    return pos;
}

bool bump_decode(const uint8_t *data, size_t len,
                 BumpCard *out_card, char *out_ack_id, size_t ack_sz)
{
    if (!data || !out_card || len < BUMP_HDR_SIZE) return false;
    if (memcmp(data, BUMP_MAGIC, 4) != 0) return false;
    if (data[4] != BUMP_VERSION) return false;

    memset(out_card, 0, sizeof(*out_card));
    if (out_ack_id && ack_sz) out_ack_id[0] = '\0';

    out_card->role = data[5];

    size_t pos = BUMP_HDR_SIZE;
    while (pos + 2 <= len) {
        uint8_t tag = data[pos];
        uint8_t vlen = data[pos + 1];
        pos += 2;
        // A record claiming more bytes than remain means a corrupt or
        // truncated frame; take what we parsed rather than reading past it.
        if (pos + vlen > len) break;

        const uint8_t *val = data + pos;
        switch (tag) {
            case BT_BADGE_ID:
                copy_bounded(out_card->badge_id, sizeof(out_card->badge_id), val, vlen);
                break;
            case BT_ACK_ID:
                if (out_ack_id && ack_sz) copy_bounded(out_ack_id, ack_sz, val, vlen);
                break;
            case BT_DISPLAY_NAME:
                copy_bounded(out_card->display_name, sizeof(out_card->display_name), val, vlen);
                break;
            case BT_NET_EMAIL:
                copy_bounded(out_card->net_email, sizeof(out_card->net_email), val, vlen);
                break;
            case BT_NET_LINKEDIN:
                copy_bounded(out_card->net_linkedin, sizeof(out_card->net_linkedin), val, vlen);
                break;
            case BT_NET_DISCORD:
                copy_bounded(out_card->net_discord, sizeof(out_card->net_discord), val, vlen);
                break;
            case BT_NET_INSTAGRAM:
                copy_bounded(out_card->net_instagram, sizeof(out_card->net_instagram), val, vlen);
                break;
            case BT_NET_X:
                copy_bounded(out_card->net_x, sizeof(out_card->net_x), val, vlen);
                break;
            case BT_NET_PHONE:
                copy_bounded(out_card->net_phone, sizeof(out_card->net_phone), val, vlen);
                break;
            case BT_RIPPLE_DEPTH:
                if (vlen == 1) {
                    out_card->ripple_active = true;
                    out_card->ripple_depth = val[0];
                }
                break;
            default:
                break;   // forward compatibility: skip tags we don't know
        }
        pos += vlen;
    }

    return out_card->badge_id[0] != '\0';
}
