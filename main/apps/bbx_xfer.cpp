#include "bbx_xfer.h"

#include "hal_radio.h"

#include "esp_random.h"
#include "esp_log.h"
#include "esp_rom_crc.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <cstdio>
#include <cstring>

namespace {

// --- timings ---------------------------------------------------------------
// All tick counts are in 20 ms registry ticks.

// A burst shorter than the advertising interval emits roughly one advertising
// event, which a peer scanning at partial duty usually misses. 30 ms is well
// clear of the 20 ms BLE floor for non-connectable extended advertising.
constexpr uint32_t kAdvXferMinMs = 30;
constexpr uint32_t kAdvXferMaxMs = 35;

// 100% duty. At hal_radio's default 20% the expected time to catch a station's
// advertisement is ~1 s, which would dominate every round trip.
constexpr uint32_t kScanFastMs = 60;

// 3 ticks = 60 ms per frame, so ~2 advertising events carry each one.
// Throughput is 232 B / 60 ms = 3.9 KB/s: a 20 KB snapshot sweeps in ~5.3 s.
constexpr int kDataDwellTicks = 3;

constexpr int kOpenHoldTicks      = 20;    // 400 ms before expecting a GRANT
constexpr int kOpenTimeoutTicks   = 250;   // 5 s per OPEN attempt
constexpr int kSweepDoneHoldTicks = 30;    // 600 ms before expecting a STAT
constexpr int kStatTimeoutTicks   = 150;   // 3 s per STAT wait
constexpr int kDoneTimeoutTicks   = 1000;  // 20 s: the station is doing TLS
constexpr int kAckHoldTicks       = 50;    // 1 s of ACK before we stop

constexpr int kMaxOpenRetries   = 3;
constexpr int kMaxBackoffRounds = 4;
constexpr int kMaxRepairRounds  = 12;
constexpr int kMaxStatRetries   = 3;
constexpr int kNoProgressRounds = 2;

// A station is "seen" if a beacon arrived within this long.
constexpr int64_t kBeaconFreshUs = 4 * 1000 * 1000;

// The spec's sequence pauses advertising while waiting for a STAT. We do not,
// and the reason is that our own held OPEN|SWEEP_DONE *is* the mechanism that
// re-asks for one: pause, and a lost STAT has no self-healing path and needs a
// separate re-request timer plus a resume, which is more states and more ways
// to deadlock. Self-interference costs a few percent of receive time at a
// 30 ms interval. Left as a constant so it is a one-line bench experiment.
constexpr bool kPauseAdvInAwaitStat = false;

// --- state -----------------------------------------------------------------

struct RxSlot {
    bool     ready;
    bool     for_me;   // a GRANT addressed elsewhere still matters: see below
    uint8_t  type, status, flags, sess, seq, total;
    uint16_t req_id;
    uint8_t  body[BBX_MAX_BODY];
    uint8_t  body_len;
};

XferState s_state = XferState::Idle;
XferFail  s_fail  = XferFail::None;
XferStats s_stats;

const uint8_t *s_data;
FILE           *s_file;
size_t         s_len;
uint8_t *s_response;
size_t s_response_cap, s_response_len;
uint16_t s_response_wire_len;
uint8_t s_response_next, s_response_frames;
uint8_t        s_kind, s_enc;
uint32_t       s_crc;
uint16_t       s_chunk;
uint8_t        s_frames;
uint16_t       s_req_id;
uint8_t        s_sess;
uint8_t        s_my_mac[6];
uint8_t        s_server_status;

uint8_t s_bitmap[BBX_XFER_BITMAP_BYTES];   // what the station says it has
uint8_t s_seq;              // next frame to send in the current sweep
int     s_dwell;
int     s_ticks;            // time in the current state
int     s_open_retries;
int     s_backoff_rounds;
int     s_backoff_ticks;
int     s_repair_rounds;
int     s_stat_retries;
int     s_last_recv_count;
int     s_no_progress;
int     s_ack_ticks;
int64_t s_start_us;

SemaphoreHandle_t s_mutex;
RxSlot            s_rx;

// Station discovery, updated from the NimBLE task.
char    s_beacon_id[BBX_MAX_BOX_ID + 1];
int8_t  s_beacon_rssi;
int64_t s_beacon_us;

int64_t now_us() { return esp_timer_get_time(); }

void ensure_mutex()
{
    if (!s_mutex) s_mutex = xSemaphoreCreateMutex();
}

// --- frame construction ----------------------------------------------------

void fill_header(bbx_hdr_t *h, uint8_t type, uint8_t flags, uint8_t seq)
{
    memset(h, 0, sizeof(*h));
    bbx_set_magic(h);
    h->type   = type;
    h->flags  = flags;
    h->req_id = s_req_id;
    h->sess   = s_sess;
    h->seq    = seq;
    h->total  = s_frames;
    memcpy(h->badge, s_my_mac, 6);
}

void send_open(uint8_t flags)
{
    uint8_t frame[BBX_HDR_SIZE + sizeof(bbx_xfer_open_t)];
    bbx_hdr_t h;
    fill_header(&h, BBX_TYPE_XFER_OPEN, flags, 0);

    bbx_xfer_open_t o = {};
    o.kind         = s_kind;
    o.enc          = s_enc;
    o.total_len    = (uint16_t)s_len;
    o.crc32        = s_crc;
    o.chunk_len    = s_chunk;
    o.total_frames = s_frames;
    o.want_downlink = s_response != nullptr ? 1 : 0;

    memcpy(frame, &h, sizeof(h));
    memcpy(frame + sizeof(h), &o, sizeof(o));
    esp_err_t err = hal_radio_send(frame, sizeof(frame));
    if (err != ESP_OK) ESP_LOGW("bbx_xfer", "OPEN send failed: %d", err);
}

void send_data(uint8_t seq)
{
    uint16_t n = bbx_chunk_size((uint16_t)s_len, s_chunk, s_frames, seq);
    if (n == 0) return;

    uint8_t frame[BBX_MAX_FRAME];
    bbx_hdr_t h;
    fill_header(&h, BBX_TYPE_DATA, 0, seq);
    memcpy(frame, &h, sizeof(h));
    if (s_file) {
        // One frame of flash per send; a short read just skips this frame
        // this sweep and the repair round picks it up.
        if (fseek(s_file, (long)((size_t)seq * s_chunk), SEEK_SET) != 0) return;
        if (fread(frame + sizeof(h), 1, n, s_file) != n) return;
    } else {
        memcpy(frame + sizeof(h), s_data + (size_t)seq * s_chunk, n);
    }
    hal_radio_send(frame, sizeof(h) + n);
    s_stats.frames_sent++;
}

void send_ack()
{
    bbx_hdr_t h;
    fill_header(&h, BBX_TYPE_ACK, 0, 0);
    hal_radio_send(&h, sizeof(h));
    s_ack_ticks = kAckHoldTicks;
}

// --- transitions -----------------------------------------------------------

void finish(XferState st, XferFail why)
{
    s_state = st;
    s_fail  = why;
    s_stats.elapsed_ms = (uint32_t)((now_us() - s_start_us) / 1000);
    // Deliberately not hal_radio_disable(): that blocks up to 2 s waiting for
    // the NimBLE host task to unwind, and doing it from a tick stalls the LVGL
    // timer callback that holds the lock. The registry force-disables the radio
    // on the next app switch anyway.
    // A failed transfer must not leave OPEN repeating: after the station's
    // lease expires, that stale request would immediately claim it again.
    // A final ACK is the exception; give it its brief hold on either verdict.
    if (s_ack_ticks == 0) hal_radio_adv_pause();
    hal_radio_reset_timings();
    if (s_ack_ticks > 0) hal_radio_adv_resume();
}

void enter_opening()
{
    s_state = XferState::Opening;
    s_ticks = 0;
    // A fresh req_id per attempt, so a station still holding the previous
    // attempt's state doesn't answer this one from its idempotent path.
    do { s_req_id = (uint16_t)(esp_random() & 0xFFFF); } while (s_req_id == 0);
    s_sess = 0;
    hal_radio_adv_resume();
    send_open(0);
}

void enter_backoff(uint8_t backoff_ds, uint8_t queue_ahead)
{
    (void)queue_ahead;
    s_state = XferState::Backoff;
    s_ticks = 0;
    s_backoff_rounds++;
    // Jitter is not optional: without it two badges denied in the same instant
    // retry in lockstep forever.
    int base_ticks   = (backoff_ds ? backoff_ds : 15) * 100 / 20;
    int jitter_ticks = (int)(esp_random() % 75);   // 0..1.5 s
    s_backoff_ticks  = base_ticks + jitter_ticks;
    hal_radio_adv_pause();
}

void enter_sweeping()
{
    s_state = XferState::Sweeping;
    s_seq   = 0;
    s_dwell = 0;
    s_ticks = 0;
    memset(s_bitmap, 0, sizeof(s_bitmap));
}

void enter_await_stat()
{
    s_state = XferState::AwaitStat;
    s_ticks = 0;
    send_open(BBX_FLAG_SWEEP_DONE);
    if (kPauseAdvInAwaitStat) hal_radio_adv_pause();
}

// Next frame the station hasn't got, at or after `from`. s_frames if none.
uint8_t next_missing(uint8_t from)
{
    for (uint8_t i = from; i < s_frames; i++) {
        if (!bbx_bitmap_test(s_bitmap, i)) return i;
    }
    return s_frames;
}

void enter_repairing()
{
    s_state = XferState::Repairing;
    s_ticks = 0;
    s_dwell = 0;
    s_repair_rounds++;
    s_stats.repair_rounds = (uint8_t)s_repair_rounds;
    s_seq = next_missing(0);
}

// --- receive ---------------------------------------------------------------

void handle_beacon(int8_t rssi, const uint8_t *body, size_t body_len)
{
    size_t n = body_len < BBX_MAX_BOX_ID ? body_len : BBX_MAX_BOX_ID;
    memcpy(s_beacon_id, body, n);
    s_beacon_id[n] = '\0';
    // 127 is the scan report's "no reading" sentinel; as a raw number it would
    // look like an impossibly strong signal.
    if (rssi != 127) s_beacon_rssi = rssi;
    s_beacon_us = now_us();
}

}  // namespace

// ---------------------------------------------------------------------------

void bbx_xfer_on_frame(const uint8_t * /*mac*/, int8_t rssi,
                       const uint8_t *data, size_t len)
{
    if (len < BBX_HDR_SIZE) return;

    bbx_hdr_t h;
    memcpy(&h, data, sizeof(h));
    if (!bbx_magic_ok(&h)) return;

    const uint8_t *body = data + BBX_HDR_SIZE;
    size_t body_len = len - BBX_HDR_SIZE;

    if (h.type == BBX_TYPE_SYNC_BEACON) {
        handle_beacon(rssi, body, body_len);
        return;
    }
    if (h.type != BBX_TYPE_XFER_GRANT && h.type != BBX_TYPE_XFER_STAT
        && h.type != BBX_TYPE_XFER_DONE && h.type != BBX_TYPE_PROFILE_DATA) {
        return;
    }

    // Broadcast has no destination, so the frame carries ours. Match on that
    // rather than the advertiser address, which NimBLE reports byte-reversed
    // and which is easy to get backwards.
    //
    // A GRANT addressed to someone ELSE is not noise — it is how we learn the
    // station is taken, so it is passed through to the tick.
    bool for_me = (memcmp(h.badge, s_my_mac, 6) == 0);
    if (!for_me && h.type != BBX_TYPE_XFER_GRANT) return;

    if (!s_mutex) return;
    if (xSemaphoreTake(s_mutex, 0) != pdTRUE) return;   // busy; the frame repeats
    if (!s_rx.ready) {
        s_rx.type     = h.type;
        s_rx.for_me   = for_me;
        s_rx.status   = h.status;
        s_rx.flags    = h.flags;
        s_rx.sess     = h.sess;
        s_rx.req_id   = h.req_id;
        s_rx.seq = h.seq;
        s_rx.total = h.total;
        s_rx.body_len = (uint8_t)(body_len > BBX_MAX_BODY ? BBX_MAX_BODY : body_len);
        memcpy(s_rx.body, body, s_rx.body_len);
        s_rx.ready    = true;
    }
    xSemaphoreGive(s_mutex);
}

bool bbx_xfer_start(const XferConfig &cfg)
{
    ensure_mutex();
    if (s_state != XferState::Idle && s_state != XferState::Done
        && s_state != XferState::Failed) {
        return false;
    }
    if ((!cfg.data && !cfg.file) || (cfg.data && cfg.file)) return false;
    if (cfg.len == 0 || cfg.len > BBX_XFER_MAX_LEN) return false;

    if (cfg.response != nullptr && cfg.response_cap < BBX_PROFILE_MAX_LEN + BBX_PROFILE_PREFIX_LEN + 1) return false;
    s_response = cfg.response;
    s_response_cap = cfg.response_cap;
    s_response_len = 0;
    s_response_wire_len = 0;
    s_response_next = s_response_frames = 0;
    s_data  = cfg.data;
    s_file  = cfg.file;
    s_len   = cfg.len;
    s_kind  = cfg.kind;
    s_enc   = cfg.enc;
    s_chunk = BBX_MAX_BODY;
    s_frames = (uint8_t)((s_len + s_chunk - 1) / s_chunk);
    // Seed 0 — the zlib-compatible convention, matching the station and server.
    s_crc = s_file ? cfg.crc : esp_rom_crc32_le(0, s_data, s_len);

    hal_radio_local_mac(s_my_mac);

    memset(&s_stats, 0, sizeof(s_stats));
    s_stats.total_frames = s_frames;
    s_stats.station_rssi = s_beacon_rssi;
    snprintf(s_stats.station_id, sizeof(s_stats.station_id), "%s", s_beacon_id);

    s_fail = XferFail::None;
    s_server_status  = 0;
    s_open_retries   = 0;
    s_backoff_rounds = 0;
    s_repair_rounds  = 0;
    s_stat_retries   = 0;
    s_last_recv_count = -1;
    s_no_progress    = 0;
    s_ack_ticks      = 0;
    s_start_us       = now_us();
    memset(&s_rx, 0, sizeof(s_rx));

    // Both of these matter more than anything else in this file: a 200 ms
    // interval makes each frame's dwell longer than the whole transfer budget,
    // and a 20% scan duty misses four out of five station replies.
    if (hal_radio_set_adv_interval_ms(kAdvXferMinMs, kAdvXferMaxMs) != ESP_OK) {
        finish(XferState::Failed, XferFail::Radio);
        return false;
    }
    hal_radio_set_scan_duty_ms(kScanFastMs, kScanFastMs);

    enter_opening();
    return true;
}

void bbx_xfer_abort()
{
    if (s_state == XferState::Idle) return;
    s_state = XferState::Idle;
    s_fail  = XferFail::None;
    s_data  = nullptr;
    s_file  = nullptr;
    s_ack_ticks = 0;
    hal_radio_adv_pause();
    hal_radio_reset_timings();
}

void bbx_xfer_tick()
{
    if (s_ack_ticks > 0 && --s_ack_ticks == 0) hal_radio_adv_pause();

    // Drain the receive slot first, so this tick acts on the newest frame.
    RxSlot rx = {};
    if (s_mutex && xSemaphoreTake(s_mutex, 0) == pdTRUE) {
        if (s_rx.ready) {
            rx = s_rx;
            s_rx.ready = false;
        }
        xSemaphoreGive(s_mutex);
    }

    if (rx.ready && rx.for_me && rx.type == BBX_TYPE_PROFILE_DATA
        && s_response != nullptr && rx.sess == s_sess && rx.req_id == s_req_id
        && (s_state == XferState::AwaitStat || s_state == XferState::AwaitDone)) {
        if (!s_response_next && rx.seq == 0) {
            if (rx.body_len < BBX_PROFILE_PREFIX_LEN) return;
            uint16_t bytes; memcpy(&bytes, rx.body, 2);
            if (!bytes || bytes > BBX_PROFILE_MAX_LEN) { finish(XferState::Failed, XferFail::TooBig); return; }
            s_response_wire_len = bytes + BBX_PROFILE_PREFIX_LEN;
            s_response_frames = (s_response_wire_len + BBX_MAX_BODY - 1) / BBX_MAX_BODY;
        }
        if (!s_response_wire_len || rx.total != s_response_frames || rx.seq >= rx.total || rx.seq > s_response_next) return;
        const size_t offset = (size_t)rx.seq * BBX_MAX_BODY;
        size_t bytes = s_response_wire_len - offset;
        if (bytes > BBX_MAX_BODY) bytes = BBX_MAX_BODY;
        if (rx.body_len != bytes || offset + bytes >= s_response_cap) return;
        if (rx.seq == s_response_next) {
            memcpy(s_response + offset, rx.body, bytes);
            ++s_response_next;
        } else if (memcmp(s_response + offset, rx.body, bytes) != 0) return;
        if (s_response_next == s_response_frames) {
            uint32_t crc; memcpy(&crc, s_response + 2, 4);
            const size_t len = s_response_wire_len - BBX_PROFILE_PREFIX_LEN;
            if (esp_rom_crc32_le(0, s_response + BBX_PROFILE_PREFIX_LEN, len) != crc) {
                finish(XferState::Failed, XferFail::Crc); return;
            }
            s_response[s_response_wire_len] = 0;
            s_response_len = len;
        }
        if (kPauseAdvInAwaitStat) hal_radio_adv_resume();
        bbx_hdr_t ack; fill_header(&ack, BBX_TYPE_PROFILE_ACK, 0, rx.seq);
        hal_radio_send(&ack, sizeof(ack));
        s_state = XferState::AwaitDone;
        s_ticks = 0;
        return;
    }

    switch (s_state) {
        case XferState::Idle:
        case XferState::Done:
        case XferState::Failed:
            return;

        case XferState::Opening:
            if (rx.ready && rx.type == BBX_TYPE_XFER_GRANT) {
                if (!rx.for_me) {
                    // Granted to another badge: the station is taken.
                    enter_backoff(0, 0);
                    return;
                }
                if (rx.status == BBX_ST_OK) {
                    s_sess = rx.sess;
                    if (rx.body_len >= sizeof(bbx_xfer_grant_t)) {
                        bbx_xfer_grant_t g;
                        memcpy(&g, rx.body, sizeof(g));
                        // The station may shrink the chunk; honour it, but
                        // never grow past what one frame can hold.
                        if (g.chunk_len > 0 && g.chunk_len <= BBX_MAX_BODY
                            && g.chunk_len != s_chunk) {
                            s_chunk  = g.chunk_len;
                            s_frames = (uint8_t)((s_len + s_chunk - 1) / s_chunk);
                            s_stats.total_frames = s_frames;
                        }
                    }
                    enter_sweeping();
                    return;
                }
                if (rx.status == BBX_ST_XFER_BUSY) {
                    bbx_xfer_grant_t g = {};
                    if (rx.body_len >= sizeof(g)) memcpy(&g, rx.body, sizeof(g));
                    enter_backoff(g.backoff_ds, g.queue_ahead);
                    return;
                }
                if (rx.status == BBX_ST_TOO_BIG) {
                    finish(XferState::Failed, XferFail::TooBig);
                    return;
                }
                finish(XferState::Failed, XferFail::Lost);
                return;
            }
            if (++s_ticks >= kOpenTimeoutTicks) {
                if (++s_open_retries >= kMaxOpenRetries) {
                    finish(XferState::Failed, XferFail::NoStation);
                    return;
                }
                enter_opening();
            } else if (s_ticks == kOpenHoldTicks) {
                // Re-assert once; the register may have been overwritten by
                // another app's traffic between enable and now.
                send_open(0);
            }
            return;

        case XferState::Backoff:
            if (--s_backoff_ticks <= 0) {
                if (s_backoff_rounds > kMaxBackoffRounds) {
                    finish(XferState::Failed, XferFail::StationBusy);
                    return;
                }
                s_open_retries = 0;
                enter_opening();
            }
            return;

        case XferState::Sweeping:
            if (++s_dwell < kDataDwellTicks) return;
            s_dwell = 0;
            if (s_seq < s_frames) {
                send_data(s_seq++);
                return;
            }
            enter_await_stat();
            return;

        case XferState::AwaitStat:
            s_ticks++;
            // A STAT is only meaningful once the station has had time to see
            // this sweep's SWEEP_DONE and refresh its register. Before that,
            // what is being advertised is the bitmap from BEFORE the repair
            // sweep we just finished — acting on it makes an unchanged
            // recv_count look like "no progress" and fails the transfer as
            // Stalled while it is in fact working. The peer's register is a
            // value that persists, not a message that is consumed, so a
            // settling window is the only thing that makes it fresh.
            if (s_ticks >= kSweepDoneHoldTicks
                && rx.ready && rx.for_me && rx.type == BBX_TYPE_XFER_STAT
                && rx.sess == s_sess && rx.body_len >= sizeof(bbx_xfer_stat_t)) {
                bbx_xfer_stat_t st;
                memcpy(&st, rx.body, sizeof(st));
                memcpy(s_bitmap, st.bitmap, sizeof(s_bitmap));
                s_stats.recv_count = st.recv_count;
                s_stat_retries = 0;

                if (st.recv_count >= s_frames) {
                    s_state = XferState::AwaitDone;
                    s_ticks = 0;
                    if (kPauseAdvInAwaitStat) hal_radio_adv_resume();
                    return;
                }
                // Two rounds that move nothing means the link is too weak to
                // finish; more repair sweeps would just burn the user's time.
                if ((int)st.recv_count == s_last_recv_count) {
                    if (++s_no_progress >= kNoProgressRounds) {
                        finish(XferState::Failed, XferFail::Stalled);
                        return;
                    }
                } else {
                    s_no_progress = 0;
                }
                s_last_recv_count = st.recv_count;

                if (s_repair_rounds >= kMaxRepairRounds) {
                    finish(XferState::Failed, XferFail::Stalled);
                    return;
                }
                if (kPauseAdvInAwaitStat) hal_radio_adv_resume();
                enter_repairing();
                return;
            }
            if (rx.ready && rx.for_me && rx.type == BBX_TYPE_XFER_DONE
                && rx.sess == s_sess) {
                // The station can jump straight to DONE, e.g. on a CRC failure.
                if (s_response != nullptr && rx.status == BBX_ST_OK && !s_response_len) rx.status = BBX_ST_TRANSPORT;
                s_server_status = rx.status;
                send_ack();
                if (rx.status == BBX_ST_OK) finish(XferState::Done, XferFail::None);
                else if (rx.status == BBX_ST_CRC) finish(XferState::Failed, XferFail::Crc);
                else finish(XferState::Failed, XferFail::Server);
                return;
            }
            if (s_ticks >= kStatTimeoutTicks) {
                if (++s_stat_retries >= kMaxStatRetries) {
                    finish(XferState::Failed, XferFail::Lost);
                    return;
                }
                enter_await_stat();
            } else if (s_ticks == kSweepDoneHoldTicks) {
                // Re-assert in case the station never saw the first one.
                send_open(BBX_FLAG_SWEEP_DONE);
            }
            return;

        case XferState::Repairing:
            if (++s_dwell < kDataDwellTicks) return;
            s_dwell = 0;
            if (s_seq < s_frames) {
                send_data(s_seq);
                s_seq = next_missing((uint8_t)(s_seq + 1));
                return;
            }
            enter_await_stat();
            return;

        case XferState::AwaitDone:
            if (rx.ready && rx.for_me && rx.type == BBX_TYPE_XFER_DONE
                && rx.sess == s_sess) {
                if (s_response != nullptr && rx.status == BBX_ST_OK && !s_response_len) rx.status = BBX_ST_TRANSPORT;
                s_server_status = rx.status;
                // ACK either way: it frees the station's slot for the next
                // badge in the queue whether or not the upload worked.
                send_ack();
                switch (rx.status) {
                    case BBX_ST_OK:      finish(XferState::Done, XferFail::None); break;
                    case BBX_ST_CRC:     finish(XferState::Failed, XferFail::Crc); break;
                    case BBX_ST_TOO_BIG: finish(XferState::Failed, XferFail::TooBig); break;
                    default:             finish(XferState::Failed, XferFail::Server); break;
                }
                return;
            }
            if (++s_ticks >= kDoneTimeoutTicks) {
                finish(XferState::Failed, XferFail::Lost);
            }
            return;
    }
}

XferState bbx_xfer_state() { return s_state; }
XferFail  bbx_xfer_fail()  { return s_fail; }
uint8_t   bbx_xfer_server_status() { return s_server_status; }
const XferStats *bbx_xfer_stats() { return &s_stats; }

int bbx_xfer_progress_pct()
{
    if (s_frames == 0) return 0;
    switch (s_state) {
        case XferState::Idle:
        case XferState::Opening:
        case XferState::Backoff:
            return 0;
        case XferState::Sweeping:
            // The first sweep is nearly all of the wall time, so it owns most
            // of the bar; repairs and the upload share the rest.
            return (int)((uint32_t)s_seq * 80 / s_frames);
        case XferState::AwaitStat:
        case XferState::Repairing:
            return 80 + (int)((uint32_t)s_stats.recv_count * 15 / s_frames);
        case XferState::AwaitDone:
            return 97;
        case XferState::Done:
            return 100;
        case XferState::Failed:
            return 0;
    }
    return 0;
}

bool bbx_xfer_station_seen(char *out_id, size_t out_sz, int *rssi_out)
{
    if (s_beacon_us == 0) return false;
    if (now_us() - s_beacon_us > kBeaconFreshUs) return false;
    if (out_id && out_sz) snprintf(out_id, out_sz, "%s", s_beacon_id);
    if (rssi_out) *rssi_out = s_beacon_rssi;
    return true;
}

void bbx_xfer_reset_discovery()
{
    s_beacon_us = 0;
    s_beacon_rssi = 127;
    s_beacon_id[0] = '\0';
}

const char *bbx_xfer_profile(size_t *len)
{
    if (s_state != XferState::Done || s_response == nullptr || !s_response_len) return nullptr;
    if (len) *len = s_response_len;
    return (const char *)(s_response + BBX_PROFILE_PREFIX_LEN);
}
