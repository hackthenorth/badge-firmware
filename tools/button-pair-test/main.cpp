#include "button_pair.h"

#include <cassert>
#include <cstdio>
#include <initializer_list>

struct Frame { uint32_t nonce, ack_nonce; };
static const uint8_t mac_a[6] = {1, 2, 3, 4, 5, 6};
static const uint8_t mac_b[6] = {2, 3, 4, 5, 6, 7};

static void deadlines()
{
    constexpr int64_t start = 1000000;
    for (int64_t duration : {int64_t{0}, int64_t{10000}, int64_t{999999},
                            int64_t{1000000}, int64_t{1500000}, int64_t{1999999},
                            int64_t{2000000}, int64_t{2000001}, int64_t{6000000}}) {
        ButtonPair::Attempt a;
        a.begin_held(start, 10);
        assert(a.advertise(start + 1999999));
        assert(a.advertise(start + 2000000));
        assert(!a.advertise(start + 2000001));
        a.release(start + duration);
        const int64_t window = duration < 1000000 ? 1000000 : duration > 2000000 ? 2000000 : duration;
        assert(a.advertise(start + window));
        assert(!a.advertise(start + window + 1));
        assert(a.active(start + window + 500000));
        assert(!a.active(start + window + 500001));
        a.release(start + 9000000);
        assert(!a.advertise(start + window + 1)); // Release cannot extend twice.
        assert(!a.receive(start - 1, mac_b, Frame{20, 10}));
        assert(!a.receive(start + window + 1, mac_b, Frame{20, 0}));
        assert(!a.receive(start + 1, mac_b, Frame{20, 999}));
        assert(a.receive(start + window + 1, mac_b, Frame{20, 10}));
        assert(a.confirmed());
        assert(a.advertise(start + window + 500000)); // ACK tail remains live.
        assert(!a.receive(start + window + 2, mac_a, Frame{20, 10}));
    }

    ButtonPair::Attempt fixed;
    fixed.begin(start, 1);
    fixed.release(start + 4000000);
    assert(fixed.advertise(start + 1000000));
    assert(!fixed.advertise(start + 1000001)); // Chess/Share retain fixed gates.
    assert(!fixed.active(start + ButtonPair::TX_CAP_US + 1));
    fixed.begin_held(start, 2);
    fixed.begin(start, 3);
    assert(!fixed.advertise(start + 1000001)); // Restart clears hold state.
}

static void packet_loss()
{
    // Model the repeated BLE payload, including long initial loss, staggered
    // starts and dropped ACKs. Neither endpoint stops on confirmation.
    for (int64_t offset : {int64_t{0}, int64_t{100000}, int64_t{600000}}) {
        for (int drop_mod : {2, 3, 5}) {
            ButtonPair::Attempt a, b;
            a.begin_held(0, 101);
            b.begin_held(offset, 202);
            a.release(3000000);
            b.release(offset + 3000000);
            uint32_t random = 0x12345678;
            auto delivered = [&] {
                random ^= random << 13;
                random ^= random >> 17;
                random ^= random << 5;
                return random % drop_mod != 0;
            };
            for (int64_t now = 0; now <= 4000000; now += 30000) {
                const Frame fa{a.nonce(), a.peer_nonce()};
                const Frame fb{b.nonce(), b.peer_nonce()};
                if (now < offset + 800000) continue;
                if (a.advertise(now) && delivered()) b.receive(now, mac_a, fa);
                if (b.advertise(now) && delivered()) a.receive(now, mac_b, fb);
            }
            assert(a.confirmed() && b.confirmed());
        }
    }
}

int main()
{
    deadlines();
    packet_loss();
    puts("Button pairing: 1-2 second holds, tap minimum, release, hard cap, ACK tail, stale/wrong peers, packet loss and unchanged fixed-window callers: PASS");
}
