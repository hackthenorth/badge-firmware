#include "share_xfer.h"
#include "esp_log.h"
#include "app_share_proto.h"
#include "share_bundle.h"

#include "hal_radio.h"

#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cstring>

namespace {

// Retuned radio while sharing: bursts shorter than the default 200 ms adv
// interval would emit ~one advertising event and a peer at partial scan duty
// would miss it. Values match Connect's bump view (app_connect.cpp).
// The receiver skips the aggressive tune while idly listening: the sender's
// OFFER is a persistent register re-held every 200 ms, so the default 20%
// scan duty still catches it within ~1 s at a fraction of the draw, and our
// own advertising stays paused until the user accepts. The aggressive tune
// applies from acceptance on, where the 60 ms DATA dwell assumes 100% scan.
constexpr uint32_t kBurstAdvMs = 30;
constexpr uint32_t kScanMs = 100;   // interval == window == 100% duty
constexpr uint32_t kIdleScanIntervalMs = 150;
constexpr uint32_t kIdleScanWindowMs = 30;   // 20% duty, the hal_radio default

constexpr int kRssiMin = -60;       // contact range at this badge's TX power

// DATA dwell: hal_radio_send is a register write, so each frame must be held
// long enough that an advertising event carries it AND the peer's scan
// catches it. At 30 ms adv + 100% scan duty, 3 ticks (60 ms) is ~2 events.
constexpr int kDwellTicks = 3;

// Windows, in 20 ms ticks.
constexpr int kAcceptBurstTicks = 150;     // 3 s: ACCEPT burst after acceptance
constexpr int kOfferLatchTicks = 1500;     // 30 s: latched offer lifetime
constexpr int kDoneHoldTicks = 50;         // 1 s: hold DONE so peer stops
constexpr int kStallWindowTicks = 500;     // 10 s without progress -> fail
constexpr int kSweepCapTicks = 3000;       // 60 s absolute sweep/repair cap

constexpr size_t kRxQueueMax = 4;

struct Incoming {
    uint8_t  mac[6];
    int8_t   rssi;
    uint16_t len;
    uint8_t  data[SHARE_MAX_FRAME];
};

SemaphoreHandle_t s_rx_mutex = nullptr;
Incoming s_rx_queue[4];
size_t s_rx_count = 0;

ShareRole s_role = ShareRole::Idle;
ShareState s_state = ShareState::Idle;
ShareFail s_fail = ShareFail::None;
uint8_t s_offer_mac[6] = {};

// One descriptor, no whole-app allocation on either end.
int s_bundle_fd = -1;
char s_rx_path[128] = {};
size_t s_verify_pos = 0;
uint32_t s_verify_crc = 0;
bool s_peer_pinned = false;
uint8_t s_peer_mac[6] = {};

// Sender file (caller-owned) + OFFER fields.
size_t s_tx_len = 0;
uint32_t s_tx_crc = 0;   // hashed once at start; re-hashing per OFFER re-hold
                         // would cost milliseconds of LVGL time each round
uint16_t s_req_id = 0;
uint8_t s_total_frames = 0;
char s_tx_slug[SHARE_MAX_SLUG + 1] = {};
char s_tx_name[SHARE_MAX_NAME + 1] = {};
char s_tx_ver[SHARE_MAX_VER + 1] = {};

// Receiver.
size_t s_rx_len = 0;
uint32_t s_rx_crc = 0;
uint8_t s_rx_total = 0;
uint8_t s_rx_bitmap[SHARE_BITMAP_BYTES] = {};
char s_rx_slug[SHARE_MAX_SLUG + 1] = {};
char s_rx_name[SHARE_MAX_NAME + 1] = {};
char s_rx_ver[SHARE_MAX_VER + 1] = {};
char s_my_name[SHARE_MAX_PEER + 1] = {};
int s_offer_rssi = 127;

// Sweep/repair cursors and timeouts.
uint8_t s_seq = 0;             // next DATA frame to hold
int s_dwell = 0;               // ticks left holding the current frame
int s_window = 0;              // state-entry window countdown
int s_stall = 0;               // ticks since last forward progress
int s_last_rx_tick = 0;        // DATA heard baseline (receiver)
int s_tick = 0;                // free-running tick counter
int s_resend = 0;              // periodic re-advertise countdown

void copy_text(char *dst, size_t dst_sz, const char *src, size_t max_copy)
{
    if (dst_sz == 0) return;
    size_t n = src ? strlen(src) : 0;
    if (n > max_copy) n = max_copy;
    if (n > dst_sz - 1) n = dst_sz - 1;
    if (n) memcpy(dst, src, n);
    dst[n] = '\0';
}

void close_bundle()
{
    if (s_bundle_fd >= 0) close(s_bundle_fd);
    s_bundle_fd = -1;
}

void discard_received()
{
    close_bundle();
    if (s_rx_path[0]) unlink(s_rx_path);
}

// Handle short IO/EINTR; never publish a bitmap bit for a partial write.
bool file_io(void *data, size_t len, bool writing)
{
    uint8_t *p = static_cast<uint8_t *>(data);
    while (len) {
        ssize_t n = writing ? write(s_bundle_fd, p, len) : read(s_bundle_fd, p, len);
        if (n < 0 && errno == EINTR) continue;
        if (n == 0) errno = EIO; // EOF/zero write is not a full-storage error
        if (n <= 0) return false;
        p += n;
        len -= (size_t)n;
    }
    return true;
}

void fail(ShareFail f)
{
    ESP_LOGW("share_xfer", "failed reason=%u role=%u state=%u received=%u/%u next_send=%u",
             (unsigned)f, (unsigned)s_role, (unsigned)s_state,
             (unsigned)share_bitmap_popcount(s_rx_bitmap, s_rx_total),
             (unsigned)s_rx_total, (unsigned)s_seq);
    discard_received();
    s_fail = f;
    s_state = ShareState::Failed;
    hal_radio_adv_pause();
}

// Capture errno before close/unlink can overwrite the original failure.
void fail_storage(const char *operation)
{
    const int error = errno;
    ESP_LOGE("share_xfer", "%s: errno=%d (%s)", operation, error, strerror(error));
    fail(error == ENOSPC ? ShareFail::StorageFull
        : error == ENOMEM ? ShareFail::NoSpace : ShareFail::Storage);
}

bool prepare_receive_directory()
{
    char parent[sizeof(s_rx_path)];
    memcpy(parent, s_rx_path, sizeof(parent));
    char *slash = strrchr(parent, '/');
    if (!slash || slash == parent) return true; // current directory or root
    *slash = '\0';
    struct stat st;
    if (stat(parent, &st) == 0) {
        if (S_ISDIR(st.st_mode)) return true;
        errno = ENOTDIR;
        return false;
    }
    if (errno != ENOENT) return false;
    return mkdir(parent, 0755) == 0;
}

void send_frame(const uint8_t *frame, size_t len)
{
    if (len == 0 || len > SHARE_MAX_FRAME) return;
    if (hal_radio_send(frame, len) != ESP_OK || hal_radio_adv_resume() != ESP_OK)
        fail(ShareFail::Radio);
}

void send_done(uint8_t status)
{
    uint8_t frame[SHARE_HDR_SIZE + 1];
    share_hdr_t *h = (share_hdr_t *)frame;
    share_set_magic(h);
    h->ver = SHARE_VERSION;
    h->type = SHARE_TYPE_DONE;
    share_wr_u16((uint8_t *)&h->req_id, s_req_id);
    h->seq = 0;
    h->total = s_total_frames;
    h->status = status;
    h->rsv = 0;
    frame[SHARE_HDR_SIZE] = status;
    send_frame(frame, sizeof(frame));
}

void send_offer()
{
    uint8_t frame[SHARE_MAX_FRAME];
    share_hdr_t *h = (share_hdr_t *)frame;
    share_set_magic(h);
    h->ver = SHARE_VERSION;
    h->type = SHARE_TYPE_OFFER;
    share_wr_u16((uint8_t *)&h->req_id, s_req_id);
    h->seq = 0;
    h->total = s_total_frames;
    h->status = SHARE_OK;
    h->rsv = 0;
    size_t body = share_offer_build(frame + SHARE_HDR_SIZE,
                                    SHARE_MAX_BODY,
                                    (uint16_t)s_tx_len,
                                    s_tx_crc,
                                    s_total_frames,
                                    s_tx_slug, s_tx_name, s_tx_ver);
    if (body) send_frame(frame, SHARE_HDR_SIZE + body);
}

void send_accept()
{
    uint8_t frame[SHARE_MAX_FRAME];
    share_hdr_t *h = (share_hdr_t *)frame;
    share_set_magic(h);
    h->ver = SHARE_VERSION;
    h->type = SHARE_TYPE_ACCEPT;
    share_wr_u16((uint8_t *)&h->req_id, s_req_id);
    h->seq = 0;
    h->total = s_rx_total;
    h->status = SHARE_OK;
    h->rsv = 0;
    size_t body = share_accept_build(frame + SHARE_HDR_SIZE, SHARE_MAX_BODY,
                                     s_my_name[0] ? s_my_name : nullptr);
    if (body) send_frame(frame, SHARE_HDR_SIZE + body);
}

void send_data(uint8_t seq)
{
    uint8_t frame[SHARE_MAX_FRAME];
    share_hdr_t *h = (share_hdr_t *)frame;
    share_set_magic(h);
    h->ver = SHARE_VERSION;
    h->type = SHARE_TYPE_DATA;
    share_wr_u16((uint8_t *)&h->req_id, s_req_id);
    h->seq = seq;
    h->total = s_total_frames;
    h->status = SHARE_OK;
    h->rsv = 0;
    size_t off = (size_t)seq * SHARE_CHUNK_LEN;
    size_t chunk = s_tx_len - off;
    if (chunk > SHARE_CHUNK_LEN) chunk = SHARE_CHUNK_LEN;
    if (lseek(s_bundle_fd, (off_t)off, SEEK_SET) != (off_t)off
        || !file_io(frame + SHARE_HDR_SIZE, chunk, false)) {
        fail_storage("read send bundle");
        return;
    }
    send_frame(frame, SHARE_HDR_SIZE + chunk);
}

// Receiver's STAT, re-held whenever the bitmap changes (the register repeats
// on its own at the adv interval; the tick also refreshes it periodically).
void send_stat()
{
    uint8_t frame[SHARE_HDR_SIZE + sizeof(share_stat_t)];
    share_hdr_t *h = (share_hdr_t *)frame;
    share_set_magic(h);
    h->ver = SHARE_VERSION;
    h->type = SHARE_TYPE_STAT;
    share_wr_u16((uint8_t *)&h->req_id, s_req_id);
    h->seq = 0;
    h->total = s_rx_total;
    h->status = SHARE_OK;
    h->rsv = 0;
    share_stat_t *st = (share_stat_t *)(frame + SHARE_HDR_SIZE);
    st->total = s_rx_total;
    st->recv_count = (uint8_t)share_bitmap_popcount(s_rx_bitmap, s_rx_total);
    memcpy(st->bitmap, s_rx_bitmap, SHARE_BITMAP_BYTES);
    send_frame(frame, sizeof(frame));
}

uint8_t sender_total_for(size_t len)
{
    uint8_t total = (uint8_t)((len + SHARE_CHUNK_LEN - 1) / SHARE_CHUNK_LEN);
    return total ? total : 1;
}

void reset_transfer_state()
{
    discard_received();
    s_rx_path[0] = '\0';
    s_verify_pos = 0;
    s_verify_crc = 0;
    s_peer_pinned = false;
    s_tx_len = 0;
    s_tx_crc = 0;
    s_total_frames = 0;
    s_rx_len = 0;
    s_rx_crc = 0;
    s_rx_total = 0;
    memset(s_rx_bitmap, 0, sizeof(s_rx_bitmap));
    s_rx_slug[0] = s_rx_name[0] = s_rx_ver[0] = '\0';
    s_seq = 0;
    s_dwell = 0;
    s_window = 0;
    s_stall = 0;
    s_offer_rssi = 127;
}

bool radio_up()
{
    // Configure the first burst before enabling, avoiding an extra controller
    // stop/reconfigure/start cycle while startup memory is at its tightest.
    if (hal_radio_set_adv_interval_ms(kBurstAdvMs, kBurstAdvMs) != ESP_OK
        || hal_radio_set_scan_duty_ms(kScanMs, kScanMs) != ESP_OK) return false;
    if (hal_radio_enable() != ESP_OK) return false;
    return hal_radio_adv_pause() == ESP_OK; // quiet until OFFER or user acceptance
}

// Receiver idle path: default 20% scan duty while waiting for an offer.
// Advertising stays paused throughout the listen, so the adv interval is
// left alone — accept_offer() sets the burst tune while still paused.
bool radio_up_relaxed()
{
    if (hal_radio_set_scan_duty_ms(kIdleScanIntervalMs, kIdleScanWindowMs) != ESP_OK)
        return false;
    if (hal_radio_enable() != ESP_OK) return false;
    return hal_radio_adv_pause() == ESP_OK; // saying nothing until acceptance
}

// Escalate to the transfer tune. Call while advertising is paused (accept
// path) so the retune costs no extra controller stop/start cycle.
bool radio_go_aggressive()
{
    return hal_radio_set_adv_interval_ms(kBurstAdvMs, kBurstAdvMs) == ESP_OK
        && hal_radio_set_scan_duty_ms(kScanMs, kScanMs) == ESP_OK;
}

// Latest STAT seen from our receiver (sender side).
struct PeerNote {
    int     fresh = 0;   // ticks since heard (large == stale)
    uint8_t recv_count = 0;
    uint8_t bitmap[SHARE_BITMAP_BYTES] = {};
};
PeerNote s_peer;

} // namespace

bool share_xfer_start_send(const char *bundle_path, size_t len, uint32_t crc,
                           const char *slug, const char *app_name,
                           const char *version)
{
    if (s_role != ShareRole::Idle || !bundle_path || len < 6
        || len > SHARE_BUNDLE_MAX_LEN || !slug || !*slug) return false;
    uint8_t total = sender_total_for(len);
    if (total > SHARE_MAX_FRAMES) return false;
    s_role = ShareRole::Sender;
    s_bundle_fd = open(bundle_path, O_RDONLY);
    struct stat st;
    if (s_bundle_fd < 0 || fstat(s_bundle_fd, &st) != 0) {
        fail_storage("open send bundle");
        return true;
    }
    if (!S_ISREG(st.st_mode) || st.st_size != (off_t)len) {
        errno = EINVAL;
        fail_storage("invalid send bundle");
        return true;
    }
    if (!s_rx_mutex) s_rx_mutex = xSemaphoreCreateMutex();
    if (!s_rx_mutex) { fail(ShareFail::NoSpace); return true; }
    if (!radio_up()) {
        s_role = ShareRole::Sender;
        fail(ShareFail::Radio);
        return true;   // started, but already failed — app reads fail()
    }
    hal_radio_set_recv_cb([](const uint8_t mac[6], int8_t rssi,
                             const uint8_t *data, size_t n) {
        share_xfer_on_frame(mac, rssi, data, n);
    });
    s_role = ShareRole::Sender;
    s_fail = ShareFail::None;
    s_tx_len = len;
    s_tx_crc = crc;
    s_total_frames = total;
    s_req_id = (uint16_t)esp_random();
    if (s_req_id == 0) s_req_id = 1;
    copy_text(s_tx_slug, sizeof(s_tx_slug), slug, SHARE_MAX_SLUG);
    copy_text(s_tx_name, sizeof(s_tx_name), app_name, SHARE_MAX_NAME);
    copy_text(s_tx_ver, sizeof(s_tx_ver), version, SHARE_MAX_VER);
    s_peer = PeerNote{};
    s_peer.fresh = 0x7fffffff;
    s_state = ShareState::Offering;
    s_resend = 10;
    ESP_LOGI("share_xfer", "offering %s: bytes=%u frames=%u req=%u",
             s_tx_slug, (unsigned)s_tx_len, (unsigned)s_total_frames, (unsigned)s_req_id);
    send_offer();
    return true;
}

bool share_xfer_start_listen(const char *receive_path)
{
    if (s_role != ShareRole::Idle || !receive_path || !*receive_path
        || strlen(receive_path) >= sizeof(s_rx_path)) return false;
    copy_text(s_rx_path, sizeof(s_rx_path), receive_path, sizeof(s_rx_path) - 1);
    s_role = ShareRole::Receiver;
    // A factory-fresh badge has no apps directory yet. Prepare it before
    // starting Bluetooth, while filesystem initialization has room to run.
    if (!prepare_receive_directory()) {
        fail_storage("prepare receive directory");
        return true;
    }
    if (unlink(s_rx_path) != 0 && errno != ENOENT) {
        fail_storage("remove abandoned transfer");
        return true;
    }
    if (!s_rx_mutex) s_rx_mutex = xSemaphoreCreateMutex();
    if (!s_rx_mutex) { fail(ShareFail::NoSpace); return true; }
    if (!radio_up_relaxed()) {
        s_role = ShareRole::Receiver;
        fail(ShareFail::Radio);
        return true;
    }
    hal_radio_set_recv_cb([](const uint8_t mac[6], int8_t rssi,
                             const uint8_t *data, size_t n) {
        share_xfer_on_frame(mac, rssi, data, n);
    });
    s_role = ShareRole::Receiver;
    s_fail = ShareFail::None;
    s_peer = PeerNote{};
    s_peer.fresh = 0x7fffffff;
    s_state = ShareState::Listening;
    return true;
}

bool share_xfer_accept_offer(const char *display_name)
{
    if (s_role != ShareRole::Receiver || s_state != ShareState::OfferReady) return false;
    s_bundle_fd = open(s_rx_path, O_CREAT | O_TRUNC | O_RDWR, 0600);
    if (s_bundle_fd < 0) {
        fail_storage("open receive bundle");
        return true;
    }
    memset(s_rx_bitmap, 0, sizeof(s_rx_bitmap));
    copy_text(s_my_name, sizeof(s_my_name), display_name, SHARE_MAX_PEER);
    if (!radio_go_aggressive()) {
        fail(ShareFail::Radio);
        return true;
    }
    s_state = ShareState::Accepting;
    s_window = kAcceptBurstTicks;
    s_resend = 10;
    s_stall = 0;
    s_last_rx_tick = s_tick;
    ESP_LOGI("share_xfer", "accepted %s: bytes=%u frames=%u req=%u",
             s_rx_slug, (unsigned)s_rx_len, (unsigned)s_rx_total, (unsigned)s_req_id);
    send_accept();
    return true;
}

void share_xfer_abort()
{
    // Clear the callback FIRST: NimBLE-task use-after-free otherwise
    // (same discipline as app_sync / mp_session_end).
    hal_radio_set_recv_cb(RadioRecvCb{});
    hal_radio_adv_pause();
    hal_radio_reset_timings();
    if (s_rx_mutex && xSemaphoreTake(s_rx_mutex, portMAX_DELAY) == pdTRUE) {
        s_rx_count = 0;
        xSemaphoreGive(s_rx_mutex);
    }
    reset_transfer_state();
    s_role = ShareRole::Idle;
    s_state = ShareState::Idle;
    s_fail = ShareFail::None;
}

void share_xfer_on_frame(const uint8_t mac[6], int8_t rssi,
                         const uint8_t *data, size_t len)
{
    if (len < SHARE_HDR_SIZE || len > SHARE_MAX_FRAME) return;
    const share_hdr_t *h = (const share_hdr_t *)data;
    if (!share_magic_ok(h) || h->ver != SHARE_VERSION) return;
    if (!s_rx_mutex) return;
    if (xSemaphoreTake(s_rx_mutex, 0) != pdTRUE) return;
    if (s_rx_count < kRxQueueMax) {
        Incoming *in = &s_rx_queue[s_rx_count++];
        memcpy(in->mac, mac, 6);
        in->rssi = rssi;
        in->len = (uint16_t)len;
        memcpy(in->data, data, len);
    }
    xSemaphoreGive(s_rx_mutex);
}

namespace {

// Drain one queued frame. Returns 1 when the engine should stop draining
// (terminal state reached), 0 to continue.
int handle_frame(const Incoming *in)
{
    const share_hdr_t *h = (const share_hdr_t *)in->data;
    uint16_t req = share_rd_u16((const uint8_t *)&h->req_id);
    size_t body_len = in->len - SHARE_HDR_SIZE;
    const uint8_t *body = in->data + SHARE_HDR_SIZE;

    if (s_role == ShareRole::Sender) {
        if (req != s_req_id || h->total != s_total_frames
            || (s_peer_pinned && memcmp(in->mac, s_peer_mac, 6))) return 0;
        if (h->type == SHARE_TYPE_ACCEPT && s_state == ShareState::Offering) {
            // Validate acceptance and bind subsequent status to this badge.
            if (!share_accept_parse(body, body_len, nullptr, 0)) return 0;
            ESP_LOGI("share_xfer", "peer accepted req=%u; starting sweep", (unsigned)req);
            memcpy(s_peer_mac, in->mac, 6);
            s_peer_pinned = true;
            s_peer.fresh = 0;
            s_state = ShareState::Sweeping;
            s_seq = 0;
            s_dwell = 0;
            s_window = kSweepCapTicks;
            s_stall = 0;
            return 0;
        }
        if (h->type == SHARE_TYPE_STAT
            && (s_state == ShareState::Offering || s_state == ShareState::Sweeping
                || s_state == ShareState::Repairing)) {
            // STAT while Offering means our ACCEPT was lost but the receiver
            // moved on — adopt the bitmap and sweep; repair fills the rest.
            if (body_len != sizeof(share_stat_t)) return 0;
            const share_stat_t *st = (const share_stat_t *)body;
            if (st->total != s_total_frames
                || st->recv_count != share_bitmap_popcount(st->bitmap, st->total)) return 0;
            memcpy(s_peer_mac, in->mac, 6);
            s_peer_pinned = true;
            if (st->recv_count > s_peer.recv_count) s_stall = 0;
            if (st->recv_count < s_peer.recv_count) return 0; // delayed STAT
            s_peer.recv_count = st->recv_count;
            memcpy(s_peer.bitmap, st->bitmap, SHARE_BITMAP_BYTES);
            s_peer.fresh = 0;
            if (s_state == ShareState::Offering) {
                s_state = ShareState::Sweeping;
                s_seq = 0;
                s_dwell = 0;
                s_window = kSweepCapTicks;
                s_stall = 0;
            }
            return 0;
        }
        return 0;
    }

    if (s_role == ShareRole::Receiver) {
        if (h->type == SHARE_TYPE_OFFER
            && (s_state == ShareState::Listening || s_state == ShareState::OfferReady)) {
            if (in->rssi == 127 || in->rssi < kRssiMin) return 0;
            uint16_t total_len = 0;
            uint32_t crc = 0;
            uint8_t total_frames = 0;
            char slug[SHARE_MAX_SLUG + 1] = {};
            char name[SHARE_MAX_NAME + 1] = {};
            char ver[SHARE_MAX_VER + 1] = {};
            if (!share_offer_parse(body, body_len, &total_len, &crc,
                                   &total_frames, slug, sizeof(slug),
                                   name, sizeof(name), ver, sizeof(ver))) return 0;
            if (req == 0 || total_len < 6 || total_len > SHARE_BUNDLE_MAX_LEN
                || total_frames != sender_total_for(total_len) || h->total != total_frames) {
                // Absurd offer: ignore it rather than fail the whole listen.
                return 0;
            }
            if (!share_bundle_valid_slug(slug)) return 0;
            if (s_state == ShareState::OfferReady) {
                // Keep a visible offer alive while the same sender advertises.
                // Other senders or changed metadata cannot replace this prompt.
                if (req != s_req_id || memcmp(in->mac, s_offer_mac, 6)
                    || total_len != s_rx_len || crc != s_rx_crc
                    || total_frames != s_rx_total || strcmp(slug, s_rx_slug)
                    || strcmp(name, s_rx_name) || strcmp(ver, s_rx_ver)) return 0;
                s_window = kOfferLatchTicks;
                return 0;
            }
            s_req_id = req;
            s_rx_len = total_len;
            s_rx_crc = crc;
            s_rx_total = total_frames;
            copy_text(s_rx_slug, sizeof(s_rx_slug), slug, SHARE_MAX_SLUG);
            copy_text(s_rx_name, sizeof(s_rx_name), name, SHARE_MAX_NAME);
            copy_text(s_rx_ver, sizeof(s_rx_ver), ver, SHARE_MAX_VER);
            memcpy(s_offer_mac, in->mac, 6);
            s_offer_rssi = in->rssi;
            s_state = ShareState::OfferReady;
            s_window = kOfferLatchTicks;
            return 0;
        }
        if (req != s_req_id || memcmp(in->mac, s_offer_mac, 6)) return 0;
        if (h->type == SHARE_TYPE_DATA
            && (s_state == ShareState::Receiving || s_state == ShareState::Accepting)) {
            if (h->total != s_rx_total || h->seq >= s_rx_total) return 0;
            if (s_bundle_fd < 0) return 0;
            size_t off = (size_t)h->seq * SHARE_CHUNK_LEN;
            if (off >= s_rx_len) return 0;
            size_t expect = s_rx_len - off;
            if (expect > SHARE_CHUNK_LEN) expect = SHARE_CHUNK_LEN;
            if (body_len != expect) return 0;   // wrong-sized chunk: not ours
            if (!share_bitmap_test(s_rx_bitmap, h->seq)) {
                if (share_bitmap_popcount(s_rx_bitmap, s_rx_total) == 0)
                    ESP_LOGI("share_xfer", "first DATA seq=%u/%u bytes=%u",
                             (unsigned)h->seq, (unsigned)s_rx_total, (unsigned)body_len);
                if (lseek(s_bundle_fd, (off_t)off, SEEK_SET) != (off_t)off
                    || !file_io(const_cast<uint8_t *>(body), body_len, true)) {
                    fail_storage("write receive bundle");
                    return 1;
                }
                share_bitmap_set(s_rx_bitmap, h->seq);
            }
            s_last_rx_tick = s_tick;
            if (s_state == ShareState::Accepting) s_state = ShareState::Receiving;
            if (share_bitmap_popcount(s_rx_bitmap, s_rx_total) == s_rx_total) {
                // Keep the previous, incomplete STAT on air until verification
                // finishes. A full bitmap is the sender's success signal.
                s_state = ShareState::Verifying;
                s_verify_pos = 0;
                s_verify_crc = 0;
                if (lseek(s_bundle_fd, 0, SEEK_SET) != 0) fail_storage("rewind receive bundle");
                return 1;
            }
            send_stat();
            return 0;
        }
        if (h->type == SHARE_TYPE_DATA && s_state == ShareState::DoneHold) {
            send_stat(); // duplicate / lost final acknowledgement
            return 0;
        }
        if (h->type == SHARE_TYPE_DONE && s_state == ShareState::DoneHold
            && h->status == SHARE_OK) {
            s_state = ShareState::Done;
            hal_radio_adv_pause();
            return 1;
        }
        if (h->type == SHARE_TYPE_DONE
            && (s_state == ShareState::Receiving || s_state == ShareState::Accepting
                || s_state == ShareState::Verifying)) {
            if (h->status == SHARE_CANCEL) fail(ShareFail::Cancelled);
            return 1;
        }
        return 0;
    }
    return 0;
}

void drain_rx()
{
    if (!s_rx_mutex || xSemaphoreTake(s_rx_mutex, 0) != pdTRUE) return;
    Incoming batch[4];
    size_t n = s_rx_count;
    if (n > 4) n = 4;
    for (size_t i = 0; i < n; i++) batch[i] = s_rx_queue[i];
    s_rx_count = 0;
    xSemaphoreGive(s_rx_mutex);
    for (size_t i = 0; i < n; i++) {
        if (handle_frame(&batch[i])) break;
    }
}

int sender_bitmap_full()
{
    return s_peer.recv_count >= s_total_frames;
}

void sender_tick()
{
    switch (s_state) {
    case ShareState::Offering:
        if (--s_resend <= 0) {
            s_resend = 10;   // re-hold: cheap insurance on a shared channel
            send_offer();
        }
        break;
    case ShareState::Sweeping: {
        if (--s_window <= 0) {
            fail(s_peer.fresh > kStallWindowTicks ? ShareFail::Lost : ShareFail::Stalled);
            break;
        }
        if (sender_bitmap_full()) {
            s_state = ShareState::DoneHold;
            s_window = kDoneHoldTicks;
            send_done(SHARE_OK);
            break;
        }
        if (s_seq >= s_total_frames) {
            s_state = ShareState::Repairing;
            s_stall = 0;
            break;
        }
        if (s_dwell <= 0) {
            send_data(s_seq);
            if (s_state == ShareState::Failed) return;
            s_dwell = kDwellTicks;
        } else if (--s_dwell <= 0) {
            s_seq++;
        }
        // STAT heard during the sweep shortens it: skip landed frames.
        while (s_seq < s_total_frames && share_bitmap_test(s_peer.bitmap, s_seq)) s_seq++;
        if (s_seq >= s_total_frames) {
            s_state = ShareState::Repairing;
            s_stall = 0;
        }
        break;
    }
    case ShareState::Repairing: {
        if (--s_window <= 0) {
            fail(s_peer.fresh > kStallWindowTicks ? ShareFail::Lost : ShareFail::Stalled);
            break;
        }
        if (sender_bitmap_full()) {
            s_state = ShareState::DoneHold;
            s_window = kDoneHoldTicks;
            send_done(SHARE_OK);
            break;
        }
        // Next missing frame.
        uint8_t miss = s_total_frames;
        for (uint8_t i = 0; i < s_total_frames; i++) {
            if (!share_bitmap_test(s_peer.bitmap, i)) {
                miss = i;
                break;
            }
        }
        if (miss >= s_total_frames) {
            s_state = ShareState::DoneHold;
            s_window = kDoneHoldTicks;
            send_done(SHARE_OK);
            break;
        }
        if (s_dwell <= 0) {
            send_data(miss);
            if (s_state == ShareState::Failed) return;
            s_seq = miss;
            s_dwell = kDwellTicks;
        } else if (--s_dwell <= 0) {
            if (++s_stall > kStallWindowTicks) fail(ShareFail::Stalled);
        }
        break;
    }
    case ShareState::DoneHold:
        if (--s_resend <= 0) {
            s_resend = 10;
            send_done(SHARE_OK);
        }
        if (--s_window <= 0) {
            s_state = ShareState::Done;
            hal_radio_adv_pause();
        }
        break;
    default:
        break;
    }
    if (s_peer.fresh < 0x7fffffff) s_peer.fresh++;
}

void receiver_tick()
{
    switch (s_state) {
    case ShareState::Listening:
    case ShareState::OfferReady:
        if (s_state == ShareState::OfferReady) {
            if (--s_window <= 0) {
                s_state = ShareState::Listening;   // latch expired, keep ears open
                s_rx_slug[0] = '\0';
            }
        }
        break;
    case ShareState::Accepting:
        if (--s_resend <= 0) {
            s_resend = 10;
            send_accept();
        }
        if (--s_window <= 0) {
            // Sender never swept (walked away, or never heard us): hold STAT
            // anyway for a while in case DATA is already arriving, else fail.
            s_state = ShareState::Receiving;
            s_window = kStallWindowTicks;
            send_stat();
        }
        if (s_tick - s_last_rx_tick > kStallWindowTicks) fail(ShareFail::Lost);
        break;
    case ShareState::Receiving:
        if (--s_resend <= 0) {
            s_resend = 25;   // refresh STAT: the register persists, this is backup
            send_stat();
        }
        if (s_tick - s_last_rx_tick > kStallWindowTicks) fail(ShareFail::Lost);
        break;
    case ShareState::Verifying: {
        uint8_t chunk[SHARE_STREAM_CHUNK_LEN];
        size_t n = s_rx_len - s_verify_pos;
        if (n > sizeof(chunk)) n = sizeof(chunk);
        if (!file_io(chunk, n, false)) { fail_storage("verify receive bundle"); break; }
        s_verify_crc = share_bundle_crc32_update(s_verify_crc, chunk, n);
        s_verify_pos += n;
        if (s_verify_pos == s_rx_len) {
            if (s_verify_crc != s_rx_crc) { fail(ShareFail::Crc); break; }
            // close commits LittleFS metadata before acknowledging success.
            int result = close(s_bundle_fd);
            s_bundle_fd = -1;
            if (result != 0) { fail_storage("commit receive bundle"); break; }
            s_state = ShareState::DoneHold;
            s_window = kDoneHoldTicks;
            s_resend = 0;
            send_stat();
        }
        break;
    }
    case ShareState::DoneHold:
        if (--s_resend <= 0) { s_resend = 10; send_stat(); }
        if (--s_window <= 0) {
            s_state = ShareState::Done;
            hal_radio_adv_pause();
        }
        break;
    default:
        break;
    }
}

} // namespace

void share_xfer_tick()
{
    if (s_role == ShareRole::Idle) return;
    s_tick++;
    drain_rx();
    if (s_state == ShareState::Done || s_state == ShareState::Failed) return;
    if (s_role == ShareRole::Sender) sender_tick();
    else receiver_tick();
}

ShareRole share_xfer_role() { return s_role; }
ShareState share_xfer_state() { return s_state; }
ShareFail share_xfer_fail() { return s_fail; }

int share_xfer_progress_pct()
{
    if (s_role == ShareRole::Sender) {
        if (s_total_frames == 0) return 0;
        if (s_state == ShareState::Done || s_state == ShareState::DoneHold) return 100;
        // Sweep owns 0..80, repair 80..99 — same weighting as bbx_xfer.
        int swept = s_seq >= s_total_frames ? s_total_frames : s_seq;
        int pct = swept * 80 / s_total_frames;
        if (s_state == ShareState::Repairing) {
            pct = 80 + s_peer.recv_count * 19 / s_total_frames;
        }
        return pct > 99 ? 99 : pct;
    }
    if (s_role == ShareRole::Receiver) {
        if (s_rx_total == 0) return 0;
        if (s_state == ShareState::Done || s_state == ShareState::DoneHold) return 100;
        return share_bitmap_popcount(s_rx_bitmap, s_rx_total) * 99 / s_rx_total;
    }
    return 0;
}

bool share_xfer_offer(char *slug_out, size_t slug_sz,
                      char *name_out, size_t name_sz,
                      char *ver_out, size_t ver_sz,
                      size_t *bundle_len_out, uint32_t *crc_out,
                      int *rssi_out)
{
    if (s_role != ShareRole::Receiver || s_state != ShareState::OfferReady) return false;
    if (slug_out && slug_sz) copy_text(slug_out, slug_sz, s_rx_slug, SHARE_MAX_SLUG);
    if (name_out && name_sz) copy_text(name_out, name_sz, s_rx_name, SHARE_MAX_NAME);
    if (ver_out && ver_sz) copy_text(ver_out, ver_sz, s_rx_ver, SHARE_MAX_VER);
    if (bundle_len_out) *bundle_len_out = s_rx_len;
    if (crc_out) *crc_out = s_rx_crc;
    if (rssi_out) *rssi_out = s_offer_rssi;
    return true;
}

const char *share_xfer_rx_file(size_t *len_out)
{
    if (s_role != ShareRole::Receiver || s_state != ShareState::Done || !s_rx_path[0]) return nullptr;
    if (len_out) *len_out = s_rx_len;
    return s_rx_path;
}
