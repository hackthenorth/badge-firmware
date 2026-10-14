#include "button_bump.h"
#include <cassert>
#include <cstdio>

using namespace ButtonBump;
static const uint8_t ALICE[] = {1, 0, 0, 0, 0, 0};
static const uint8_t BOB[] = {2, 0, 0, 0, 0, 0};
static const uint8_t OTHER[] = {3, 0, 0, 0, 0, 0};

static Frame frame(const Attempt &attempt)
{
    Frame message;
    message.nonce = attempt.nonce();
    message.ack_nonce = attempt.peer_nonce();
    snprintf(message.card.badge_id, sizeof(message.card.badge_id), "test-peer");
    return message;
}

static void pair_at(int64_t delay)
{
    Attempt a, b;
    a.begin(0, 101);
    // B's uptime is different. Only elapsed local time matters.
    b.begin(9000000 + delay, 202);
    assert(a.receive(delay, BOB, frame(b)));
    assert(!a.confirmed()); // Hearing a card alone cannot save it.
    assert(b.receive(9000000 + delay, ALICE, frame(a)));
    assert(b.confirmed());
    assert(a.receive(delay + 10000, BOB, frame(b)));
    assert(a.confirmed()); // ACK can arrive after the one-second pairing window.
    assert(a.advertise(1500000));
    assert(!a.advertise(1500001));
}

int main()
{
    pair_at(0);
    pair_at(999000);
    pair_at(1000000);

    // If B presses at the end of A's window, B's matching ACK may cross the
    // air afterward. It proves B heard A while A was still advertising, so the
    // bounded ACK grace must complete the exchange.
    Attempt edge_a, edge_b;
    edge_a.begin(0, 101);
    edge_b.begin(999000, 202);
    assert(edge_b.receive(999000, ALICE, frame(edge_a)));
    assert(edge_a.receive(1250000, BOB, frame(edge_b)));
    assert(edge_a.confirmed());

    Attempt a, b;
    a.begin(0, 101);
    b.begin(1001000, 202);
    auto unacknowledged_late_peer = frame(b);
    unacknowledged_late_peer.ack_nonce = 0;
    assert(!a.receive(1003000, BOB, unacknowledged_late_peer));
    assert(!a.confirmed());

    a.begin(1000000, 303);
    assert(!a.receive(999999, BOB, frame(b))); // Queued before the local press.
    auto stale = frame(b);
    stale.ack_nonce = 101;
    assert(!a.receive(1001000, BOB, stale)); // ACK from the previous attempt.
    b.begin(1000000, 404);
    assert(a.receive(1003000, BOB, frame(b)));
    assert(!a.receive(1004000, OTHER, frame(b))); // Lock to one peer MAC.
    stale = frame(b);
    stale.nonce++;
    assert(!a.receive(1005000, BOB, stale)); // And to that peer's attempt.
    assert(!a.receive(2500001, BOB, frame(b))); // Absolute hard cap.
    a.begin(2000000, 505);
    assert(a.advertise(3000000));
    assert(!a.advertise(3000001)); // Unmatched cards stop after the pairing window.

    uint8_t bytes[BUMP_MAX_FRAME + 1]{};
    auto message = frame(a);
    message.ack_nonce = 606;
    snprintf(message.card.display_name, sizeof(message.card.display_name), "Badge QA");
    const size_t size = encode(bytes, sizeof(bytes), message);
    assert(size > HEADER_SIZE && size <= BUMP_MAX_FRAME);
    Frame decoded;
    assert(decode(bytes, size, decoded));
    assert(decoded.nonce == 505 && decoded.ack_nonce == 606);
    assert(strcmp(decoded.card.display_name, "Badge QA") == 0);
    BumpCard legacy;
    assert(!bump_decode(bytes, size, &legacy, nullptr, 0));
    assert(!decode(bytes, size - 1, decoded)); // Truncated field.
    assert(!decode(bytes, BUMP_MAX_FRAME + 1, decoded));
    assert(!decode(bytes, HEADER_SIZE - 1, decoded));
    put_u32(bytes + 4, 0);
    assert(!decode(bytes, size, decoded));
    assert(!encode(bytes, HEADER_SIZE, message));
    message.nonce = 0;
    assert(!encode(bytes, sizeof(bytes), message));
    puts("Button bump: fixed one-second gate, ACK grace, late rejection, mutual ACK, stale attempts, peer lock, caps and codec: PASS");
}
