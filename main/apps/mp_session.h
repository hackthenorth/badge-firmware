#pragma once

#include "mp_proto.h"

#include <cstddef>
#include <cstdint>

// Badge-to-badge multiplayer session: bump to pair, then exchange game state
// until somebody leaves. Game-agnostic — this layer never looks inside a body.
//
// Free functions over file-static state, the shape of bbx_xfer rather than an
// object, for the reason bbx_xfer.h:17-21 gives: hal_radio has exactly ONE
// recv callback slot, so something has to own it, and an app that owned it
// permanently would have to multiplex every protocol through itself. The app
// installs a forwarding callback and drives this from its on_tick().
//
// WHAT MAKES THIS DIFFERENT FROM A CONNECTION: there isn't one. The badge's
// NimBLE build has both connection roles compiled out (sdkconfig:577-578), so
// this is extended-advertising broadcast and passive scan. hal_radio_send() is
// a register write, not a transmit — see mp_proto.h. Everything here follows
// from that:
//
//   * A peer's frame is a value, not a message. It repeats until they change
//     it, so receiving it twice means nothing, and every handler is idempotent.
//   * Delivery is proven by seeing your own content echoed in the peer's ack
//     fields, never by having sent it.
//   * "Stop talking" is hal_radio_adv_pause(), and it is free. "Stop
//     listening" is a scan duty cycle, and it is the entire power budget.
//
// The duty tiers in mp_session.cpp are the power story. Pairing runs the radio
// at 100% and is hard-capped at 30 s for that reason; in-game tiers run at
// 3-20%. But the largest saving by far is not a tier at all — it is
// kIdleAbandonTicks releasing the sleep wake lock on a game nobody is playing.

enum class MpState : uint8_t {
    Off,        // begin() not called, or it failed
    Seeking,    // advertising HELLO, looking for a bump partner
    Proposed,   // picked someone, waiting for them to agree
    Live,       // playing
    Over,       // the game ended; still in touch, rematch possible
    Lost,       // peer went silent past kPeerLostTicks
    Ended,      // we said BYE and are on our way out
};

enum class MpFail : uint8_t {
    None,
    Radio,       // hal_radio_enable() failed
    NoPartner,   // nobody bumped back before the seek timeout
    SeatClash,   // both badges computed the same seat; a bug, surfaced not hidden
    PeerGone,    // silent too long
    PeerLeft,    // clean BYE
};

// Radio duty tiers, in the order kDuty[] declares them.
enum class MpDuty : uint8_t {
    Pairing,    // 100% scan, 30 ms adv. Bounded by kSeekTimeoutTicks.
    Pushing,    // I have content the peer has not acked yet
    Waiting,    // acked; it is the peer's move, so a frame is coming
    Thinking,   // acked; it is my move, so only a resign can arrive
    Resting,    // over, lost, or pairing gave up
};

struct MpConfig {
    uint8_t game;               // MPG_GAME_*
    int     bump_threshold_mg;  // combined X/Y/Z sample delta; 300 mg default
    int     rssi_min;           // -60 is contact range at this badge's -3 dBm TX
    bool    require_bump;       // false pairs with the strongest peer, for bench work
};

// Sets the pairing timings, THEN enables the radio, THEN installs the recv
// callback — in that order, because hal_radio.cpp:429 re-applies the payload
// after a retune only when one has already been sent, so retuning an enabled
// radio that has never sent leaves the instance with no advertising data.
// hal_radio.h:81-82 documents that setting timings before enable is safe.
//
// False (and MpFail::Radio) if the radio would not come up.
bool mp_session_begin(const MpConfig &cfg);

// Clears the recv callback FIRST — it is invoked from the NimBLE host task and
// outliving this module would be a use-after-free (app_sync.cpp:96-98). Then
// pauses advertising and restores the default timings. Deliberately does NOT
// call hal_radio_disable(): that blocks up to 2 s and the registry does it on
// the next app switch anyway.
void mp_session_end();

// Drain, run the state machine, retune at most once. Call from on_tick().
void mp_session_tick();

// Force the bump window open without an accelerometer event. Backs the
// `chess bump` console verb, which is how duet.py pairs two badges sitting
// still on a desk.
void mp_session_note_bump();
// Attendee A-button path: mutual intent within 1 s, distinct from physical bumps.
void mp_session_note_button();

// The game owns whose move it is; the duty tiers need to know, because the
// side to move can only receive a resignation while the side waiting is about
// to receive a whole position.
void mp_session_set_my_turn(bool mine);

// Reset the abandonment timer. The app calls this on every LOCAL button press
// — not on peer traffic, because a peer politely repeating its register at
// 3.3% duty is not evidence that anyone is still holding either badge.
void mp_session_note_activity();

MpState  mp_session_state();
MpFail   mp_session_fail();
MpDuty   mp_session_duty();
uint32_t mp_session_id();
uint8_t  mp_session_seat();      // 0 = first mover
uint8_t  mp_session_game_no();
bool     mp_session_peer_mac(uint8_t out[6]);
int      mp_session_peer_rssi(); // 127 if never heard
int      mp_session_ticks_since_peer();
int      mp_session_candidates();
bool     mp_session_peer_flag(uint8_t flag);   // MPG_FLAG_* last seen from the peer

// --- game payload -----------------------------------------------------------

// Install the body this badge asserts, at sequence `seq`. The CRC is computed
// here rather than passed in, so there is exactly one implementation and no
// way for the sender's idea of the checksum to differ from the frame's.
//
// Calling with an unchanged (seq, body) is a no-op, so it is safe to call on
// every tick — that keeps "only touch the radio when something changed" out of
// the game code entirely.
bool mp_session_publish(const void *body, size_t len, uint16_t seq);

// The most recent validated peer frame, or nullptr. Stays available until
// mp_session_take_inbound() consumes it.
const mpg_hdr_t *mp_session_inbound(const uint8_t **body, size_t *len);
void mp_session_take_inbound();

// Record that we have adopted the peer's state, so our next frame tells them
// they can stop pushing it.
void mp_session_ack(uint16_t peer_seq, uint16_t peer_sum);

// True once the peer's ack fields name what we published. This is the only
// honest definition of "delivered" on a broadcast register.
bool mp_session_published_acked();

// --- intents ----------------------------------------------------------------
//
// Session-level intents live in header flags only, never in the body, so there
// is nowhere for the two to disagree.

void mp_session_offer_draw(bool on);
void mp_session_offer_rematch(bool on);
void mp_session_resign();
void mp_session_declare_over();   // the game ended by its own rules

// Hold a BYE for kByeHoldTicks so the peer learns we left rather than timing
// out. Returns true once the hold has elapsed and it is safe to leave the app.
// Cannot be done from on_exit(): that runs synchronously under the LVGL lock
// immediately before the registry's hal_radio_disable(), with no ticks left.
void mp_session_say_bye();
bool mp_session_bye_done();

// 0 = first mover. Antisymmetric by construction, so both badges call the same
// function with swapped arguments and get opposite answers with no negotiation
// round trip. MACs are derived from the chip's WiFi MAC (hal_radio.cpp:92-98)
// and so are always distinct, which is what makes the tie-break total.
uint8_t mp_seat_for(uint32_t na, const uint8_t ma[6],
                    uint32_t nb, const uint8_t mb[6]);

size_t mp_session_debug(char *out, size_t out_sz);
