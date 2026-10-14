// Badge sync station: reassembles a badge's filesystem snapshot over BLE and
// POSTs it to the upload endpoint.
//
// Structurally the same as tools/relay_firmware: a NimBLE host callback that
// does no work, a worker task that does the blocking HTTPS, and a ticker that
// ages state out. The difference is what arrives — a relay handles one 232-byte
// request, a station handles a ~20 KB transfer split across ~90 frames.
//
// Task layout, and why:
//   NimBLE host task  on_frame()     validate + reassemble. Never blocks.
//   worker task       worker_task()  the HTTPS upload. Blocks for seconds.
//   ticker task       200 ms         session timeouts, and the idle beacon.
//
// The split matters: an HTTPS request from the host callback would stall the
// BLE stack for the whole round trip, and a 20 KB upload takes seconds.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_rom_crc.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "bbx_proto.h"
#include "ble_link.h"
#include "buttons.h"
#include "heartbeat.h"
#include "lights.h"
#include "net_post.h"
#include "screen.h"
#include "wifi_sta.h"
#include "xfer_rx.h"

static const char *TAG = "sync";

// ---------------------------------------------------------------------------
// Beacon
// ---------------------------------------------------------------------------

// BBX_TYPE_SYNC_BEACON, not BBX_TYPE_BEACON. app_blindbox treats any BEACON as
// a Solana Scavenger Hunt id, so sharing the type would make a badge in proximity mode walk
// up to a sync desk and fire a REQ that nobody answers.
// The advertisement is a single register shared with xfer_rx, which owns it
// while a session is live. This tracks whether the beacon is the thing
// currently up, so the ticker restores it once — not five times a second.
static bool s_beacon_up;

static void broadcast_beacon(uint8_t queue_ahead)
{
    uint8_t frame[BBX_HDR_SIZE + BBX_MAX_BOX_ID];
    bbx_hdr_t hdr = {0};
    bbx_set_magic(&hdr);
    hdr.type   = BBX_TYPE_SYNC_BEACON;
    hdr.total  = 1;
    hdr.status = queue_ahead;   // lets a badge show "2 ahead" before it asks
    memcpy(frame, &hdr, sizeof(hdr));

    // Body is the station id: ASCII, NOT nul-terminated, length implied by the
    // frame length.
    size_t id_len = strlen(box_id_current());
    if (id_len > BBX_MAX_BOX_ID) id_len = BBX_MAX_BOX_ID;
    memcpy(frame + sizeof(hdr), box_id_current(), id_len);

    ble_link_set_payload(frame, sizeof(hdr) + id_len);
    s_beacon_up = true;
}

// ---------------------------------------------------------------------------
// Receive
// ---------------------------------------------------------------------------

// Runs on the NimBLE host task. Every frame on this channel arrives here,
// including Badge Bump's BMP2 and the Solana Scavenger Hunt's BBX1 REQ; xfer_rx checks the
// magic and the type and ignores everything that isn't ours.
static void on_frame(const uint8_t *data, size_t len)
{
    xfer_rx_on_frame(data, len);
}

// Transient network failures (DNS/TCP/TLS) are retried a few times: venue
// WiFi often eats the first SYN while the radio roams, and a fast failure
// plus a quick retry still lands inside the badge's wait. HTTP refusals
// (401/403/...) are answers, not retries — they fail immediately.
//
// The retry budget is deliberately shorter than the badge's ~20 s AwaitDone
// timeout (main/apps/bbx_xfer.cpp kDoneTimeoutTicks): retrying past it only
// ties up the station's single session for a badge that already walked away.
#define UPLOAD_MAX_ATTEMPTS 3
#define UPLOAD_RETRY_DELAY_MS 2000
#define UPLOAD_RETRY_BUDGET_US (18 * 1000 * 1000)

// True when another upload attempt is worthwhile after attempt `attempt`
// failed with BBX_ST_TRANSPORT: attempts remain and the badge is probably
// still listening (see the budget note above).
static bool retry_left(int attempt, int64_t t_start)
{
    return attempt < UPLOAD_MAX_ATTEMPTS
        && esp_timer_get_time() - t_start < UPLOAD_RETRY_BUDGET_US;
}

// Picks up a reassembled snapshot and uploads it. Separate from the host task
// because this blocks for seconds.
static void worker_task(void *arg)
{
    (void)arg;
    for (;;) {
        char   *body = NULL;
        size_t  len  = 0;
        uint8_t badge[6];
        uint8_t sess, downlink;

        if (!xfer_rx_take_upload(&body, &len, badge, &sess, &downlink)) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        char badge_hex[13];
        snprintf(badge_hex, sizeof(badge_hex), "%02x%02x%02x%02x%02x%02x",
                 badge[0], badge[1], badge[2], badge[3], badge[4], badge[5]);

        ESP_LOGI(TAG, "uploading sess %u: %u bytes from %s, free heap %u",
                 sess, (unsigned)len, badge_hex, (unsigned)esp_get_free_heap_size());

        net_post_status_t res;
        memset(&res, 0, sizeof(res));
        int64_t t_start = esp_timer_get_time();
        for (int attempt = 1;; ++attempt) {
            if (downlink) {
                char *profile = malloc(BBX_PROFILE_MAX_LEN + 128);
                net_sync_upload(CONFIG_SYNC_UPLOAD_URL, body, len, badge_hex,
                                profile, BBX_PROFILE_MAX_LEN + 128, &res);
                if (res.status != BBX_ST_TRANSPORT || !retry_left(attempt, t_start)) {
                    // Only the numeric code and the short network-error
                    // detail reach the screen: never log or display bodies,
                    // tokens, or profiles.
                    xfer_rx_note_http(res.http_code);
                    xfer_rx_note_transport(res.transport_err);
                    xfer_rx_finish_profile(profile, res.profile_len, res.status);
                    free(profile);
                    break;
                }
                free(profile);
            } else {
                net_post_upload(CONFIG_SYNC_UPLOAD_URL, body, len, badge_hex, &res);
                if (res.status != BBX_ST_TRANSPORT || !retry_left(attempt, t_start)) {
                    xfer_rx_note_http(res.http_code);
                    xfer_rx_note_transport(res.transport_err);
                    xfer_rx_finish_upload(res.status);
                    break;
                }
            }
            ESP_LOGW(TAG, "sess %u upload attempt %d failed (%s), retrying in %d ms",
                     sess, attempt, res.transport_err, UPLOAD_RETRY_DELAY_MS);
            vTaskDelay(pdMS_TO_TICKS(UPLOAD_RETRY_DELAY_MS));
        }
        // One line per request: outcome codes only, never bodies, tokens,
        // or profiles. `net` is the underlying network error for transport
        // failures (socket errno or TLS error), empty otherwise.
        ESP_LOGI(TAG, "sess %u request done: status=%u http=%d downlink=%u profile_len=%u net=%s",
                 sess, res.status, res.http_code, downlink, (unsigned)res.profile_len,
                 res.transport_err);
    }
}

// One-line rendering of a transfer outcome for the short screen. An HTTP
// failure shows the endpoint's numeric code ("HTTP 401") so the desk can tell
// a wrong station password (401), a registration problem (403/404/409), an
// overlong profile (422), and rate limiting (429) apart without touching logs.
// A transport failure shows the underlying network error ("errno 104",
// "tls 0x7f00") so DNS/TCP/TLS faults are distinguishable at the desk;
// without detail it falls back to "net err".
static void done_short(uint8_t st, int http_code, const char *transport_err,
                       char *out, size_t out_sz)
{
    switch (st) {
        case BBX_ST_OK:        snprintf(out, out_sz, "ok"); break;
        case BBX_ST_NO_WIFI:   snprintf(out, out_sz, "no wifi"); break;
        case BBX_ST_TRANSPORT:
            if (transport_err != NULL && transport_err[0] != '\0') {
                snprintf(out, out_sz, "%.23s", transport_err);
            } else {
                snprintf(out, out_sz, "net err");
            }
            break;
        case BBX_ST_HTTP:
            if (http_code > 0) snprintf(out, out_sz, "HTTP %d", http_code);
            else               snprintf(out, out_sz, "http err");
            break;
        case BBX_ST_CRC:       snprintf(out, out_sz, "crc err"); break;
        case BBX_ST_TOO_BIG:   snprintf(out, out_sz, "too big"); break;
        case BBX_ST_TIMEOUT:   snprintf(out, out_sz, "timeout"); break;
        case BBX_ST_BAD_SESS:  snprintf(out, out_sz, "sess?"); break;
        default:               snprintf(out, out_sz, "busy"); break;
    }
}

// Status vocabulary shared with the badge Sync app (main/apps/app_sync.cpp):
// both sides render the same transfer as status + detail + progress bar, with
// the same verdict strings (done_short() here matches the badge's failure
// copy) — and now on the same graphics system too: the station carries the
// badge's LCD and renders it with the badge's vendored LVGL stack
// (main/screen.c mirrors the Sync app's chrome).
//
// The screen always says what the station is doing: receiving (with a fill
// bar), uploading, sending the profile back, the last DONE while it is still
// on the air, or idle.
// screen_show() diffs, so a steady state costs no I2C traffic.
static char s_last_verdict[24];
static int64_t s_last_done_us;
static bool s_have_verdict;

// How long a DONE verdict keeps owning the screen after the session releases
// (badge ACKed, or the hold expired). The lease frees immediately so the next
// badge isn't locked out; only the display lingers, so a failure stays
// readable instead of blipping back to "ready" in under a second.
#define VERDICT_HOLD_US (10 * 1000 * 1000)

static void screen_refresh(void)
{
    xfer_snapshot_t s;
    xfer_rx_snapshot(&s);

    if (!s.idle) {
        if (s.done) {
            char verdict[24];
            done_short(s.done_status, s.http_code, s.transport_err,
                       verdict, sizeof(verdict));
            screen_show("DONE", verdict, -1, 0);
            if (s.done_status == BBX_ST_OK) lights_status(0x36, 0xe0, 0x7a);
            else lights_status(0xff, 0x5c, 0x8a);
            memcpy(s_last_verdict, verdict, sizeof(s_last_verdict));
            s_last_done_us = esp_timer_get_time();
            s_have_verdict = true;
        } else if (s.downlink) {
            // The uplink counters are stale here — the profile is its own
            // short transfer, so bar it by acked profile frames.
            char frac[12];
            snprintf(frac, sizeof(frac), "%u/%u", s.down_seq, s.down_frames);
            screen_show("SYNC", frac, s.down_seq, s.down_frames);
            lights_status(0xff, 0xd1, 0x66);
        } else if (s.uploading) {
            char kb[12];
            snprintf(kb, sizeof(kb), "%uKB",
                     (unsigned)((s.total_len + 1023) / 1024));
            screen_show("POST", kb, -1, 0);
            lights_status(0xff, 0xd1, 0x66);
        } else {
            char frac[12];
            snprintf(frac, sizeof(frac), "%u/%u", s.recv_count, s.total_frames);
            screen_show("RX", frac, s.recv_count, s.total_frames);
            lights_status(0xff, 0xd1, 0x66);
        }
    } else if (s_have_verdict &&
               esp_timer_get_time() - s_last_done_us < VERDICT_HOLD_US) {
        // Session released (ACKed or expired) but the verdict is still fresh:
        // hold it so failures stay readable instead of blipping to "ready".
        screen_show("DONE", s_last_verdict, -1, 0);
    } else if (!wifi_sta_connected()) {
        screen_show(box_id_current(), "no wifi", -1, 0);
        lights_status(0xff, 0x5c, 0x8a);
    } else {
        s_have_verdict = false;
#if CONFIG_SYNC_AIR_DEBUG
        // Air counters: all extended-advertising reports heard / ours
        // (0xFFFF). While a badge syncs, the second number must climb ~30/s;
        // if it doesn't, the badge's frames never reach this scan.
        uint32_t all = 0, mfg = 0;
        ble_link_air_counts(&all, &mfg);
        char idle[32];
        snprintf(idle, sizeof(idle), "ready %u/%u", (unsigned)all, (unsigned)mfg);
        screen_show(box_id_current(), idle, -1, 0);
#else
        screen_show(box_id_current(), "ready", -1, 0);
        lights_status(0x36, 0xe0, 0x7a);
#endif
    }
}

// Ages sessions out and puts the beacon back once the register is free again.
static void ticker_task(void *arg)
{
    static int ticks;
    (void)arg;
    for (;;) {
        xfer_rx_tick();

        bool busy = xfer_rx_busy();
        if (busy) {
            // xfer_rx owns the advertisement while a session is live.
            s_beacon_up = false;
        } else if (!s_beacon_up) {
            broadcast_beacon(0);
        }
        screen_refresh();
        // Remote eyes for the desk: a 5 s heartbeat with the air counters
        // and the session snapshot, so a nearby badge sync can be followed
        // over USB serial without watching the LCD.
        if (++ticks >= 25) {
            ticks = 0;
            uint32_t all = 0, mfg = 0;
            ble_link_air_counts(&all, &mfg);
            xfer_snapshot_t s;
            xfer_rx_snapshot(&s);
            ESP_LOGI(TAG, "hb air=%u ours=%u idle=%d sess=%u rx=%u/%u wifi=%d",
                     (unsigned)all, (unsigned)mfg, (int)s.idle, s.sess,
                     s.recv_count, s.total_frames, (int)wifi_sta_connected());
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

// Re-broadcast the idle beacon when no session is live (button context).
void sync_beacon_refresh(void)
{
    if (!xfer_rx_busy()) broadcast_beacon(0);
}

static void on_box_id_changed(void)
{
    sync_beacon_refresh();
    screen_show(box_id_current(), "ready", -1, 0);
}

// ---------------------------------------------------------------------------
// Boot self-test
// ---------------------------------------------------------------------------

// Posts a synthetic body the same size as a real snapshot.
//
// The relay's equivalent sends 60 bytes, which exercises none of what is
// actually risky here: a 20 KB body is what proves the chunked write loop, the
// Content-Length path and the LWIP send-buffer sizing. Doing it at boot means
// those fail on a bench with a serial log attached rather than in a hall with
// a badge in someone's hand.
static void run_selftest(void)
{
    const size_t kBodyLen = 20 * 1024;
    char *body = malloc(kBodyLen + 1);
    if (!body) {
        ESP_LOGE(TAG, "selftest: cannot allocate %u bytes", (unsigned)kBodyLen);
        return;
    }

    // The upload path only accepts HTNSYNC1 envelopes (claim id + 64-hex
    // token + JSON snapshot) and refuses anything else before any network
    // I/O — so the synthetic body must be a valid envelope, or the
    // self-test passes without ever exercising WiFi, TLS, or the server.
    // Claim/token are fixtures, never real credentials.
    int n = snprintf(body, kBodyLen + 1,
                     "HTNSYNC1\n42\n"
                     "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\n"
                     "{\"v\":1,\"kind\":\"selftest\",\"station\":\"%s\",\"pad\":\"",
                     CONFIG_SYNC_STATION_ID);
    // Pad to exactly kBodyLen, leaving room for the closing quote and brace.
    size_t tail = 3;   // "\"}" plus the NUL
    memset(body + n, 'A', kBodyLen - (size_t)n - tail + 1);
    body[kBodyLen - 2] = '"';
    body[kBodyLen - 1] = '}';
    body[kBodyLen]     = '\0';

    ESP_LOGI(TAG, "selftest: posting %u bytes, free heap %u",
             (unsigned)kBodyLen, (unsigned)esp_get_free_heap_size());
    screen_show("selftest", "POST", -1, 0);

    net_post_status_t res;
    net_post_upload(CONFIG_SYNC_UPLOAD_URL, body, kBodyLen, "000000000000", &res);

    ESP_LOGI(TAG, "selftest: status=%u http=%d, free heap %u",
             res.status, res.http_code, (unsigned)esp_get_free_heap_size());
    if (res.status == BBX_ST_OK) {
        screen_show("selftest", "200 ok", -1, 0);
    } else if (res.status == BBX_ST_HTTP) {
        char http[12];
        snprintf(http, sizeof(http), "HTTP %d", res.http_code);
        screen_show("selftest", http, -1, 0);
    } else {
        char verdict[24];
        done_short(res.status, res.http_code, res.transport_err,
                   verdict, sizeof(verdict));
        screen_show("selftest", verdict, -1, 0);
    }
    // The ticker takes over the screen a moment later; hold the verdict long
    // enough to read (matches the badge's ~8 s error hold at the desk).
    vTaskDelay(pdMS_TO_TICKS(3000));
    free(body);
}


// ---------------------------------------------------------------------------
// Reassembly loopback test
// ---------------------------------------------------------------------------

#if CONFIG_SYNC_LOOPBACK_TEST

// Feeds a synthetic transfer straight into the receive path — no badge, no
// radio. This exists because the bitmap arithmetic is the part that is
// genuinely miserable to debug over the air: LSB-first bit order, the short
// final frame, and the requirement that a repeated OPEN not reset a transfer.
//
// The loss pattern is deliberate rather than random, so a failure reproduces.
static void feed_frame(uint8_t type, uint8_t flags, uint8_t sess, uint8_t seq,
                       uint16_t req_id, const uint8_t badge[6],
                       const void *body, size_t body_len)
{
    uint8_t frame[BBX_MAX_FRAME];
    bbx_hdr_t hdr = {0};
    bbx_set_magic(&hdr);
    hdr.type   = type;
    hdr.flags  = flags;
    hdr.req_id = req_id;
    hdr.sess   = sess;
    hdr.seq    = seq;
    hdr.total  = 1;
    memcpy(hdr.badge, badge, 6);
    memcpy(frame, &hdr, sizeof(hdr));
    if (body_len) memcpy(frame + sizeof(hdr), body, body_len);
    xfer_rx_on_frame(frame, sizeof(hdr) + body_len);
}

static void run_loopback_test(void)
{
    // Deliberately not a multiple of the chunk size, so the last frame is
    // short and the remainder arithmetic actually gets exercised.
    const uint16_t kLen   = 20003;
    const uint16_t kChunk = BBX_MAX_BODY;
    const uint8_t  kFrames = (uint8_t)((kLen + kChunk - 1) / kChunk);
    const uint8_t  badge[6] = {0xde, 0xad, 0xbe, 0xef, 0x00, 0x01};
    const uint16_t req_id = 0x1234;

    char *payload = malloc(kLen);
    if (!payload) { ESP_LOGE(TAG, "loopback: no heap"); return; }
    for (uint16_t i = 0; i < kLen; i++) payload[i] = (char)('A' + (i % 26));

    ESP_LOGI(TAG, "loopback: %u bytes, %u frames, last frame %u bytes",
             kLen, kFrames, bbx_chunk_size(kLen, kChunk, kFrames, kFrames - 1));

    bbx_xfer_open_t o = {0};
    o.kind         = BBX_XFER_KIND_UPLOAD;
    o.enc          = BBX_XFER_ENC_RAW;
    o.total_len    = kLen;
    o.crc32        = esp_rom_crc32_le(0, (const uint8_t *)payload, kLen);
    o.chunk_len    = kChunk;
    o.total_frames = kFrames;

    feed_frame(BBX_TYPE_XFER_OPEN, 0, 0, 0, req_id, badge, &o, sizeof(o));

    // The session id is assigned by the station and we cannot see it from here,
    // but it is the first one handed out after boot.
    const uint8_t sess = 1;

    // Sweep, dropping every 7th frame.
    int dropped = 0;
    for (uint8_t seq = 0; seq < kFrames; seq++) {
        if (seq % 7 == 3) { dropped++; continue; }
        uint16_t n = bbx_chunk_size(kLen, kChunk, kFrames, seq);
        feed_frame(BBX_TYPE_DATA, 0, sess, seq, req_id, badge,
                   payload + (size_t)seq * kChunk, n);
    }
    ESP_LOGI(TAG, "loopback: swept, dropped %d frames on purpose", dropped);

    // Hammer the OPEN the way a real badge's repeating advertisement does. If
    // this reset the session, the frames above would be lost and the transfer
    // below could never complete — which is exactly the assertion.
    for (int i = 0; i < 50; i++) {
        feed_frame(BBX_TYPE_XFER_OPEN, 0, 0, 0, req_id, badge, &o, sizeof(o));
    }

    // Replaying an already-received frame must be a no-op, not a double count.
    feed_frame(BBX_TYPE_DATA, 0, sess, 0, req_id, badge, payload,
               bbx_chunk_size(kLen, kChunk, kFrames, 0));

    // Repair sweep.
    for (uint8_t seq = 0; seq < kFrames; seq++) {
        if (seq % 7 != 3) continue;
        uint16_t n = bbx_chunk_size(kLen, kChunk, kFrames, seq);
        feed_frame(BBX_TYPE_DATA, 0, sess, seq, req_id, badge,
                   payload + (size_t)seq * kChunk, n);
    }

    char   *body = NULL;
    size_t  len  = 0;
    uint8_t got_badge[6];
    uint8_t got_sess, got_downlink;
    if (!xfer_rx_take_upload(&body, &len, got_badge, &got_sess, &got_downlink)) {
        ESP_LOGE(TAG, "loopback: FAILED — session did not complete");
        free(payload);
        return;
    }

    bool ok = (len == kLen) && (memcmp(body, payload, kLen) == 0) && got_downlink == 0;
    ESP_LOGI(TAG, "loopback: %s — %u bytes, sess=%u, bytes %s",
             ok ? "PASS" : "FAILED", (unsigned)len, got_sess,
             ok ? "identical" : "DIFFER");
    screen_show("loopback", ok ? "PASS" : "FAIL", -1, 0);
    vTaskDelay(pdMS_TO_TICKS(3000));

    // Release the lease so the station is usable afterwards.
    xfer_rx_finish_upload(ok ? BBX_ST_OK : BBX_ST_CRC);
    free(payload);
}

#endif  // CONFIG_SYNC_LOOPBACK_TEST

// ---------------------------------------------------------------------------

void app_main(void)
{
    // Verbose WiFi-driver logging for join debugging. Everything else stays
    // at INFO so the log is readable.
    esp_log_level_set("wifi", ESP_LOG_DEBUG);
    esp_log_level_set("pp", ESP_LOG_DEBUG);
    esp_log_level_set("net80211", ESP_LOG_DEBUG);

    // Screen first, so "boot" is up while everything else initialises.
    screen_init();
    screen_show(CONFIG_SYNC_STATION_ID, "boot", -1, 0);

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    // Buttons before anything reads the station id: the selection starts
    // where NVS (or the flashed SYNC_STATION_ID) points.
    buttons_set_changed_cb(on_box_id_changed);
    buttons_start();
    lights_init();

    screen_show(box_id_current(), "wifi...", -1, 0);
    ESP_ERROR_CHECK(wifi_sta_start());

#if CONFIG_SYNC_SELFTEST_ON_BOOT
    run_selftest();
#endif

    xfer_rx_init();

    // BLE comes up after WiFi so software coexistence is arbitrating from the
    // start, rather than having BLE established and then WiFi barge in.
    ESP_ERROR_CHECK(ble_link_init(on_frame));
    broadcast_beacon(0);

#if CONFIG_SYNC_LOOPBACK_TEST
    run_loopback_test();
#endif

    xTaskCreate(worker_task, "sync_worker", 8192, NULL, 5, NULL);
    xTaskCreate(ticker_task, "sync_ticker", 3072, NULL, 4, NULL);
#if CONFIG_SYNC_HEARTBEAT_ENABLE
    heartbeat_start();
#endif

    ESP_LOGI(TAG, "station \"%s\" ready -> %s",
             box_id_current(), CONFIG_SYNC_UPLOAD_URL);
    ESP_LOGI(TAG, "free heap %u", (unsigned)esp_get_free_heap_size());
}
