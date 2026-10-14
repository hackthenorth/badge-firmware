#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace ButtonPair {
// begin() preserves the fixed one-second gate used by Chess and Share.
// Connect opts into begin_held()/release() for a 1-2 second gate. Keep a
// bounded transport tail for acknowledgements to make the BLE round trip.
constexpr int64_t PAIR_WINDOW_US = 1000000;
constexpr int64_t ACK_GRACE_US = 500000;
constexpr int64_t TX_CAP_US = PAIR_WINDOW_US + ACK_GRACE_US;
constexpr int64_t MAX_HOLD_US = 2000000;
// Clocks need not be synchronized. Each badge must hear its chosen peer after
// its own A press and inside its gate; only an ACK of that fresh nonce confirms
// the exchange. ACK retries may finish later, but never widen the pairing gate.
class Attempt {
public:
    void begin(int64_t now, uint32_t nonce)
    {
        *this = {};
        started_ = now;
        nonce_ = nonce;
    }
    void begin_held(int64_t now, uint32_t nonce)
    {
        begin(now, nonce);
        pair_window_us_ = MAX_HOLD_US;
        held_ = true;
    }
    void release(int64_t now)
    {
        if (!held_) return;
        const int64_t elapsed = now - started_;
        pair_window_us_ = elapsed < PAIR_WINDOW_US ? PAIR_WINDOW_US
            : elapsed > MAX_HOLD_US ? MAX_HOLD_US : elapsed;
        held_ = false;
    }
    bool started() const { return nonce_ != 0; }
    bool active(int64_t now) const
    {
        return started() && now >= started_
            && now - started_ <= pair_window_us_ + ACK_GRACE_US;
    }
    bool advertise(int64_t now) const
    {
        return active(now) && (peer_nonce_ || now - started_ <= pair_window_us_);
    }
    template <typename Frame>
    bool receive(int64_t received_at, const uint8_t mac[6], const Frame &frame)
    {
        if (!frame.nonce || !active(received_at)
            || (frame.ack_nonce && frame.ack_nonce != nonce_)) return false;
        if (!peer_nonce_) {
            // A matching ACK proves this peer heard our nonce while we were
            // advertising inside the acceptance gate. Accept that proof during
            // the bounded transport grace period; an unacknowledged new peer
            // still has to arrive inside the gate itself.
            const bool in_pair_window = received_at - started_ <= pair_window_us_;
            const bool acknowledges_us = frame.ack_nonce == nonce_;
            if (!in_pair_window && !acknowledges_us) return false;
            peer_nonce_ = frame.nonce;
            memcpy(peer_mac_, mac, sizeof(peer_mac_));
        }
        if (peer_nonce_ != frame.nonce || memcmp(peer_mac_, mac, sizeof(peer_mac_)))
            return false;
        if (frame.ack_nonce == nonce_) confirmed_ = true;
        return true;
    }
    uint32_t nonce() const { return nonce_; }
    uint32_t peer_nonce() const { return peer_nonce_; }
    bool confirmed() const { return confirmed_; }

private:
    int64_t started_ = 0;
    int64_t pair_window_us_ = PAIR_WINDOW_US;
    bool held_ = false;
    uint32_t nonce_ = 0, peer_nonce_ = 0;
    uint8_t peer_mac_[6] = {};
    bool confirmed_ = false;
};

// Separate protocol, bound to an application/offer and an explicit sender role.
// Both sides may retain ACK advertisements for the 1.5 s hard cap before
// resuming their protocol.
constexpr size_t SIZE = 23;
enum Kind : uint8_t { Chess = 1, ShareSender = 2, ShareReceiver = 3 };
struct Intent {
    uint32_t nonce = 0, ack_nonce = 0, context = 0;
    uint8_t kind = 0, mac[6] = {};
};
inline void encode(uint8_t out[SIZE], const Intent &f)
{
    memcpy(out, "BPI1", 4);
    const uint32_t values[] = {f.nonce, f.ack_nonce, f.context};
    for (int n = 0; n < 3; ++n)
        for (int b = 0; b < 4; ++b) out[4 + n*4 + b] = values[n] >> (b*8);
    out[16] = f.kind;
    memcpy(out + 17, f.mac, 6);
}
inline bool decode(const uint8_t *data, size_t size, Intent &f)
{
    if (size != SIZE || memcmp(data, "BPI1", 4)) return false;
    uint32_t values[3] = {};
    for (int n = 0; n < 3; ++n)
        for (int b = 0; b < 4; ++b) values[n] |= uint32_t(data[4+n*4+b]) << (b*8);
    f.nonce = values[0]; f.ack_nonce = values[1]; f.context = values[2];
    f.kind = data[16];
    memcpy(f.mac, data + 17, 6);
    return f.nonce && f.context && f.kind >= Chess && f.kind <= ShareReceiver;
}
} // namespace ButtonPair
