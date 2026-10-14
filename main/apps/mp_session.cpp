#include "mp_session.h"
#include "bump_detector.h"
#include "button_pair.h"

#include "hal_accel.h"
#include "hal_radio.h"
#include "hal_sleep.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"

#include <cstdio>
#include <cstring>

static const char *TAG = "mp";

// ---------------------------------------------------------------------------
// Timing. One tick is 20 ms (app_registry.cpp:121 creates the timer at 50 Hz).

namespace {

constexpr int kBumpWindowTicks     = 25;    // 500 ms, same as app_badge_bump.cpp:177
constexpr int kBumpRefractoryTicks = 8;     // 160 ms, collapses one impact into one bump
constexpr int kBumpRecentTicks     = 100;   // 2 s: how long a PROPOSE naming us is welcome
constexpr int kCandExpireTicks     = 100;   // 2 s of silence and a candidate is gone
constexpr int kBenchSettleTicks    = 75;    // 1.5 s of listening before bench-mode pairs
constexpr int kSeekTimeoutTicks    = 1500;  // 30 s. A HARD cap: seeking runs the radio
                                            // at 100% duty, the most expensive thing
                                            // this badge can do.
constexpr int kProposeHoldTicks    = 15;    // 300 ms minimum dwell before giving up
constexpr int kProposeTimeoutTicks = 150;   // 3 s
constexpr int kProposeJitterTicks  = 50;    // + up to 1 s, see restart_seeking()
constexpr int kProposeRetries      = 3;
constexpr int kEscalateTicks       = 50;    // 1 s of Pushing after ANY peer frame
constexpr int kMinTierTicks        = 10;    // 200 ms floor between radio retunes
constexpr int kPeerLostTicks       = 3000;  // 60 s
constexpr int kByeHoldTicks        = 50;    // 1 s
constexpr int kIdleAbandonTicks    = 30000; // 10 min with no LOCAL press
constexpr int kRssiTieDb           = 6;

constexpr int kMaxCandidates = 4;
ButtonPair::Attempt s_button;
ButtonPair::Intent s_button_peer;
int8_t s_button_rssi = 127;

// Radio duty tiers, indexed by MpDuty.
//
// The numbers come from a two-regime model of how long a passive scanner takes
// to catch a repeating advertiser. When the scan window is SHORTER than the
// advertiser's maximum event period, detection is geometric with a long tail:
// E[detect] ~= T_adv * interval / window. When the window is LONGER, every
// window contains at least one advertising event, so detection is ~certain per
// window and the worst case is BOUNDED at one scan interval.
//
// That model is calibrated against this repo's own measured number:
// bbx_xfer.cpp:26-28 reports ~1 s for a 30/150 scan against a 200 ms
// advertiser, and 200 * 150/30 = 1000 ms exactly.
//
// Hence the rule these rows follow: BUY DUTY REDUCTION FROM THE INTERVAL,
// NEVER FROM THE WINDOW, and never take the window below ~30 ms. A 30 ms
// window against a 30 ms advertiser (whose max event period is 40 ms with
// advDelay) sits in the geometric regime by a hair; 50 ms sits in the bounded
// one for 1.7x the duty. The scanner also has to stay free long enough to
// follow the AuxPtr to the AUX_ADV_IND that actually carries the payload.
//
// Note hal_radio.cpp:182 sets channel_map = 7, so every advertising event goes
// out on all three primaries and there is NO 1/3 channel penalty to model.
struct DutyTimings { uint32_t adv_min, adv_max, scan_itvl, scan_win; };

constexpr DutyTimings kDuty[] = {
    /* Pairing  */ {   30,   35,    60,  60 },   // 100%,  ~15 ms,  +68 mA
    /* Pushing  */ {   30,   35,   250,  50 },   //  20%, ~125 ms,  +17 mA
    /* Waiting  */ { 1000, 1100,   500,  50 },   //  10%, ~250 ms, +6.9 mA
    /* Thinking */ { 2000, 2200,  1500,  50 },   // 3.3%, ~750 ms, +2.3 mA
    /* Resting  */ {  500,  550,  1500,  50 },   // 3.3%, ~750 ms, +2.5 mA
};

const char *const kStateName[] = { "Off", "Seeking", "Proposed", "Live",
                                   "Over", "Lost", "Ended" };
const char *const kFailName[]  = { "None", "Radio", "NoPartner", "SeatClash",
                                   "PeerGone", "PeerLeft" };
const char *const kDutyName[]  = { "Pairing", "Pushing", "Waiting",
                                   "Thinking", "Resting" };

struct Cand {
    bool     in_use;
    uint8_t  mac[6];
    uint32_t nonce;
    int8_t   rssi;
    int      age;
    bool     bumping;
};

struct RxSlot {
    int64_t received_at;
    uint8_t  data[MPG_MAX_FRAME];
    uint16_t len;
    int8_t   rssi;
};

}  // namespace

// ---------------------------------------------------------------------------
// State

static MpConfig s_cfg{};
static MpState  s_state = MpState::Off;
static MpFail   s_fail  = MpFail::None;
static MpDuty   s_duty  = MpDuty::Pairing;

static uint8_t  s_my_mac[6];
static uint8_t  s_peer_mac[6];
static uint32_t s_nonce;
static uint32_t s_peer_nonce;
static uint32_t s_session;
static uint8_t  s_seat;
static uint8_t  s_game_no;
static int8_t   s_peer_rssi = 127;

static uint16_t s_seq, s_sum;              // what I assert
static uint16_t s_ack_seq, s_ack_sum;      // what I have adopted from the peer
static uint8_t  s_body[MPG_MAX_BODY];
static uint16_t s_body_len;

static uint16_t s_peer_seq, s_peer_sum;
static uint16_t s_peer_ack_seq, s_peer_ack_sum;
static uint8_t  s_peer_flags;
static uint16_t s_delivered_seq, s_delivered_sum;

static uint8_t  s_flags;                   // our own intent flags

static int  s_bump_window, s_bumped_recent;
static int  s_seek_ticks, s_propose_ticks, s_propose_timeout, s_propose_attempts;
static int  s_escalate, s_tier_hold, s_since_peer, s_idle_ticks, s_bye_ticks;
static bool s_my_turn, s_window_closed, s_wake_lock;
static CombinedAxisBumpDetector s_bump_detector;

static uint8_t s_tx[MPG_MAX_FRAME];
static size_t  s_tx_len;

static mpg_hdr_t s_in_hdr;
static uint8_t   s_in_body[MPG_MAX_BODY];
static size_t    s_in_body_len;
static bool      s_in_ready;

static Cand s_cands[kMaxCandidates];
static RxSlot s_rx[kMaxCandidates];
static uint8_t s_rx_count;
static SemaphoreHandle_t s_rx_mutex;

// ---------------------------------------------------------------------------

uint8_t mp_seat_for(uint32_t na, const uint8_t ma[6],
                    uint32_t nb, const uint8_t mb[6])
{
    if (na != nb) return na < nb ? 0 : 1;
    return memcmp(ma, mb, 6) < 0 ? 0 : 1;
}

namespace {

uint8_t wire_state()
{
    switch (s_state) {
        case MpState::Seeking:  return MPG_STATE_SEEKING;
        case MpState::Proposed: return MPG_STATE_PROPOSE;
        case MpState::Live:     return MPG_STATE_LIVE;
        case MpState::Over:     return MPG_STATE_OVER;
        default:                return MPG_STATE_IDLE;
    }
}

// esp_random() is only a true RNG once the radio is up, so this must never be
// called before hal_radio_enable() succeeds. The timer and MAC are folded in
// regardless, so even a degraded source cannot produce a predictable value.
void roll_nonce()
{
    uint32_t mix = 0;
    memcpy(&mix, s_my_mac + 2, 4);
    do {
        s_nonce = esp_random() ^ (uint32_t)esp_timer_get_time() ^ mix;
    } while (s_nonce == 0);   // 0 is the "no nonce" sentinel
}

Cand *cand_for(const uint8_t mac[6])
{
    Cand *empty = nullptr, *oldest = nullptr;
    for (auto &c : s_cands) {
        if (c.in_use && memcmp(c.mac, mac, 6) == 0) return &c;
        if (!c.in_use && !empty) empty = &c;
        if (c.in_use && (!oldest || c.age > oldest->age)) oldest = &c;
    }
    Cand *slot = empty ? empty : oldest;
    if (!slot) return nullptr;
    slot->in_use = true;
    memcpy(slot->mac, mac, 6);
    slot->age = 0;
    return slot;
}

// RSSI is not reciprocal — A can measure B at -50 while B measures A at -58 —
// so a plain argmax lets the two badges pick different partners. Quantising
// into kRssiTieDb buckets and tie-breaking on the MAC gives a TOTAL order that
// both sides compute identically from the same sightings. Deliberately no
// peer-switching: on genuine disagreement both sides time out and the user
// bumps again, which for a physical gesture is a perfectly good failure mode
// and deletes an entire state machine.
int rssi_bucket(int8_t r) { return ((int)r + 128) / kRssiTieDb; }

bool better(const Cand &a, const Cand &b)
{
    int ba = rssi_bucket(a.rssi), bb = rssi_bucket(b.rssi);
    if (ba != bb) return ba > bb;
    return memcmp(a.mac, b.mac, 6) < 0;
}

void restart_seeking()
{
    s_button = {};
    s_state = MpState::Seeking;
    s_session = 0;
    s_peer_nonce = 0;
    s_seat = 0;
    memset(s_peer_mac, 0, 6);
    for (auto &c : s_cands) c.in_use = false;
    roll_nonce();
    s_seek_ticks = 0;
    s_propose_ticks = 0;
    s_in_ready = false;
    s_seq = s_sum = s_ack_seq = s_ack_sum = 0;
    s_peer_seq = s_peer_sum = 0;
    s_delivered_seq = s_delivered_sum = 0;
    s_body_len = 0;
    s_flags = 0;
}

bool latch(const uint8_t mac[6], uint32_t nonce, int8_t rssi)
{
    // A nonce collision makes session 0 (the unpaired sentinel) AND ties the
    // seat rule. p = 2^-32, and detecting it is two lines.
    if (nonce == 0 || nonce == s_nonce) { roll_nonce(); return false; }
    uint32_t sess = s_nonce ^ nonce;
    if (sess == 0) { roll_nonce(); return false; }

    memcpy(s_peer_mac, mac, 6);
    s_peer_nonce = nonce;
    s_session    = sess;
    s_seat       = mp_seat_for(s_nonce, s_my_mac, nonce, mac);
    s_peer_rssi  = rssi;
    s_since_peer = 0;
    s_state      = MpState::Proposed;
    s_propose_ticks = 0;
    s_propose_timeout = kProposeTimeoutTicks + (int)(esp_random() % kProposeJitterTicks);
    ESP_LOGI(TAG, "proposed sess=%08lx seat=%u rssi=%d",
             (unsigned long)s_session, s_seat, rssi);
    return true;
}

void go_live()
{
    s_state = MpState::Live;
    s_fail = MpFail::None;
    s_since_peer = 0;
    s_idle_ticks = 0;
    ESP_LOGI(TAG, "live sess=%08lx seat=%u", (unsigned long)s_session, s_seat);
}

// A rematch cannot be a synchronised transition: whoever flips first would
// start sending a game_no the other side is still filtering out, and the two
// would deadlock each waiting for the other. So game_no IS the shared clock —
// whoever notices both flags advances, and the other follows the number.
// Seats swap, so the same badge is not White every time.
void advance_game(uint8_t n)
{
    s_game_no = n;
    s_seat    = (uint8_t)(s_seat ^ 1);
    s_flags  &= (uint8_t)~(MPG_FLAG_REMATCH | MPG_FLAG_DRAW_OFFER | MPG_FLAG_RESIGN);
    s_seq = s_sum = s_ack_seq = s_ack_sum = 0;
    s_peer_seq = s_peer_sum = 0;
    s_peer_ack_seq = s_peer_ack_sum = 0;
    s_delivered_seq = s_delivered_sum = 0;
    s_peer_flags = 0;
    s_body_len = 0;
    s_in_ready = false;
    s_state = MpState::Live;
    s_idle_ticks = 0;
    ESP_LOGI(TAG, "rematch: game %u, now seat %u", s_game_no, s_seat);
}

void pick_partner()
{
    Cand *best = nullptr;
    for (auto &c : s_cands) {
        if (!c.in_use) continue;
        if (s_cfg.require_bump) {
            // Mutual: our window is open AND they say theirs is. app_badge_bump
            // can infer mutuality from hearing anything at all, because it only
            // transmits after a bump. A pairing HELLO advertises continuously,
            // so the peer has to state it.
            if (!c.bumping || c.age > kBumpWindowTicks) continue;
        }
        if (c.rssi == 127 || c.rssi < s_cfg.rssi_min) continue;
        if (!best || better(c, *best)) best = &c;
    }
    if (best) latch(best->mac, best->nonce, best->rssi);
}

bool detect_bump()
{
    AccelSample s;
    if (hal_accel_read(&s) != ESP_OK) return false;
    return s_bump_detector.update(s, (float)s_cfg.bump_threshold_mg,
                                  kBumpRefractoryTicks);
}

// Rebuild the advertised register and push it ONLY if it actually changed.
// hal_radio_send() is an HCI round trip; re-sending an identical frame every
// tick would be 50 of them a second for nothing. Because MPG_FLAG_BUMPING is
// part of the frame, a bump naturally produces exactly two sends — one at each
// edge of the window — which is what makes the flag observable to a peer.
void refresh_frame()
{
    if (s_button.active(esp_timer_get_time())) {
        if (!s_button.advertise(esp_timer_get_time())) { hal_radio_adv_pause(); return; }
        ButtonPair::Intent intent{s_button.nonce(), s_button.peer_nonce(), s_cfg.game, ButtonPair::Chess, {}};
        memcpy(intent.mac, s_my_mac, 6);
        uint8_t frame[ButtonPair::SIZE]; ButtonPair::encode(frame, intent);
        hal_radio_send(frame, sizeof(frame)); hal_radio_adv_resume();
        s_tx_len = 0; // force the native HELLO to replace this temporary payload
        return;
    }
    uint8_t f[MPG_MAX_FRAME];
    memset(f, 0, MPG_HDR_SIZE);
    mpg_hdr_t *h = (mpg_hdr_t *)f;

    mpg_set_magic(h);
    h->ver     = MPG_VERSION;
    h->game    = s_cfg.game;
    h->flags   = (uint8_t)(s_flags | (s_bump_window > 0 ? MPG_FLAG_BUMPING : 0));
    h->session = s_session;
    memcpy(h->src, s_my_mac, 6);
    memcpy(h->dst, s_peer_mac, 6);
    h->state   = wire_state();
    h->seat    = s_seat;
    h->game_no = s_game_no;
    h->seq     = s_seq;
    h->ack_seq = s_ack_seq;
    h->ack_sum = s_ack_sum;

    size_t bl = 0;
    if (s_state == MpState::Ended) {
        h->type = MPG_TYPE_BYE;
    } else if (s_state == MpState::Seeking || s_state == MpState::Proposed) {
        h->type = MPG_TYPE_HELLO;
        mpg_hello_t hello{};
        hello.nonce      = s_nonce;
        hello.peer_nonce = s_peer_nonce;
        hello.games      = s_cfg.game;
        memcpy(f + MPG_HDR_SIZE, &hello, sizeof(hello));
        bl = sizeof(hello);
    } else {
        h->type = MPG_TYPE_STATE;
        memcpy(f + MPG_HDR_SIZE, s_body, s_body_len);
        bl = s_body_len;
    }
    h->body_len = (uint16_t)bl;
    h->sum = mpg_crc16(f + MPG_HDR_SIZE, bl);

    size_t len = MPG_HDR_SIZE + bl;
    if (len == s_tx_len && memcmp(f, s_tx, len) == 0) return;

    memcpy(s_tx, f, len);
    s_tx_len = len;
    if (hal_radio_send(s_tx, s_tx_len) == ESP_OK) hal_radio_adv_resume();
}

MpDuty compute_duty()
{
    if (s_state == MpState::Seeking || s_state == MpState::Proposed)
        return MpDuty::Pairing;
    // An inbound frame proves the peer is transmitting right now, which is
    // when listening is at its most productive — and the whole three-hop
    // ack settle fits inside one escalation window.
    if (s_escalate > 0) return MpDuty::Pushing;
    if (s_state != MpState::Live) return MpDuty::Resting;
    if (!mp_session_published_acked()) return MpDuty::Pushing;
    return s_my_turn ? MpDuty::Thinking : MpDuty::Waiting;
}

void apply_duty(MpDuty d)
{
    if (d == s_duty || s_tier_hold > 0) return;
    s_duty = d;
    s_tier_hold = kMinTierTicks;

    // Retuning is not free: set_scan_duty_ms cancels and restarts the scan
    // (hal_radio.cpp:450-452), which is a brief deaf gap, and
    // set_adv_interval_ms stops, reconfigures and restarts the instance. Doing
    // it only on a tier change, with a 200 ms floor between changes, is what
    // keeps that from becoming a permanent stutter.
    const DutyTimings &t = kDuty[(int)d];
    hal_radio_set_adv_interval_ms(t.adv_min, t.adv_max);
    hal_radio_set_scan_duty_ms(t.scan_itvl, t.scan_win);

    // hal_radio.cpp:429 re-applies the cached payload after a reconfigure, but
    // only when one has already been sent. Re-sending here costs one HCI
    // command per tier change and removes the ordering hazard entirely.
    if (s_tx_len) hal_radio_send(s_tx, s_tx_len);
}

// The badge holds the sleep wake lock for the whole game at ~72-77 mA, where
// light sleep is ~130 uA — three orders of magnitude. So this is worth far
// more than every duty tier combined, and it is the app's job to call it
// because app_registry.cpp:79 reads wants_wake_lock() exactly ONCE at launch.
void update_wake_lock()
{
    bool want = (s_state == MpState::Seeking || s_state == MpState::Proposed
                 || s_state == MpState::Live)
                && s_idle_ticks < kIdleAbandonTicks;
    if (want == s_wake_lock) return;
    s_wake_lock = want;
    hal_sleep_wake_lock(want);
}

// --- receive ---------------------------------------------------------------

void seek_frame(const mpg_hdr_t *h, const uint8_t *body, size_t bl, int8_t rssi)
{
    if (h->type != MPG_TYPE_HELLO || bl < sizeof(mpg_hello_t)) return;
    mpg_hello_t hello;
    memcpy(&hello, body, sizeof(hello));
    if (hello.nonce == 0) return;

    // Somebody has already picked us. Their PROPOSE carries OUR nonce, which
    // only a badge that heard our HELLO could know, and a session that must
    // equal the XOR of the two — so this is verifiable rather than merely
    // plausible. Accepting it is what makes pairing work when the two bumps
    // are far enough apart that only one badge's window saw the other's.
    if (h->state == MPG_STATE_PROPOSE && memcmp(h->dst, s_my_mac, 6) == 0) {
        if (s_bumped_recent <= 0) return;              // we never bumped
        if (hello.peer_nonce != s_nonce) return;
        if (h->session == 0 || h->session != (hello.nonce ^ s_nonce)) return;
        latch(h->src, hello.nonce, rssi);
        return;
    }

    if (h->state != MPG_STATE_SEEKING) return;
    if (rssi < s_cfg.rssi_min) return;
    if (!(hello.games & s_cfg.game)) return;

    Cand *c = cand_for(h->src);
    if (!c) return;
    c->nonce   = hello.nonce;
    c->rssi    = rssi;
    c->age     = 0;
    c->bumping = (h->flags & MPG_FLAG_BUMPING) != 0;
}

void propose_frame(const mpg_hdr_t *h, int8_t rssi)
{
    if (memcmp(h->src, s_peer_mac, 6) != 0) return;
    if (h->session != s_session) return;
    if (memcmp(h->dst, s_my_mac, 6) != 0) return;

    s_since_peer = 0;
    s_peer_rssi = rssi;

    // LIVE is a superset-confirmation of PROPOSE: a badge that already moved
    // on has, by doing so, agreed. Without this the two sides can deadlock
    // waiting for a confirmation each thinks it already gave — the same shape
    // as bbx_proto.h:161-164's idempotent-OPEN rule.
    if (h->state != MPG_STATE_PROPOSE && h->state != MPG_STATE_LIVE) return;

    // Both sides advertise the seat they computed. If they match, the
    // antisymmetry argument broke somewhere, and a visible "pairing failed" is
    // enormously better than two badges silently both playing White.
    if (h->seat == s_seat) {
        s_fail = MpFail::SeatClash;
        s_state = MpState::Over;
        ESP_LOGE(TAG, "seat clash: both computed %u", s_seat);
        return;
    }
    go_live();
}

void live_frame(const mpg_hdr_t *h, const uint8_t *body, size_t bl, int8_t rssi)
{
    if (memcmp(h->src, s_peer_mac, 6) != 0) return;

    // Our peer re-bumped: they rebooted, or backed out and want a new game.
    // Without this rule, re-pairing after one badge restarts needs BOTH users
    // to leave the app.
    if (h->type == MPG_TYPE_HELLO && (h->flags & MPG_FLAG_BUMPING)
        && h->session != s_session) {
        ESP_LOGI(TAG, "peer re-bumped; back to seeking");
        restart_seeking();
        return;
    }

    if (h->session != s_session) return;
    if (memcmp(h->dst, s_my_mac, 6) != 0) return;

    s_since_peer = 0;
    s_peer_rssi  = rssi;
    s_escalate   = kEscalateTicks;
    s_peer_flags = h->flags;

    if (h->type == MPG_TYPE_BYE) {
        s_fail = MpFail::PeerLeft;
        s_state = MpState::Over;
        return;
    }
    if (h->type != MPG_TYPE_STATE) return;

    if (h->game_no != s_game_no) {
        // They started the rematch first. Follow the number, but only if we
        // actually agreed to one — otherwise a stale frame from an older game
        // could drag us into a board we never accepted.
        if (h->game_no > s_game_no && (s_flags & MPG_FLAG_REMATCH))
            advance_game(h->game_no);
        return;
    }

    s_peer_ack_seq = h->ack_seq;
    s_peer_ack_sum = h->ack_sum;

    // A crashed peer's register repeats forever. Refusing to go backwards is
    // what makes that harmless rather than a way to rewind a live game.
    if (h->seq < s_peer_seq) return;
    s_peer_seq = h->seq;
    s_peer_sum = h->sum;

    // The same frame repeating is the NORMAL case, not an error — it is a
    // register, not a message. Only wake the game for something new.
    if (h->seq == s_delivered_seq && h->sum == s_delivered_sum) return;

    memcpy(&s_in_hdr, h, sizeof(s_in_hdr));
    memcpy(s_in_body, body, bl);
    s_in_body_len = bl;
    s_in_ready = true;
}

void handle(const uint8_t *d, size_t n, int8_t rssi)
{
    mpg_hdr_t h;
    memcpy(&h, d, sizeof(h));
    if (memcmp(h.src, s_my_mac, 6) == 0) return;          // our own echo

    const uint8_t *body = d + MPG_HDR_SIZE;
    size_t bl = h.body_len;
    if (bl > MPG_MAX_BODY || MPG_HDR_SIZE + bl != n) return;
    if (mpg_crc16(body, bl) != h.sum) return;
    if (h.game != s_cfg.game) return;

    switch (s_state) {
        case MpState::Seeking:  seek_frame(&h, body, bl, rssi); break;
        case MpState::Proposed: propose_frame(&h, rssi);        break;
        case MpState::Live:
        case MpState::Over:     live_frame(&h, body, bl, rssi); break;
        default: break;
    }
}

void drain()
{
    // One frame at a time: a local copy is 225 bytes of stack, where draining
    // all four at once would be 900 on a task with 7168 total. The mutex is
    // held only for the copy, never across processing.
    for (int i = 0; i < kMaxCandidates; ++i) {
        RxSlot slot;
        bool got = false;
        if (s_rx_mutex && xSemaphoreTake(s_rx_mutex, 0) == pdTRUE) {
            if (s_rx_count > 0) {
                slot = s_rx[0];
                for (int j = 1; j < s_rx_count; ++j) s_rx[j - 1] = s_rx[j];
                s_rx_count--;
                got = true;
            }
            xSemaphoreGive(s_rx_mutex);
        }
        if (!got) return;
        ButtonPair::Intent intent;
        if (ButtonPair::decode(slot.data, slot.len, intent)) {
            if (intent.kind == ButtonPair::Chess && intent.context == s_cfg.game
                && slot.rssi >= s_cfg.rssi_min && slot.rssi != 127
                && memcmp(intent.mac, s_my_mac, 6) != 0
                && s_button.active(esp_timer_get_time())
                && s_button.receive(slot.received_at, intent.mac, intent)) {
                s_button_peer = intent;
                s_button_rssi = slot.rssi;
            }
            continue;
        }
        if (!s_button.active(esp_timer_get_time())) handle(slot.data, slot.len, slot.rssi);
    }
}

// Runs on the NimBLE host task. Copy and get out: no LVGL, no allocation, and
// a try-lock only. Prefilter cheapest-and-most-discriminating first, because
// company ID 0xFFFF is shared and Badge Bump's BMP2 and Solana Scavenger Hunt's BBX1
// frames arrive here too.
void on_radio_frame(const uint8_t mac[6], int8_t rssi, const uint8_t *d, size_t n)
{
    (void)mac;   // NimBLE reports the advertiser address byte-reversed; the
                 // frame carries its own identity in hdr.src and we match that.
    ButtonPair::Intent intent;
    const bool button = ButtonPair::decode(d, n, intent);
    if (!button && (n < MPG_HDR_SIZE || n > MPG_MAX_FRAME)) return;
    if (!button) {
        if (d[0] != MPG_MAGIC_0 || d[1] != MPG_MAGIC_1
            || d[2] != MPG_MAGIC_2 || d[3] != MPG_MAGIC_3) return;
        if (d[4] != MPG_VERSION) return;
        // 127 is the "reading unavailable" sentinel, not an extremely strong
        // signal (hal_radio.h:100-103).
        if (rssi == 127) return;

        uint16_t body_len;
        memcpy(&body_len, d + offsetof(mpg_hdr_t, body_len), sizeof(body_len));
        if ((size_t)MPG_HDR_SIZE + body_len != n) return;
    }
    if (!s_rx_mutex || xSemaphoreTake(s_rx_mutex, 0) != pdTRUE) return;
    if (s_rx_count < kMaxCandidates) {
        RxSlot &s = s_rx[s_rx_count++];
        memcpy(s.data, d, n);
        s.len = (uint16_t)n;
        s.rssi = rssi;
        s.received_at = esp_timer_get_time();
    }
    xSemaphoreGive(s_rx_mutex);
}

void tick_seeking()
{
    if (s_button.active(esp_timer_get_time())) return;
    if (s_cfg.require_bump && detect_bump()) mp_session_note_bump();

    // Evaluated at the window's CLOSE, not on the first sighting: comparing
    // candidates fairly needs the whole window's worth, and 500 ms is
    // invisible next to the gesture itself.
    if (s_window_closed) pick_partner();
    else if (!s_cfg.require_bump && s_seek_ticks == kBenchSettleTicks) pick_partner();

    if (++s_seek_ticks > kSeekTimeoutTicks) {
        s_fail = MpFail::NoPartner;
        s_state = MpState::Over;    // drops to Resting; the 100% scan stops
        ESP_LOGI(TAG, "seek timeout");
    }
}

void tick_proposed()
{
    if (++s_propose_ticks < kProposeHoldTicks) return;
    if (s_propose_ticks <= s_propose_timeout) return;

    if (++s_propose_attempts >= kProposeRetries) {
        s_fail = MpFail::NoPartner;
        s_state = MpState::Over;
        return;
    }
    // Jittered in latch(). Two badges that disagreed and time out in lockstep
    // would re-roll in lockstep and can reach the same wrong answer again —
    // bbx_proto.h:205-206 learned this the expensive way.
    restart_seeking();
}

}  // namespace

// ---------------------------------------------------------------------------
// Public API

bool mp_session_begin(const MpConfig &cfg)
{
    s_cfg = cfg;
    if (!s_rx_mutex) s_rx_mutex = xSemaphoreCreateMutex();

    memset(s_cands, 0, sizeof(s_cands));
    s_rx_count = 0;
    s_tx_len = 0;
    s_in_ready = false;
    s_fail = MpFail::None;
    s_game_no = 0;
    s_my_turn = false;
    s_peer_rssi = 127;
    s_escalate = s_tier_hold = s_since_peer = s_idle_ticks = 0;
    s_bump_window = s_bumped_recent = 0;
    s_propose_attempts = 0;
    s_bump_detector.reset();
    s_window_closed = false;
    s_wake_lock = false;

    // Order matters. hal_radio.cpp:429 re-applies the payload after a retune
    // only when one has already been sent, so retuning an enabled radio that
    // has never sent leaves the instance advertising nothing. Setting the
    // timings first is explicitly safe (hal_radio.h:81-82) — they are
    // remembered and applied at enable.
    const DutyTimings &t = kDuty[(int)MpDuty::Pairing];
    hal_radio_set_adv_interval_ms(t.adv_min, t.adv_max);
    hal_radio_set_scan_duty_ms(t.scan_itvl, t.scan_win);
    s_duty = MpDuty::Pairing;

    if (hal_radio_enable() != ESP_OK) {
        s_state = MpState::Off;
        s_fail = MpFail::Radio;
        return false;
    }

    hal_radio_local_mac(s_my_mac);
    roll_nonce();                       // only now is esp_random() a true RNG
    restart_seeking();
    refresh_frame();

    hal_radio_set_recv_cb(on_radio_frame);
    hal_sleep_wake_lock(true);
    s_wake_lock = true;
    return true;
}

void mp_session_end()
{
    // FIRST. It is invoked from the NimBLE host task, and outliving this
    // module would be a use-after-free (app_sync.cpp:96-98).
    hal_radio_set_recv_cb(RadioRecvCb{});

    hal_radio_adv_pause();
    hal_radio_reset_timings();
    // Deliberately NOT hal_radio_disable(): it blocks up to 2 s, and on_exit()
    // runs under the LVGL lock. The registry disables it on the next switch.

    if (s_rx_mutex && xSemaphoreTake(s_rx_mutex, portMAX_DELAY) == pdTRUE) {
        s_rx_count = 0;
        xSemaphoreGive(s_rx_mutex);
    }
    if (s_wake_lock) { hal_sleep_wake_lock(false); s_wake_lock = false; }
    s_button = {};
    s_state = MpState::Off;
    s_tx_len = 0;
    s_in_ready = false;
}

void mp_session_tick()
{
    if (s_state == MpState::Off) return;

    s_window_closed = false;
    if (s_bump_window > 0 && --s_bump_window == 0) s_window_closed = true;
    if (s_bumped_recent > 0) s_bumped_recent--;
    if (s_escalate > 0) s_escalate--;
    if (s_tier_hold > 0) s_tier_hold--;
    if (s_bye_ticks > 0) s_bye_ticks--;
    if (s_since_peer < kPeerLostTicks * 2) s_since_peer++;
    if (s_idle_ticks < kIdleAbandonTicks) s_idle_ticks++;
    for (auto &c : s_cands)
        if (c.in_use && ++c.age > kCandExpireTicks) c.in_use = false;

    drain();
    if (s_button.started() && !s_button.active(esp_timer_get_time())) {
        if (s_button.confirmed()) latch(s_button_peer.mac, s_button_peer.nonce, s_button_rssi);
        s_button = {};
        s_tx_len = 0;
        hal_radio_adv_resume();
    }

    switch (s_state) {
        case MpState::Seeking:  tick_seeking();  break;
        case MpState::Proposed: tick_proposed(); break;
        case MpState::Live:
            if (s_since_peer > kPeerLostTicks) {
                s_fail = MpFail::PeerGone;
                s_state = MpState::Lost;
            }
            break;
        case MpState::Over:
            if ((s_flags & MPG_FLAG_REMATCH) && (s_peer_flags & MPG_FLAG_REMATCH))
                advance_game((uint8_t)(s_game_no + 1));
            break;
        default: break;
    }

    refresh_frame();
    apply_duty(compute_duty());
    update_wake_lock();
}

void mp_session_note_button()
{
    if (s_state == MpState::Off || s_state == MpState::Live || s_state == MpState::Proposed
        || s_button.active(esp_timer_get_time())) return;
    restart_seeking();
    s_fail = MpFail::None;
    s_bump_window = s_bumped_recent = 0;
    s_button.begin(esp_timer_get_time(), s_nonce);
    s_tier_hold = 0;
    apply_duty(MpDuty::Pairing);
    refresh_frame();
}

void mp_session_note_bump()
{
    s_bump_window   = kBumpWindowTicks;
    s_bumped_recent = kBumpRecentTicks;
    s_idle_ticks    = 0;
}

void mp_session_note_activity() { s_idle_ticks = 0; }
void mp_session_set_my_turn(bool mine) { s_my_turn = mine; }

MpState  mp_session_state()    { return s_state; }
MpFail   mp_session_fail()     { return s_fail; }
MpDuty   mp_session_duty()     { return s_duty; }
uint32_t mp_session_id()       { return s_session; }
uint8_t  mp_session_seat()     { return s_seat; }
uint8_t  mp_session_game_no()  { return s_game_no; }
int      mp_session_peer_rssi(){ return s_peer_rssi; }
int      mp_session_ticks_since_peer() { return s_since_peer; }
bool     mp_session_peer_flag(uint8_t f) { return (s_peer_flags & f) != 0; }

bool mp_session_peer_mac(uint8_t out[6])
{
    static const uint8_t zero[6] = {};
    if (memcmp(s_peer_mac, zero, 6) == 0) return false;
    memcpy(out, s_peer_mac, 6);
    return true;
}

int mp_session_candidates()
{
    int n = 0;
    for (auto &c : s_cands) if (c.in_use) n++;
    return n;
}

bool mp_session_publish(const void *body, size_t len, uint16_t seq)
{
    if (len > MPG_MAX_BODY) return false;
    uint16_t sum = mpg_crc16(body, len);
    // Idempotent, so the game can call this every tick and never think about
    // when the radio should be touched.
    if (seq == s_seq && sum == s_sum && len == s_body_len) return true;
    memcpy(s_body, body, len);
    s_body_len = (uint16_t)len;
    s_seq = seq;
    s_sum = sum;
    return true;
}

const mpg_hdr_t *mp_session_inbound(const uint8_t **body, size_t *len)
{
    if (!s_in_ready) return nullptr;
    if (body) *body = s_in_body;
    if (len) *len = s_in_body_len;
    return &s_in_hdr;
}

void mp_session_take_inbound()
{
    s_delivered_seq = s_in_hdr.seq;
    s_delivered_sum = s_in_hdr.sum;
    s_in_ready = false;
}

void mp_session_ack(uint16_t peer_seq, uint16_t peer_sum)
{
    s_ack_seq = peer_seq;
    s_ack_sum = peer_sum;
}

// The only honest definition of "delivered" on a broadcast register: the peer
// is echoing our content back at us. Having sent it proves nothing.
bool mp_session_published_acked()
{
    if (s_body_len == 0) return true;
    return s_peer_ack_seq == s_seq && s_peer_ack_sum == s_sum;
}

void mp_session_offer_draw(bool on)
{
    if (on) s_flags |= MPG_FLAG_DRAW_OFFER; else s_flags &= (uint8_t)~MPG_FLAG_DRAW_OFFER;
}

void mp_session_offer_rematch(bool on)
{
    if (on) s_flags |= MPG_FLAG_REMATCH; else s_flags &= (uint8_t)~MPG_FLAG_REMATCH;
}

void mp_session_resign()
{
    // URGENT is how this reaches a peer that is scanning at 3.3% because it
    // believes it is its turn to think. Without it a resignation lands
    // whenever they next happen to listen.
    s_flags |= MPG_FLAG_RESIGN | MPG_FLAG_URGENT;
    s_state = MpState::Over;
}

void mp_session_declare_over()
{
    if (s_state == MpState::Live) s_state = MpState::Over;
}

void mp_session_say_bye()
{
    s_state = MpState::Ended;
    s_flags |= MPG_FLAG_URGENT;
    s_bye_ticks = kByeHoldTicks;
}

bool mp_session_bye_done() { return s_state != MpState::Ended || s_bye_ticks == 0; }

size_t mp_session_debug(char *out, size_t out_sz)
{
    uint8_t p[6] = {};
    bool have_peer = mp_session_peer_mac(p);
    const DutyTimings &t = kDuty[(int)s_duty];
    return (size_t)snprintf(out, out_sz,
        "mp_state=%s mp_fail=%s duty=%s session=%08lx seat=%u game_no=%u\n"
        "peer=%s%02x%02x%02x%02x%02x%02x rssi=%d since_peer=%d cands=%d\n"
        "seq=%u sum=%04x ack=%u/%04x peer_seq=%u peer_sum=%04x acked=%d\n"
        "adv=%lu-%lu scan=%lu/%lu flags=%02x peer_flags=%02x "
        "bump=%d idle=%d wake=%d\n",
        kStateName[(int)s_state], kFailName[(int)s_fail], kDutyName[(int)s_duty],
        (unsigned long)s_session, s_seat, s_game_no,
        have_peer ? "" : "none:", p[0], p[1], p[2], p[3], p[4], p[5],
        s_peer_rssi, s_since_peer, mp_session_candidates(),
        s_seq, s_sum, s_ack_seq, s_ack_sum, s_peer_seq, s_peer_sum,
        mp_session_published_acked() ? 1 : 0,
        (unsigned long)t.adv_min, (unsigned long)t.adv_max,
        (unsigned long)t.scan_itvl, (unsigned long)t.scan_win,
        s_flags, s_peer_flags, s_bump_window, s_idle_ticks, s_wake_lock ? 1 : 0);
}
