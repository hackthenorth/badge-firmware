// Solana Scavenger Hunt BLE -> HTTP relay.
//
// Listens for BBX1 request frames from badges, POSTs the body verbatim to a
// configured webhook, and broadcasts the reply back. It never parses the
// payload in either direction — the badge builds the request JSON and parses
// the response JSON itself.
//
// Task layout, and why:
//   NimBLE host task  on_frame()   validate + dedupe + enqueue. Never blocks.
//   worker task       do_job()     the HTTPS round trip. Blocks for seconds.
//   ticker task       200 ms       retires the response back to a BEACON.
//
// The split matters: an HTTPS request from the host callback would stall the
// BLE stack for the whole round trip.

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
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

static const char *TAG = "relay";

// How long a response keeps being broadcast if the badge never ACKs. The badge
// gives up at 12 s, so this only needs to cover a badge that heard us and went
// away — an ACK normally retires it in well under a second.
#define RESP_HOLD_US (6 * 1000 * 1000)

// Remembering the last 16 (badge, req_id) pairs is enough: advertising repeats
// every ~200 ms for up to 12 s, so without this a single tap would fire dozens
// of POSTs.
#define DEDUPE_SLOTS 16

#define JOB_QUEUE_DEPTH 4

typedef struct {
    uint8_t  badge[6];
    uint16_t req_id;
    uint16_t body_len;
    char     body[BBX_MAX_BODY + 1];
} job_t;

typedef struct {
    uint8_t  badge[6];
    uint16_t req_id;
    bool     used;
} dedupe_slot_t;

static QueueHandle_t     s_jobs;
static SemaphoreHandle_t s_lock;          // guards everything below

static dedupe_slot_t s_dedupe[DEDUPE_SLOTS];
static int           s_dedupe_next;

// The response currently being advertised, if any.
static bool     s_resp_active;
static uint8_t  s_resp_badge[6];
static uint16_t s_resp_req_id;
static int64_t  s_resp_until_us;

// Set by the host task when a request arrives with a full queue; the ticker
// turns it into a BUSY response so the badge fails fast instead of timing out.
static bool     s_busy_pending;
static uint8_t  s_busy_badge[6];
static uint16_t s_busy_req_id;

// Relay status for the screen. Written by the host task (counters) and the
// worker/ticker (verdicts); the ticker renders under s_lock. All guarded by
// s_lock.
static uint32_t s_req_seen;
static uint32_t s_req_dup;
static uint32_t s_served;
static uint32_t s_busy_sent;
static bool     s_posting;
static uint16_t s_post_req_id;
static uint8_t  s_last_status;
static bool     s_have_last;

// Idle "I'm alive, and I'm this box". The body tells a badge which station
// it walked up to; the badge echoes it back as the "box" field.
static void broadcast_beacon(void)
{
    uint8_t frame[BBX_MAX_FRAME];
    bbx_hdr_t hdr = {0};
    bbx_set_magic(&hdr);
    hdr.type  = BBX_TYPE_BEACON;
    hdr.seq   = 0;
    hdr.total = 1;
    memcpy(frame, &hdr, sizeof(hdr));

    size_t id_len = strlen(box_id_current());
    if (id_len > BBX_MAX_BOX_ID) id_len = BBX_MAX_BOX_ID;
    memcpy(frame + sizeof(hdr), box_id_current(), id_len);

    ble_link_set_payload(frame, sizeof(hdr) + id_len);
}

static void broadcast_response(const uint8_t badge[6], uint16_t req_id,
                               uint8_t status, uint8_t flags,
                               const char *body, size_t body_len)
{
    uint8_t frame[BBX_MAX_FRAME];
    bbx_hdr_t hdr = {0};
    bbx_set_magic(&hdr);
    hdr.type   = BBX_TYPE_RESP;
    hdr.flags  = flags;
    hdr.req_id = req_id;
    hdr.status = status;
    hdr.seq    = 0;
    hdr.total  = 1;
    memcpy(hdr.badge, badge, 6);
    memcpy(frame, &hdr, sizeof(hdr));

    if (body_len > BBX_MAX_BODY) body_len = BBX_MAX_BODY;
    if (body_len > 0) memcpy(frame + sizeof(hdr), body, body_len);

    ble_link_set_payload(frame, sizeof(hdr) + body_len);

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_resp_active   = true;
    s_resp_req_id   = req_id;
    memcpy(s_resp_badge, badge, 6);
    s_resp_until_us = esp_timer_get_time() + RESP_HOLD_US;
    xSemaphoreGive(s_lock);
}

// Caller must hold s_lock.
static bool seen_before_locked(const uint8_t badge[6], uint16_t req_id)
{
    for (int i = 0; i < DEDUPE_SLOTS; i++) {
        if (s_dedupe[i].used && s_dedupe[i].req_id == req_id &&
            memcmp(s_dedupe[i].badge, badge, 6) == 0) {
            return true;
        }
    }
    s_dedupe[s_dedupe_next].used   = true;
    s_dedupe[s_dedupe_next].req_id = req_id;
    memcpy(s_dedupe[s_dedupe_next].badge, badge, 6);
    s_dedupe_next = (s_dedupe_next + 1) % DEDUPE_SLOTS;
    return false;
}

// NimBLE host task. Keep this cheap — no logging in the common repeat case, no
// network, no long lock holds.
static void on_frame(const uint8_t *data, size_t len)
{
    if (len < sizeof(bbx_hdr_t)) return;

    bbx_hdr_t hdr;
    memcpy(&hdr, data, sizeof(hdr));
    if (!bbx_magic_ok(&hdr)) return;   // e.g. a "BUMP" frame from badge bumping

    if (hdr.type == BBX_TYPE_ACK) {
        // Badge has the answer; free the slot so the next attendee isn't stuck
        // waiting out the hold timer.
        xSemaphoreTake(s_lock, portMAX_DELAY);
        bool match = s_resp_active && s_resp_req_id == hdr.req_id &&
                     memcmp(s_resp_badge, hdr.badge, 6) == 0;
        if (match) {
            s_resp_active = false;
            s_resp_until_us = 0;
        }
        xSemaphoreGive(s_lock);
        if (match) broadcast_beacon();
        return;
    }

    if (hdr.type != BBX_TYPE_REQ) return;
    if (hdr.req_id == 0) return;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool dup = seen_before_locked(hdr.badge, hdr.req_id);
    if (dup) s_req_dup++;
    else s_req_seen++;
    xSemaphoreGive(s_lock);
    if (dup) return;

    job_t job = {0};
    memcpy(job.badge, hdr.badge, 6);
    job.req_id = hdr.req_id;
    size_t body_len = len - sizeof(hdr);
    if (body_len > BBX_MAX_BODY) body_len = BBX_MAX_BODY;
    memcpy(job.body, data + sizeof(hdr), body_len);
    job.body[body_len] = '\0';
    job.body_len = (uint16_t)body_len;

    if (xQueueSend(s_jobs, &job, 0) != pdTRUE) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_busy_pending = true;
        s_busy_req_id  = hdr.req_id;
        memcpy(s_busy_badge, hdr.badge, 6);
        xSemaphoreGive(s_lock);
    }
}

static void worker_task(void *arg)
{
    static net_post_result_t result;   // ~250 B, keep it off the task stack
    job_t job;

    for (;;) {
        if (xQueueReceive(s_jobs, &job, portMAX_DELAY) != pdTRUE) continue;

        ESP_LOGI(TAG, "req %04x from %02x%02x%02x%02x%02x%02x: %s",
                 job.req_id, job.badge[0], job.badge[1], job.badge[2],
                 job.badge[3], job.badge[4], job.badge[5], job.body);

        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_posting = true;
        s_post_req_id = job.req_id;
        xSemaphoreGive(s_lock);

        net_post_json(CONFIG_RELAY_WEBHOOK_URL, job.body, job.body_len, &result);

        ESP_LOGI(TAG, "resp %04x status=%u: %s", job.req_id, result.status,
                 result.body);

        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_posting = false;
        s_served++;
        s_last_status = result.status;
        s_have_last = true;
        xSemaphoreGive(s_lock);

        broadcast_response(job.badge, job.req_id, result.status, result.flags,
                           result.body, result.body_len);
    }
}

// One-line rendering of a POST outcome for the status screen.
static void verdict_short(uint8_t st, char *out, size_t out_sz)
{
    switch (st) {
        case BBX_ST_OK:        snprintf(out, out_sz, "ok"); break;
        case BBX_ST_NO_WIFI:   snprintf(out, out_sz, "no wifi"); break;
        case BBX_ST_TRANSPORT: snprintf(out, out_sz, "net err"); break;
        case BBX_ST_HTTP:      snprintf(out, out_sz, "http err"); break;
        case BBX_ST_BUSY:      snprintf(out, out_sz, "busy"); break;
        default:               snprintf(out, out_sz, "st %u", st); break;
    }
}

// The screen always says what the relay is doing: posting (with the req id),
// holding a response on the air (with its verdict), or idle with a served
// count — plus "no wifi" when the uplink is down.
// screen_show() diffs, so a steady state costs no redraws.
static void screen_refresh(void)
{
    bool resp, posting, have;
    uint8_t st;
    uint16_t post_id;
    uint32_t served, busy;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    resp = s_resp_active;
    posting = s_posting;
    post_id = s_post_req_id;
    have = s_have_last;
    st = s_last_status;
    served = s_served;
    busy = s_busy_sent;
    xSemaphoreGive(s_lock);

    if (posting) {
        char id[SCREEN_LINE_MAX + 1];
        snprintf(id, sizeof(id), "%04x", post_id);
        screen_show("POST", id, -1, 0);
        lights_status(0xff, 0xd1, 0x66);
    } else if (resp && have) {
        char verdict[SCREEN_LINE_MAX + 1];
        verdict_short(st, verdict, sizeof(verdict));
        screen_show("RESP", verdict, -1, 0);
        if (st == BBX_ST_OK) lights_status(0x36, 0xe0, 0x7a);
        else lights_status(0xff, 0x5c, 0x8a);
    } else if (!wifi_sta_connected()) {
        screen_show(box_id_current(), "no wifi", -1, 0);
        lights_status(0xff, 0x5c, 0x8a);
    } else {
        // 31-char lines fit any counter outright, so snprintf can never
        // truncate (-Werror=format-truncation).
        char l2[SCREEN_LINE_MAX + 1];
        if (busy > 0) snprintf(l2, sizeof(l2), "ok %u busy %u",
                               (unsigned)served, (unsigned)busy);
        else if (served > 0) snprintf(l2, sizeof(l2), "ok %u", (unsigned)served);
        else snprintf(l2, sizeof(l2), "ready");
        screen_show(box_id_current(), l2, -1, 0);
        lights_status(0x36, 0xe0, 0x7a);
    }
}

// Retires responses. Runs off the host task so it can safely take the lock and
// touch the advertising payload without sitting inside a GAP callback.
static void ticker_task(void *arg)
{
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(200));

        bool     do_busy = false;
        uint8_t  busy_badge[6];
        uint16_t busy_req_id = 0;
        bool     expire = false;

        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (s_busy_pending) {
            s_busy_pending = false;
            do_busy = true;
            busy_req_id = s_busy_req_id;
            memcpy(busy_badge, s_busy_badge, 6);
        } else if (s_resp_active && esp_timer_get_time() > s_resp_until_us) {
            s_resp_active = false;
            expire = true;
        }
        xSemaphoreGive(s_lock);

        if (do_busy) {
            ESP_LOGW(TAG, "queue full, telling %04x we're busy", busy_req_id);
            xSemaphoreTake(s_lock, portMAX_DELAY);
            s_busy_sent++;
            s_last_status = BBX_ST_BUSY;
            s_have_last = true;
            xSemaphoreGive(s_lock);
            broadcast_response(busy_badge, busy_req_id, BBX_ST_BUSY, 0, "", 0);
        } else if (expire) {
            ESP_LOGI(TAG, "response un-ACKed, back to beacon");
            broadcast_beacon();
        }
        screen_refresh();
    }
}

// A button press reselects the station id at runtime: re-beacon immediately
// when idle so badges see the new id without waiting out a response hold,
// and let the 200 ms ticker repaint the screen from box_id_current().
// Re-broadcast the beacon when idle (button task context).
void relay_beacon_refresh(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool idle = !s_resp_active;
    xSemaphoreGive(s_lock);
    if (idle) broadcast_beacon();
}

static void on_box_id_changed(void)
{
    relay_beacon_refresh();
    screen_show(box_id_current(), "ready", -1, 0);
}

static void run_selftest(void)
{
#if CONFIG_RELAY_SELFTEST_ON_BOOT
    static net_post_result_t result;
    // Match the current webhook contract without using an attendee's identity.
    const char *body = "{\"box\":\"selftest\",\"user_id\":\"relay-selftest\",\"name\":\"relay\"}";
    ESP_LOGI(TAG, "self-test POST to %s", CONFIG_RELAY_WEBHOOK_URL);
    screen_show("selftest", "POST", -1, 0);
    net_post_json(CONFIG_RELAY_WEBHOOK_URL, body, strlen(body), &result);
    if (result.status == BBX_ST_OK) {
        ESP_LOGI(TAG, "self-test OK: %s", result.body);
        screen_show("selftest", "ok", -1, 0);
    } else {
        ESP_LOGE(TAG, "self-test FAILED status=%u: %s", result.status, result.body);
        char verdict[SCREEN_LINE_MAX + 1];
        verdict_short(result.status, verdict, sizeof(verdict));
        screen_show("selftest", verdict, -1, 0);
    }
    // The ticker takes over the screen a moment later; hold the verdict long
    // enough to read.
    vTaskDelay(pdMS_TO_TICKS(1200));
#endif
}

void app_main(void)
{
    // NVS first: the button editor reads its persisted selection at start.
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    // Screen first, so the box id is up while everything else initialises.
    screen_init();
    // Buttons before anything reads the box id: the selection starts where
    // NVS (or the flashed RELAY_BOX_ID) points.
    buttons_set_changed_cb(on_box_id_changed);
    buttons_start();
    lights_init();
    screen_show(box_id_current(), "boot", -1, 0);

    s_lock = xSemaphoreCreateMutex();
    s_jobs = xQueueCreate(JOB_QUEUE_DEPTH, sizeof(job_t));
    if (!s_lock || !s_jobs) {
        ESP_LOGE(TAG, "out of memory during init");
        return;
    }

    screen_show(box_id_current(), "wifi...", -1, 0);
    ESP_ERROR_CHECK(wifi_sta_start());
    ESP_ERROR_CHECK(net_post_init());
    run_selftest();

    // BLE comes up after WiFi so coexistence is arbitrating from the start
    // rather than having the scan yanked out from under itself mid-association.
    ESP_ERROR_CHECK(ble_link_init(on_frame));
    broadcast_beacon();

    // 8 KB because the TLS handshake in net_post_json() is the deepest thing
    // this task does.
    xTaskCreate(worker_task, "bbx_worker", 8192, NULL, 5, NULL);
    xTaskCreate(ticker_task, "bbx_ticker", 3072, NULL, 4, NULL);
#if CONFIG_RELAY_HEARTBEAT_ENABLE
    heartbeat_start();
#endif

    ESP_LOGI(TAG, "relay ready -> %s", CONFIG_RELAY_WEBHOOK_URL);
}
