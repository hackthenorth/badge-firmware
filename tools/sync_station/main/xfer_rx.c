#include "xfer_rx.h"

#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_rom_crc.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "ble_link.h"

static const char *TAG = "xfer_rx";

// No accepted frame for this long and the badge has walked away or died. The
// buffer is freed and the lease released so the next badge isn't locked out.
#define LEASE_IDLE_US (8 * 1000 * 1000)

// Hard ceiling regardless of activity, so a badge that keeps dribbling frames
// can't hold the station forever.
#define LEASE_MAX_US (90 * 1000 * 1000)

// How long XFER_DONE keeps being broadcast if the badge never ACKs. Matches the
// relay's RESP_HOLD_US.
#define DONE_HOLD_US (6 * 1000 * 1000)

typedef enum {
    ST_IDLE = 0,
    ST_RECEIVING,   // accepting DATA
    ST_READY,       // full and CRC-checked, waiting for the worker
    ST_POSTING,     // worker owns the buffer
    ST_DOWNLINK,
    ST_DONE,        // broadcasting XFER_DONE, waiting for ACK or expiry
} state_t;

static SemaphoreHandle_t s_lock;   // guards everything below

static state_t  s_state;
static uint8_t  s_badge[6];
static uint8_t  s_sess;
static uint16_t s_req_id;
static uint16_t s_total_len;
static uint32_t s_crc32;
static uint16_t s_chunk_len;
static uint8_t  s_total_frames;
static uint8_t  s_recv_count;
static uint8_t  s_bitmap[BBX_XFER_BITMAP_BYTES];
static char    *s_buf;
static uint8_t  s_done_status;
static int      s_http_code;
static char     s_transport_err[24];
static uint8_t s_want_downlink, s_down_seq, s_down_frames;
static size_t s_down_len;
static int64_t  s_last_frame_us;
static int64_t  s_started_us;
static int64_t  s_done_since_us;

// Session ids cycle rather than restart at 1, so a late frame from the previous
// transfer can't be mistaken for one belonging to the new session.
static uint8_t s_next_sess = 1;

static uint8_t alloc_sess(void)
{
    uint8_t s = s_next_sess++;
    if (s_next_sess == 0) s_next_sess = 1;   // 0 means "no session"
    return s;
}

// --- broadcast helpers; all callers must hold s_lock -----------------------

static void broadcast(uint8_t type, uint8_t flags, uint8_t status,
                      const void *body, size_t body_len)
{
    uint8_t frame[BBX_MAX_FRAME];
    if (body_len > BBX_MAX_BODY) return;

    bbx_hdr_t hdr = {0};
    bbx_set_magic(&hdr);
    hdr.type   = type;
    hdr.flags  = flags;
    hdr.status = status;
    hdr.req_id = s_req_id;
    hdr.sess   = s_sess;
    hdr.seq    = type == BBX_TYPE_PROFILE_DATA ? s_down_seq : 0;
    hdr.total  = type == BBX_TYPE_PROFILE_DATA ? s_down_frames : 1;
    // Broadcast has no destination, so every frame carries the grantee's MAC.
    // This is also how a *second* badge learns the station is taken: it sees a
    // GRANT addressed to someone else. There is deliberately no separate
    // "busy" broadcast — emitting one would overwrite the register the granted
    // badge is relying on, breaking the transfer in progress to answer a badge
    // that can already tell.
    memcpy(hdr.badge, s_badge, 6);

    memcpy(frame, &hdr, sizeof(hdr));
    if (body_len) memcpy(frame + sizeof(hdr), body, body_len);
    ble_link_set_payload(frame, sizeof(hdr) + body_len);
}

static void broadcast_grant(uint8_t status)
{
    bbx_xfer_grant_t g = {0};
    g.chunk_len   = s_chunk_len;
    g.backoff_ds  = 15;   // 1.5 s; the badge adds its own jitter on top
    g.queue_ahead = 1;
    broadcast(BBX_TYPE_XFER_GRANT, 0, status, &g, sizeof(g));
}

static void broadcast_stat(void)
{
    bbx_xfer_stat_t st = {0};
    st.total      = s_total_frames;
    st.recv_count = s_recv_count;
    memcpy(st.bitmap, s_bitmap, sizeof(st.bitmap));
    broadcast(BBX_TYPE_XFER_STAT, 0, BBX_ST_OK, &st, sizeof(st));
}

static void broadcast_done(uint8_t status)
{
    broadcast(BBX_TYPE_XFER_DONE, 0, status, NULL, 0);
}

static void broadcast_profile(void)
{
    const size_t offset = (size_t)s_down_seq * BBX_MAX_BODY;
    size_t len = s_down_len - offset;
    if (len > BBX_MAX_BODY) len = BBX_MAX_BODY;
    broadcast(BBX_TYPE_PROFILE_DATA, 0, BBX_ST_OK, s_buf + offset, len);
}

static void release_locked(const char *why)
{
    if (s_buf) { free(s_buf); s_buf = NULL; }
    s_state = ST_IDLE;
    s_http_code = 0;
    s_transport_err[0] = '\0';
    s_sess = 0;
    s_recv_count = 0;
    s_total_frames = 0;
    memset(s_bitmap, 0, sizeof(s_bitmap));
    memset(s_badge, 0, sizeof(s_badge));
    ble_link_set_adv_interval_ms(200, 220);
    ESP_LOGI(TAG, "session released (%s)", why);
}

// --- frame handlers; all called with s_lock held ---------------------------

static bool open_params_sane(const bbx_xfer_open_t *o)
{
    if (o->kind != BBX_XFER_KIND_UPLOAD || o->want_downlink > 1) return false;
    if (o->enc != BBX_XFER_ENC_RAW) return false;
    if (o->total_len == 0 || o->total_len > BBX_XFER_MAX_LEN) return false;
    if (o->total_len > CONFIG_SYNC_MAX_BLOB_BYTES) return false;
    if (o->chunk_len == 0 || o->chunk_len > BBX_MAX_BODY) return false;
    if (o->total_frames == 0) return false;
    // The frame count must be exactly what the length and chunk imply, or the
    // bitmap and the buffer disagree about where the last byte goes.
    uint32_t need = ((uint32_t)o->total_len + o->chunk_len - 1) / o->chunk_len;
    if (need != o->total_frames) return false;
    return true;
}

static void handle_open(const bbx_hdr_t *hdr, const uint8_t *body, size_t body_len)
{
    if (body_len < sizeof(bbx_xfer_open_t)) return;
    bbx_xfer_open_t o;
    memcpy(&o, body, sizeof(o));

    bool same_badge = (memcmp(hdr->badge, s_badge, 6) == 0);
    bool sweep_done = (hdr->flags & BBX_FLAG_SWEEP_DONE) != 0;

    // The badge's advertisement is a register that repeats tens of times a
    // second, so the overwhelmingly common case is a repeat of the OPEN we
    // already granted. It MUST be idempotent: re-broadcast what the badge is
    // waiting for and return. Re-allocating here would reset the transfer
    // every 30 ms and present as a permanent 0%.
    if (s_state != ST_IDLE) {
        if (same_badge && hdr->req_id == s_req_id) {
            if (s_state == ST_RECEIVING) {
                if (sweep_done) broadcast_stat();
                else            broadcast_grant(BBX_ST_OK);
            } else if (s_state == ST_READY || s_state == ST_POSTING) {
                // Everything arrived; the upload is in flight. Re-assert the
                // full bitmap so a badge that missed the first one still learns
                // it can stop sweeping and start waiting.
                broadcast_stat();
            } else if (s_state == ST_DOWNLINK) {
                broadcast_profile();
            } else if (s_state == ST_DONE) {
                broadcast_done(s_done_status);
            }
            s_last_frame_us = esp_timer_get_time();
        }
        // A different badge gets no reply at all — see the note in broadcast().
        return;
    }

    if (!open_params_sane(&o)) {
        // Answer rather than going silent, so the badge fails fast with a real
        // message instead of waiting out its timeout.
        memcpy(s_badge, hdr->badge, 6);
        s_req_id = hdr->req_id;
        s_sess = 0;
        s_chunk_len = 0;
        broadcast_grant(o.total_len > CONFIG_SYNC_MAX_BLOB_BYTES
                            ? BBX_ST_TOO_BIG : BBX_ST_BAD_SESS);
        memset(s_badge, 0, sizeof(s_badge));
        ESP_LOGW(TAG, "rejected OPEN: len=%u chunk=%u frames=%u",
                 o.total_len, o.chunk_len, o.total_frames);
        return;
    }

    s_buf = malloc(o.total_len);
    if (!s_buf) {
        memcpy(s_badge, hdr->badge, 6);
        s_req_id = hdr->req_id;
        s_sess = 0;
        broadcast_grant(BBX_ST_TOO_BIG);
        memset(s_badge, 0, sizeof(s_badge));
        ESP_LOGE(TAG, "no heap for %u bytes", o.total_len);
        return;
    }

    memcpy(s_badge, hdr->badge, 6);
    s_req_id       = hdr->req_id;
    s_sess         = alloc_sess();
    s_http_code    = 0;
    s_transport_err[0] = '\0';
    s_total_len    = o.total_len;
    s_want_downlink = o.want_downlink;
    s_crc32        = o.crc32;
    s_chunk_len    = o.chunk_len;
    s_total_frames = o.total_frames;
    s_recv_count   = 0;
    memset(s_bitmap, 0, sizeof(s_bitmap));
    s_started_us = s_last_frame_us = esp_timer_get_time();
    s_state = ST_RECEIVING;

    // GRANT and STAT are pure round-trip latency for the badge, so the register
    // has to turn over fast while a session is live.
    ble_link_set_adv_interval_ms(CONFIG_SYNC_ADV_INTERVAL_MS,
                                 CONFIG_SYNC_ADV_INTERVAL_MS + 5);
    broadcast_grant(BBX_ST_OK);

    ESP_LOGI(TAG, "OPEN sess=%u from %02x%02x%02x%02x%02x%02x len=%u frames=%u chunk=%u",
             s_sess, hdr->badge[0], hdr->badge[1], hdr->badge[2],
             hdr->badge[3], hdr->badge[4], hdr->badge[5],
             o.total_len, o.total_frames, o.chunk_len);
    ESP_LOGI(TAG, "sess=%u granted OK, downlink=%d", s_sess, (int)o.want_downlink);
}

static void handle_data(const bbx_hdr_t *hdr, const uint8_t *body, size_t body_len)
{
    if (s_state != ST_RECEIVING) return;
    if (hdr->sess != s_sess) return;
    if (memcmp(hdr->badge, s_badge, 6) != 0) return;
    if (hdr->seq >= s_total_frames) return;

    s_last_frame_us = esp_timer_get_time();

    // The bitmap IS the dedupe. Setting a set bit is a no-op, which is why this
    // module has no LRU: the relay's (badge, req_id) LRU would drop every frame
    // after the first, since a whole transfer shares one req_id.
    if (bbx_bitmap_test(s_bitmap, hdr->seq)) return;

    uint16_t want = bbx_chunk_size(s_total_len, s_chunk_len, s_total_frames, hdr->seq);
    if (body_len != want) {
        ESP_LOGW(TAG, "seq %u: %u bytes, expected %u",
                 hdr->seq, (unsigned)body_len, want);
        return;
    }

    memcpy(s_buf + (size_t)hdr->seq * s_chunk_len, body, body_len);
    bbx_bitmap_set(s_bitmap, hdr->seq);
    s_recv_count++;

    if (s_recv_count < s_total_frames) return;

    uint32_t crc = esp_rom_crc32_le(0, (const uint8_t *)s_buf, s_total_len);
    if (crc != s_crc32) {
        // Every frame arrived and the whole thing is still wrong. Retrying the
        // same bytes would fail the same way, so end it rather than looping.
        ESP_LOGE(TAG, "sess %u CRC %08x, expected %08x",
                 s_sess, (unsigned)crc, (unsigned)s_crc32);
        s_done_status = BBX_ST_CRC;
        s_state = ST_DONE;
        s_done_since_us = esp_timer_get_time();
        broadcast_done(BBX_ST_CRC);
        return;
    }

    ESP_LOGI(TAG, "sess %u complete: %u frames, %u bytes, crc %08x ok",
             s_sess, s_total_frames, s_total_len, (unsigned)crc);
    s_state = ST_READY;

    // Tell the badge immediately that everything landed. Until the upload
    // finishes there is no DONE to send, and the advertising register would
    // otherwise still be holding the previous STAT — the one that says a frame
    // is missing. The badge re-reads that, sees no progress, and gives up as
    // "signal too weak" while the POST is still in flight. A full STAT moves it
    // to AwaitDone, where it waits patiently for the real answer.
    broadcast_stat();
}

static void handle_ack(const bbx_hdr_t *hdr)
{
    if (s_state != ST_DONE) return;
    if (hdr->sess != s_sess) return;
    if (memcmp(hdr->badge, s_badge, 6) != 0) return;
    release_locked("acked");
}

static void handle_profile_ack(const bbx_hdr_t *hdr)
{
    if (s_state != ST_DOWNLINK || hdr->sess != s_sess || hdr->req_id != s_req_id
        || memcmp(hdr->badge, s_badge, 6) != 0 || hdr->seq != s_down_seq) return;
    s_last_frame_us = esp_timer_get_time();
    if (++s_down_seq < s_down_frames) broadcast_profile();
    else {
        s_done_status = BBX_ST_OK;
        s_state = ST_DONE;
        s_done_since_us = esp_timer_get_time();
        broadcast_done(BBX_ST_OK);
    }
}

// --- public ----------------------------------------------------------------

void xfer_rx_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_state = ST_IDLE;
}

void xfer_rx_on_frame(const uint8_t *data, size_t len)
{
    if (len < BBX_HDR_SIZE) return;

    bbx_hdr_t hdr;
    memcpy(&hdr, data, sizeof(hdr));
    if (!bbx_magic_ok(&hdr)) return;

    if (hdr.type != BBX_TYPE_XFER_OPEN && hdr.type != BBX_TYPE_DATA
        && hdr.type != BBX_TYPE_ACK && hdr.type != BBX_TYPE_PROFILE_ACK) {
        return;
    }

    const uint8_t *body = data + BBX_HDR_SIZE;
    size_t body_len = len - BBX_HDR_SIZE;

    // Try-lock only. This runs on the NimBLE host task and every frame repeats,
    // so blocking here to wait out the worker would stall the BLE stack to
    // gain nothing.
    if (xSemaphoreTake(s_lock, 0) != pdTRUE) return;

    switch (hdr.type) {
        case BBX_TYPE_XFER_OPEN: handle_open(&hdr, body, body_len); break;
        case BBX_TYPE_DATA:      handle_data(&hdr, body, body_len); break;
        case BBX_TYPE_ACK:       handle_ack(&hdr);                  break;
        case BBX_TYPE_PROFILE_ACK: handle_profile_ack(&hdr); break;
        default: break;
    }

    xSemaphoreGive(s_lock);
}

void xfer_rx_tick(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int64_t now = esp_timer_get_time();

    switch (s_state) {
        case ST_DOWNLINK:
        case ST_RECEIVING:
            if (now - s_last_frame_us > LEASE_IDLE_US) {
                ESP_LOGW(TAG, "sess %u stalled at %u/%u frames",
                         s_sess, s_recv_count, s_total_frames);
                release_locked("idle timeout");
            } else if (now - s_started_us > LEASE_MAX_US) {
                release_locked("max lease");
            }
            break;
        case ST_DONE:
            if (now - s_done_since_us > DONE_HOLD_US) {
                release_locked("done un-ACKed");
            }
            break;
        case ST_READY:
        case ST_POSTING:
            // The worker owns these; no timeout, since the HTTP client has its
            // own and a slow upload is not a stalled session.
            break;
        case ST_IDLE:
        default:
            break;
    }
    xSemaphoreGive(s_lock);
}

bool xfer_rx_busy(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool busy = (s_state != ST_IDLE);
    xSemaphoreGive(s_lock);
    return busy;
}

void xfer_rx_snapshot(xfer_snapshot_t *out)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    out->idle         = (s_state == ST_IDLE);
    out->sess         = s_sess;
    out->recv_count   = s_recv_count;
    out->total_frames = s_total_frames;
    out->total_len    = s_total_len;
    out->uploading    = (s_state == ST_READY || s_state == ST_POSTING);
    out->downlink     = (s_state == ST_DOWNLINK);
    out->down_seq     = s_down_seq;
    out->down_frames  = s_down_frames;
    out->done         = (s_state == ST_DONE);
    out->done_status  = s_done_status;
    out->http_code    = s_http_code;
    memcpy(out->transport_err, s_transport_err, sizeof(out->transport_err));
    xSemaphoreGive(s_lock);
}

bool xfer_rx_take_upload(char **body_out, size_t *len_out,
                         uint8_t badge_out[6], uint8_t *sess_out, uint8_t *downlink_out)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool got = (s_state == ST_READY);
    if (got) {
        s_state = ST_POSTING;   // parks the buffer; no frame path frees it here
        *body_out = s_buf;
        *len_out  = s_total_len;
        memcpy(badge_out, s_badge, 6);
        *sess_out = s_sess;
        *downlink_out = s_want_downlink;
    }
    xSemaphoreGive(s_lock);
    return got;
}

void xfer_rx_note_http(int http_code)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_state == ST_POSTING) s_http_code = http_code;
    xSemaphoreGive(s_lock);
}

void xfer_rx_note_transport(const char *detail)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_state == ST_POSTING) {
        if (detail != NULL && detail[0] != '\0') {
            strncpy(s_transport_err, detail, sizeof(s_transport_err) - 1);
            s_transport_err[sizeof(s_transport_err) - 1] = '\0';
        } else {
            s_transport_err[0] = '\0';
        }
    }
    xSemaphoreGive(s_lock);
}

void xfer_rx_finish_upload(uint8_t status)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_state == ST_POSTING) {
        s_done_status   = status;
        s_state         = ST_DONE;
        s_done_since_us = esp_timer_get_time();
        broadcast_done(status);
        ESP_LOGI(TAG, "sess %u done, status=%u", s_sess, status);
    }
    xSemaphoreGive(s_lock);
}

void xfer_rx_finish_profile(const char *profile, size_t len, uint8_t status)
{
    if (status != BBX_ST_OK || !profile || len == 0 || len > BBX_PROFILE_MAX_LEN) {
        xfer_rx_finish_upload(status == BBX_ST_OK ? BBX_ST_TOO_BIG : status);
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_state == ST_POSTING) {
        char *response = malloc(len + BBX_PROFILE_PREFIX_LEN);
        if (!response) {
            s_state = ST_DONE; s_done_status = BBX_ST_TOO_BIG;
            s_done_since_us = esp_timer_get_time(); broadcast_done(s_done_status);
        } else {
            uint16_t size = (uint16_t)len;
            uint32_t crc = esp_rom_crc32_le(0, (const uint8_t *)profile, len);
            memcpy(response, &size, 2); memcpy(response + 2, &crc, 4);
            memcpy(response + BBX_PROFILE_PREFIX_LEN, profile, len);
            free(s_buf); s_buf = response;
            s_down_len = len + BBX_PROFILE_PREFIX_LEN;
            s_down_seq = 0;
            s_down_frames = (s_down_len + BBX_MAX_BODY - 1) / BBX_MAX_BODY;
            s_state = ST_DOWNLINK;
            s_started_us = s_last_frame_us = esp_timer_get_time();
            broadcast_profile();
            ESP_LOGI(TAG, "sess %u downlink: %u profile bytes in %u frames",
                     s_sess, (unsigned)len, s_down_frames);
        }
    }
    xSemaphoreGive(s_lock);
}
