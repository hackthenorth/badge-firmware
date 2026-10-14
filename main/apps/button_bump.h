#pragma once

#include "bump_proto.h"
#include "button_pair.h"
#include <cstring>

// Separate magic keeps a button attempt from being mistaken for a physical
// bump by older firmware. The payload reuses the existing contact-card codec.
namespace ButtonBump {
static constexpr char MAGIC[] = "BPA1";
static constexpr size_t HEADER_SIZE = 12;
static constexpr int64_t PAIR_WINDOW_US = ButtonPair::PAIR_WINDOW_US;
static constexpr int64_t TX_CAP_US = ButtonPair::TX_CAP_US;

struct Frame {
    uint32_t nonce = 0;
    uint32_t ack_nonce = 0;
    BumpCard card{};
};

inline void put_u32(uint8_t *out, uint32_t value)
{
    for (unsigned i = 0; i < 4; ++i) out[i] = value >> (8 * i);
}
inline uint32_t get_u32(const uint8_t *in)
{
    uint32_t value = 0;
    for (unsigned i = 0; i < 4; ++i) value |= uint32_t(in[i]) << (8 * i);
    return value;
}

inline size_t encode(uint8_t *out, size_t capacity, const Frame &frame)
{
    if (!out || !frame.nonce || capacity < HEADER_SIZE + BUMP_HDR_SIZE) return 0;
    if (capacity > BUMP_MAX_FRAME) capacity = BUMP_MAX_FRAME;
    const size_t size = bump_encode(out + HEADER_SIZE, capacity - HEADER_SIZE,
                                    frame.card, nullptr);
    if (!size) return 0;
    memcpy(out, MAGIC, 4);
    put_u32(out + 4, frame.nonce);
    put_u32(out + 8, frame.ack_nonce);
    return HEADER_SIZE + size;
}

inline bool decode(const uint8_t *data, size_t size, Frame &frame)
{
    if (!data || size < HEADER_SIZE + BUMP_HDR_SIZE || size > BUMP_MAX_FRAME
        || memcmp(data, MAGIC, 4) != 0) return false;
    // Reject partial TLVs instead of confirming an incomplete card.
    size_t pos = HEADER_SIZE + BUMP_HDR_SIZE;
    while (pos + 2 <= size) pos += 2 + data[pos + 1];
    if (pos != size) return false;
    frame.nonce = get_u32(data + 4);
    frame.ack_nonce = get_u32(data + 8);
    return frame.nonce && bump_decode(data + HEADER_SIZE, size - HEADER_SIZE,
                                      &frame.card, nullptr, 0);
}

using Attempt = ButtonPair::Attempt;
} // namespace ButtonBump
