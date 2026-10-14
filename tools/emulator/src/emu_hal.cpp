// Browser implementations of the badge HAL. These replace hal_display,
// hal_buttons, hal_accel, hal_nfc, hal_radio, hal_fs and hal_i2c, and stand in
// for the LED strip and sleep drivers underneath the real hal_lights.cpp and
// hal_sleep.cpp. Every other firmware source is compiled unmodified.

#include "emu.h"

#include "hal_accel.h"
#include "hal_buttons.h"
#include "hal_display.h"
#include "hal_fs.h"
#include "hal_i2c.h"
#include "hal_nfc.h"
#include "hal_radio.h"
#include "hal_sleep.h"
#include "ui_theme.h"

#include "esp_bt.h"
#include "esp_heap_caps.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_sleep.h"
#include "led_strip.h"
#include "led_strip_rmt.h"

#include <emscripten.h>

#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <strings.h>

static EmuStatus s_status = {};
EmuStatus &emu_status() { return s_status; }

static uint32_t now_ms() { return (uint32_t)(emu_now_us() / 1000); }

// ---- display ---------------------------------------------------------------

#define LCD_H_RES 320
#define LCD_V_RES 240
// Same 30-row stripes as the SPI panel, so partial-refresh behaviour (and the
// console `shot` stripe layout) matches a badge.
#define LCD_DRAW_ROWS 30

static lv_obj_t *s_root = nullptr;
static lv_display_t *s_disp = nullptr;
static uint8_t s_rgba[LCD_H_RES * LCD_V_RES * 4];
static uint8_t s_stripe_a[LCD_H_RES * LCD_DRAW_ROWS * 2];
static uint8_t s_stripe_b[LCD_H_RES * LCD_DRAW_ROWS * 2];

static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    const int32_t w = lv_area_get_width(area);
    const int32_t h = lv_area_get_height(area);
    const uint32_t stride = lv_draw_buf_width_to_stride((uint32_t)w, LV_COLOR_FORMAT_RGB565);
    for (int32_t y = 0; y < h; ++y) {
        const int32_t dst_y = area->y1 + y;
        if (dst_y < 0 || dst_y >= LCD_V_RES) continue;
        const uint16_t *src = (const uint16_t *)(const void *)(px_map + (size_t)y * stride);
        uint8_t *dst = s_rgba + ((size_t)dst_y * LCD_H_RES + area->x1) * 4;
        for (int32_t x = 0; x < w; ++x, dst += 4) {
            if (area->x1 + x < 0 || area->x1 + x >= LCD_H_RES) continue;
            const uint16_t c = src[x];
            const uint8_t r = (uint8_t)(c >> 11);
            const uint8_t g = (uint8_t)((c >> 5) & 0x3f);
            const uint8_t b = (uint8_t)(c & 0x1f);
            dst[0] = (uint8_t)((r << 3) | (r >> 2));
            dst[1] = (uint8_t)((g << 2) | (g >> 4));
            dst[2] = (uint8_t)((b << 3) | (b >> 2));
            dst[3] = 255;
        }
    }
    s_status.fb_revision++;
    lv_display_flush_ready(disp);
}

void hal_display_init(void)
{
    lv_init();
    lv_tick_set_cb([]() -> uint32_t { return now_ms(); });

    // Panel stays dark until hal_display_show(), as on the ST7789.
    s_status.panel_on = 0;
    for (size_t i = 3; i < sizeof(s_rgba); i += 4) s_rgba[i] = 255;

    s_disp = lv_display_create(LCD_H_RES, LCD_V_RES);
    lv_display_set_color_format(s_disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(s_disp, s_stripe_a, s_stripe_b, sizeof(s_stripe_a),
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(s_disp, flush_cb);

    s_root = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(s_root);
    lv_obj_set_size(s_root, LCD_H_RES, LCD_V_RES);
    lv_obj_set_pos(s_root, 0, 0);
    lv_obj_set_style_bg_color(s_root, lv_color_hex(BadgeTheme::BACKGROUND), 0);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);
}

lv_obj_t *hal_display_root(void) { return s_root; }
lv_display_t *hal_display_lv(void) { return s_disp; }

void hal_display_show(void)
{
    if (!s_disp) return;
    lv_refr_now(nullptr);
    s_status.panel_on = 1;
}

extern "C" EMSCRIPTEN_KEEPALIVE uint8_t *emu_fb_ptr() { return s_rgba; }
extern "C" EMSCRIPTEN_KEEPALIVE EmuStatus *emu_status_ptr()
{
    s_status.sleep_tier = (uint8_t)hal_sleep_tier();
    return &s_status;
}

// ---- buttons ---------------------------------------------------------------

static constexpr uint8_t kButtonCount = (uint8_t)Button::COUNT;
static constexpr size_t kQueueCap = 32;

static const char *kButtonNames[kButtonCount] = {
    "A", "B", "Home", "Down", "Left", "Right", "Up", "Aux1", "Start",
};

static std::function<void(ButtonEvent)> s_dispatcher;
static ButtonEvent s_queue[kQueueCap];
static size_t s_queue_head = 0;
static size_t s_queue_count = 0;
// Level the page currently reports, and the level the firmware has been told
// about. They differ only for presses swallowed during light sleep.
static uint16_t s_level = 0;
static uint16_t s_reported = 0;

static void queue_push(ButtonEvent ev)
{
    if (s_queue_count == kQueueCap) return; // xQueueSend(..., 0) drops when full
    s_queue[(s_queue_head + s_queue_count++) % kQueueCap] = ev;
}

bool emu_start_wake_pending()
{
    const uint16_t start_bit = (uint16_t)(1u << (uint8_t)Button::Start);
    if (s_level & start_bit) return true;
    for (size_t i = 0; i < s_queue_count; ++i) {
        const ButtonEvent &ev = s_queue[(s_queue_head + i) % kQueueCap];
        if (ev.button == Button::Start && ev.kind == ButtonEventKind::Pressed) return true;
    }
    return false;
}

extern "C" EMSCRIPTEN_KEEPALIVE void emu_button(int index, int pressed)
{
    if (index < 0 || index >= kButtonCount) return;
    const uint16_t bit = (uint16_t)(1u << index);
    const bool was = (s_level & bit) != 0;
    if (was == (pressed != 0)) return;
    if (pressed) s_level |= bit;
    else s_level &= (uint16_t)~bit;

    // In light sleep the CPU is stopped and the shift register is not polled,
    // so only START (the wake GPIO) registers. A swallowed press also swallows
    // its release.
    const bool is_start = index == (int)Button::Start;
    if (pressed) {
        if (emu_sleep_frozen() && !is_start) return;
        s_reported |= bit;
        queue_push({ (Button)index, ButtonEventKind::Pressed });
    } else if (s_reported & bit) {
        s_reported &= (uint16_t)~bit;
        queue_push({ (Button)index, ButtonEventKind::Released });
    }
}

static void drain_timer_cb(lv_timer_t *)
{
    while (s_queue_count > 0) {
        const ButtonEvent ev = s_queue[s_queue_head];
        s_queue_head = (s_queue_head + 1) % kQueueCap;
        --s_queue_count;
        if (s_dispatcher) s_dispatcher(ev);
    }
}

void hal_buttons_init()
{
    // Aux1 is a maintained side switch: its position at power-on is the
    // baseline, not a press.
    if (EM_ASM_INT({ return Module.emu && Module.emu.sideSwitch ? 1 : 0; })) {
        const uint16_t bit = (uint16_t)(1u << (uint8_t)Button::Aux1);
        s_level |= bit;
        s_reported |= bit;
    }
    lv_timer_create(drain_timer_cb, 10, nullptr);
    ESP_LOGI("hal_buttons", "buttons ready");
}

void hal_buttons_set_dispatcher(std::function<void(ButtonEvent)> fn)
{
    s_dispatcher = std::move(fn);
}

void hal_buttons_inject_press(Button b)
{
    queue_push({ b, ButtonEventKind::Pressed });
    queue_push({ b, ButtonEventKind::Released });
}

bool hal_buttons_button_from_name(const char *name, Button *out)
{
    if (!name || !out) return false;
    for (uint8_t i = 0; i < kButtonCount; i++) {
        if (strcasecmp(name, kButtonNames[i]) == 0) {
            *out = (Button)i;
            return true;
        }
    }
    return false;
}

// ---- accelerometer ---------------------------------------------------------
//
// The badge rests face-up (1 g on +Z). The page's Shake button drives a burst
// of multi-axis motion that saturates the SC7A20H's +/-2 g range, as a real
// shake does, so every consumer sees it: gesture shake/tap, the Connect bump
// detector, and the 67 app's Z-axis rep counter.

static bool s_accel_suspended = false;
static int64_t s_shake_start_us = 0;
static int64_t s_shake_until_us = 0;
static float s_gravity[3] = { 0.0f, 0.0f, 1000.0f };

extern "C" EMSCRIPTEN_KEEPALIVE void emu_shake(int duration_ms)
{
    const int64_t now = emu_now_us();
    if (now >= s_shake_until_us) s_shake_start_us = now;
    const int64_t until = now + (int64_t)duration_ms * 1000;
    if (until > s_shake_until_us) s_shake_until_us = until;
}

extern "C" EMSCRIPTEN_KEEPALIVE void emu_set_gravity(float x_mg, float y_mg, float z_mg)
{
    s_gravity[0] = x_mg;
    s_gravity[1] = y_mg;
    s_gravity[2] = z_mg;
}

static float clamp_axis(float mg)
{
    // 12-bit, 1 mg/digit.
    return mg < -2048.0f ? -2048.0f : (mg > 2047.0f ? 2047.0f : mg);
}

static float sensor_noise()
{
    // A few mg of jitter, like the real part at 100 Hz.
    return (float)((int)(esp_random() % 9) - 4);
}

static AccelSample sample_now()
{
    AccelSample s = { s_gravity[0], s_gravity[1], s_gravity[2] };
    const int64_t now = emu_now_us();
    if (now < s_shake_until_us) {
        const float t = (float)(now - s_shake_start_us) / 1e6f;
        const float tau = 6.2831853f;
        s.x_mg += 2600.0f * sinf(tau * 6.5f * t);
        s.y_mg += 2100.0f * sinf(tau * 5.0f * t + 1.1f);
        s.z_mg += 1400.0f * sinf(tau * 8.0f * t + 2.3f);
    }
    s.x_mg = clamp_axis(roundf(s.x_mg + sensor_noise()));
    s.y_mg = clamp_axis(roundf(s.y_mg + sensor_noise()));
    s.z_mg = clamp_axis(roundf(s.z_mg + sensor_noise()));
    return s;
}

esp_err_t hal_accel_init() { return ESP_OK; }
esp_err_t hal_accel_set_output_rate(AccelOutputRate) { return ESP_OK; }

esp_err_t hal_accel_read(AccelSample *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    *out = sample_now();
    return ESP_OK;
}

void hal_accel_set_suspended(bool suspended) { s_accel_suspended = suspended; }

esp_err_t hal_accel_cached_read(AccelSample *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    if (s_accel_suspended) return ESP_ERR_INVALID_STATE;
    *out = sample_now();
    return ESP_OK;
}

// ---- LEDs ------------------------------------------------------------------

static constexpr uint32_t kLedCount = 6;

struct EmuLedStrip {
    uint8_t pending[kLedCount][3];
};
static EmuLedStrip s_strip;

extern "C" esp_err_t led_strip_new_rmt_device(const led_strip_config_t *config,
                                              const led_strip_rmt_config_t *,
                                              led_strip_handle_t *ret_strip)
{
    if (!config || !ret_strip || config->max_leds != kLedCount) return ESP_ERR_INVALID_ARG;
    memset(&s_strip, 0, sizeof(s_strip));
    *ret_strip = &s_strip;
    return ESP_OK;
}

extern "C" esp_err_t led_strip_set_pixel(led_strip_handle_t strip, uint32_t index,
                                         uint32_t red, uint32_t green, uint32_t blue)
{
    if (!strip || index >= kLedCount) return ESP_ERR_INVALID_ARG;
    strip->pending[index][0] = (uint8_t)red;
    strip->pending[index][1] = (uint8_t)green;
    strip->pending[index][2] = (uint8_t)blue;
    return ESP_OK;
}

extern "C" esp_err_t led_strip_refresh(led_strip_handle_t strip)
{
    if (!strip) return ESP_ERR_INVALID_ARG;
    memcpy(s_status.leds, strip->pending, sizeof(s_status.leds));
    s_status.led_revision++;
    return ESP_OK;
}

// The real driver clears its pixel buffer and transmits it.
extern "C" esp_err_t led_strip_clear(led_strip_handle_t strip)
{
    if (!strip) return ESP_ERR_INVALID_ARG;
    memset(strip->pending, 0, sizeof(strip->pending));
    return led_strip_refresh(strip);
}

// ---- NFC -------------------------------------------------------------------
//
// The page "taps" a tag by holding it over the antenna for a moment. The
// reader behaves like the MFRC522 HAL: off by default, a software duty cycle
// (750 ms on / 750 ms off) driven by hal_nfc_last_card() polls, and a latched
// card that holds the field up until the app clears it.

static constexpr uint32_t kFieldOnMs = 750;
static constexpr uint32_t kFieldOffMs = 750;
// hal_nfc_read_text() reads 64 bytes of tag memory. After the NDEF TLV and
// text-record headers that leaves room for this much text; a longer record
// does not fit the window and reads back as "no text record".
static constexpr size_t kNfcMaxText = 55;
static const char *kJournalPath = "/littlefs/nfc_journal.txt";

enum EmuTagKind : int {
    kTagText = 0,       // NTAG with one NDEF text record
    kTagBlank = 1,      // readable, but no NDEF text record
    kTagUnreadable = 2, // seen, then pulled away before the read finished
};

static bool s_nfc_running = false;
static bool s_field_on = false;
static uint32_t s_phase_start_ms = 0;
static bool s_have_card = false;
static NfcCard s_last_card = {};
static char s_last_text[256] = {};
static int s_last_kind = kTagText;

static struct {
    bool present;
    bool reported; // already raised an "active" event in this field-on window
    int64_t until_us;
    NfcCard card;
    char text[256];
    int kind;
} s_tag;

bool emu_nfc_running() { return s_nfc_running; }

static void nfc_publish_state()
{
    s_status.nfc_state = !s_nfc_running ? 0 : (s_field_on ? 2 : 1);
}

void hal_nfc_journal(const char *stage)
{
    if (!stage) return;
    FILE *f = fopen(kJournalPath, "a");
    if (!f) return;
    fprintf(f, "%lu %s\n", (unsigned long)now_ms(), stage);
    fclose(f);
}

static void field_up()
{
    if (s_field_on) return;
    s_field_on = true;
    s_phase_start_ms = now_ms();
    // A tag still resting on the antenna is re-activated by the new field.
    s_tag.reported = false;
    nfc_publish_state();
}

static void field_down()
{
    if (!s_field_on) return;
    s_field_on = false;
    s_phase_start_ms = now_ms();
    nfc_publish_state();
}

static void tag_poll()
{
    if (s_tag.present && emu_now_us() >= s_tag.until_us) s_tag.present = false;
    if (!s_nfc_running || !s_field_on || !s_tag.present || s_tag.reported) return;
    s_tag.reported = true;
    s_last_card = s_tag.card;
    s_last_kind = s_tag.kind;
    snprintf(s_last_text, sizeof(s_last_text), "%s", s_tag.text);
    s_have_card = true;
}

static void duty_tick(bool have_card)
{
    if (!s_nfc_running) return;
    if (have_card) {
        if (!s_field_on) field_up();
        s_phase_start_ms = now_ms();
        return;
    }
    const uint32_t elapsed = now_ms() - s_phase_start_ms;
    if (s_field_on && elapsed >= kFieldOnMs) field_down();
    else if (!s_field_on && elapsed >= kFieldOffMs) field_up();
}

esp_err_t hal_nfc_enable()
{
    struct stat st;
    if (stat(kJournalPath, &st) == 0 && st.st_size > 4096) unlink(kJournalPath);
    hal_nfc_journal("enable");
    hal_accel_set_suspended(true);
    field_up();
    hal_nfc_journal("field-up-ok");
    s_nfc_running = true;
    nfc_publish_state();
    return ESP_OK;
}

esp_err_t hal_nfc_probe(uint8_t *version_out)
{
    if (version_out) *version_out = 0x92; // genuine MFRC522 v2.0
    return ESP_OK;
}

void hal_nfc_disable()
{
    if (!s_nfc_running) return;
    field_down();
    s_nfc_running = false;
    hal_accel_set_suspended(false);
    nfc_publish_state();
}

bool hal_nfc_is_enabled() { return s_nfc_running; }

void hal_nfc_field_down()
{
    if (!s_nfc_running) return;
    field_down();
}

bool hal_nfc_last_card(NfcCard *out)
{
    tag_poll();
    const bool have = s_have_card;
    duty_tick(have);
    tag_poll(); // a field that just came up sees a waiting tag at once
    if (!s_have_card) return false;
    if (out) *out = s_last_card;
    return true;
}

void hal_nfc_clear_last_card() { s_have_card = false; }

esp_err_t hal_nfc_read_text(char *out, size_t out_sz)
{
    if (!out || out_sz < 16) return ESP_ERR_INVALID_ARG;
    out[0] = '\0';
    if (!s_have_card) return ESP_ERR_NOT_FOUND;
    if (s_nfc_running && !s_field_on) field_up();
    if (s_last_kind == kTagUnreadable) return ESP_ERR_TIMEOUT;
    if (s_last_kind == kTagBlank) return ESP_ERR_NOT_FOUND;

    size_t len = strlen(s_last_text);
    if (len > kNfcMaxText) return ESP_ERR_NOT_FOUND;
    if (len > out_sz - 1) len = out_sz - 1;
    memcpy(out, s_last_text, len);
    out[len] = '\0';
    while (len > 0) {
        const char c = out[len - 1];
        if (c != ' ' && c != '\r' && c != '\n' && c != '\t') break;
        out[--len] = '\0';
    }
    return ESP_OK;
}

// Returns the reader state at the moment of the tap (0 means the field is off
// and nothing will read the tag), so the page can say so.
extern "C" EMSCRIPTEN_KEEPALIVE int emu_nfc_tap(const char *text, int kind, int hold_ms)
{
    memset(&s_tag, 0, sizeof(s_tag));
    s_tag.present = true;
    s_tag.until_us = emu_now_us() + (int64_t)hold_ms * 1000;
    s_tag.kind = kind;
    snprintf(s_tag.text, sizeof(s_tag.text), "%s", text ? text : "");

    // NTAG21x: 7-byte UID starting with NXP's manufacturer code. Derived from
    // the payload so the same sticker always presents the same UID.
    uint32_t hash = 2166136261u;
    for (const char *p = s_tag.text; *p; ++p) hash = (hash ^ (uint8_t)*p) * 16777619u;
    hash ^= (uint32_t)kind * 0x9E3779B9u;
    s_tag.card.uid_len = 7;
    s_tag.card.uid[0] = 0x04;
    for (int i = 1; i < 7; ++i) {
        s_tag.card.uid[i] = (uint8_t)(hash >> ((i - 1) * 5));
    }
    s_tag.card.sak = 0x00;
    s_tag.card.atqa = 0x0044;
    return s_status.nfc_state;
}

// ---- radio -----------------------------------------------------------------
//
// BLE extended advertising, modelled at the level the apps use it: one
// persistent manufacturer-data payload re-broadcast every advertising
// interval, and a duty-cycled passive scan. The page relays frames between
// emulator windows, so two windows are two badges in range of each other.

static constexpr uint32_t kAdvUnitsMin = 0x20;      // 20 ms
static constexpr uint32_t kAdvUnitsMax = 0x4000;    // 10.24 s
static constexpr uint32_t kScanUnitsMin = 0x0004;   // 2.5 ms
static constexpr uint32_t kScanUnitsMax = 0x4000;   // 10.24 s
static constexpr uint32_t kAdvItvlMin = 320;        // 200 ms
static constexpr uint32_t kAdvItvlMax = 352;        // 220 ms
static constexpr uint32_t kScanItvl = 240;          // 150 ms
static constexpr uint32_t kScanWindow = 48;         // 30 ms

static constexpr uint32_t ms_to_units(uint32_t ms) { return (ms * 8 + 2) / 5; }
static uint32_t clamp_units(uint32_t v, uint32_t lo, uint32_t hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static bool s_radio_ever_up = false;
static bool s_radio_running = false;
static bool s_advertising = false;
static RadioRecvCb s_recv_cb;
static uint8_t s_payload[kHalRadioMaxPayload];
static size_t s_payload_len = 0;
static uint32_t s_adv_itvl_min = kAdvItvlMin;
static uint32_t s_adv_itvl_max = kAdvItvlMax;
static uint32_t s_scan_itvl = kScanItvl;
static uint32_t s_scan_window = kScanWindow;
static int64_t s_next_adv_us = 0;

EM_JS(void, emu_js_radio_tx, (const uint8_t *mac, const uint8_t *data, int len), {
    if (Module.emu && Module.emu.radioTx) {
        Module.emu.radioTx(HEAPU8.slice(mac, mac + 6), HEAPU8.slice(data, data + len));
    }
});

static void radio_publish_state()
{
    s_status.radio_state = !s_radio_running ? 0 : (s_advertising ? 2 : 1);
    s_status.adv_interval_ms = s_adv_itvl_min * 5 / 8;
    s_status.scan_duty_pct = s_scan_itvl ? s_scan_window * 100 / s_scan_itvl : 0;
}

static void schedule_next_adv()
{
    // advInterval is chosen in [min, max]; the controller adds 0-10 ms of
    // advDelay to every event so two advertisers cannot stay phase-locked.
    const uint32_t span = s_adv_itvl_max - s_adv_itvl_min;
    const uint32_t units = s_adv_itvl_min + (span ? esp_random() % (span + 1) : 0);
    s_next_adv_us = emu_now_us() + (int64_t)units * 625 + (int64_t)(esp_random() % 10000);
}

void emu_radio_poll()
{
    if (!s_radio_running || !s_advertising) return;
    if (emu_now_us() < s_next_adv_us) return;
    schedule_next_adv();
    // The post-enable placeholder carries no payload; receivers drop it.
    if (s_payload_len == 0) return;
    uint8_t mac[6];
    hal_radio_local_mac(mac);
    emu_js_radio_tx(mac, s_payload, (int)s_payload_len);
    s_status.tx_count++;
}

extern "C" EMSCRIPTEN_KEEPALIVE void emu_radio_rx(const uint8_t *mac, int rssi,
                                                  const uint8_t *data, int len)
{
    if (!s_radio_running || !mac || !data || len <= 0
        || (size_t)len > kHalRadioMaxPayload) return;
    // A scanner listening for `window` out of every `interval` catches about
    // that fraction of advertising events.
    if (s_scan_window < s_scan_itvl
        && esp_random() % s_scan_itvl >= s_scan_window) return;
    s_status.rx_count++;
    const RadioRecvCb cb = s_recv_cb;
    if (cb) cb(mac, (int8_t)rssi, data, (size_t)len);
}

esp_err_t hal_radio_enable()
{
    s_radio_ever_up = true;
    if (s_radio_running) return ESP_OK;
    s_payload_len = 0;
    s_advertising = true;
    s_radio_running = true;
    schedule_next_adv();
    radio_publish_state();
    ESP_LOGI("hal_radio", "BLE radio ready");
    return ESP_OK;
}

esp_err_t hal_radio_probe()
{
    s_radio_ever_up = true;
    return ESP_OK;
}

void hal_radio_debug(char *out, size_t capacity)
{
    if (!out || capacity == 0) return;
    snprintf(out, capacity,
             "last_error_stage=none\nlast_error_code=0 (0x0)\n"
             "controller_state=%d\nfree_heap=%u\nlargest_heap_block=%u\n"
             "dma_free=%u dma_largest=%u\n"
             "running=%d advertising=%d adv_active=%d scanning=%d\n",
             (int)esp_bt_controller_get_status(),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_DEFAULT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA),
             (int)s_radio_running, (int)s_advertising, (int)s_advertising,
             (int)s_radio_running);
}

bool hal_radio_is_enabled() { return s_radio_running; }

void hal_radio_disable()
{
    s_advertising = false;
    s_radio_running = false;
    s_recv_cb = RadioRecvCb{};
    hal_radio_reset_timings();
    s_payload_len = 0;
    radio_publish_state();
}

esp_err_t hal_radio_send(const void *data, size_t len)
{
    if (!s_radio_running) return ESP_ERR_INVALID_STATE;
    if (!data || len == 0 || len > sizeof(s_payload)) return ESP_ERR_INVALID_ARG;
    memcpy(s_payload, data, len);
    s_payload_len = len;
    return ESP_OK;
}

esp_err_t hal_radio_adv_pause()
{
    if (!s_radio_running) return ESP_ERR_INVALID_STATE;
    s_advertising = false;
    radio_publish_state();
    return ESP_OK;
}

esp_err_t hal_radio_adv_resume()
{
    if (!s_radio_running) return ESP_ERR_INVALID_STATE;
    if (!s_advertising) {
        s_advertising = true;
        // Starting an instance transmits its first event almost immediately.
        s_next_adv_us = emu_now_us() + (int64_t)(esp_random() % 10000);
    }
    radio_publish_state();
    return ESP_OK;
}

bool hal_radio_is_advertising() { return s_radio_running && s_advertising; }

esp_err_t hal_radio_set_adv_interval_ms(uint32_t min_ms, uint32_t max_ms)
{
    if (min_ms == 0 || max_ms < min_ms) return ESP_ERR_INVALID_ARG;
    const uint32_t lo = clamp_units(ms_to_units(min_ms), kAdvUnitsMin, kAdvUnitsMax);
    uint32_t hi = clamp_units(ms_to_units(max_ms), kAdvUnitsMin, kAdvUnitsMax);
    if (hi < lo) hi = lo;
    if (lo == s_adv_itvl_min && hi == s_adv_itvl_max) return ESP_OK;
    s_adv_itvl_min = lo;
    s_adv_itvl_max = hi;
    // Retuning restarts the instance, which also restarts its schedule.
    if (s_radio_running && s_advertising) schedule_next_adv();
    radio_publish_state();
    return ESP_OK;
}

esp_err_t hal_radio_set_scan_duty_ms(uint32_t interval_ms, uint32_t window_ms)
{
    if (interval_ms == 0 || window_ms == 0) return ESP_ERR_INVALID_ARG;
    const uint32_t itvl = clamp_units(ms_to_units(interval_ms), kScanUnitsMin, kScanUnitsMax);
    uint32_t win = clamp_units(ms_to_units(window_ms), kScanUnitsMin, kScanUnitsMax);
    if (win > itvl) win = itvl;
    s_scan_itvl = itvl;
    s_scan_window = win;
    radio_publish_state();
    return ESP_OK;
}

void hal_radio_reset_timings()
{
    s_adv_itvl_min = kAdvItvlMin;
    s_adv_itvl_max = kAdvItvlMax;
    s_scan_itvl = kScanItvl;
    s_scan_window = kScanWindow;
    if (s_radio_running && s_advertising) schedule_next_adv();
    radio_publish_state();
}

void hal_radio_set_recv_cb(RadioRecvCb cb) { s_recv_cb = std::move(cb); }

// The page gives every emulated badge its own stable "WiFi MAC"; the BLE
// static random address is derived from it exactly as on the chip.
void hal_radio_local_mac(uint8_t out[6])
{
    if (!out) return;
    for (int i = 0; i < 6; ++i) {
        out[i] = (uint8_t)EM_ASM_INT({
            return Module.emu && Module.emu.mac ? Module.emu.mac[$0] | 0 : 0;
        }, i);
    }
    out[5] |= 0xC0;
}

extern "C" esp_bt_controller_status_t esp_bt_controller_get_status(void)
{
    return s_radio_ever_up ? ESP_BT_CONTROLLER_STATUS_ENABLED
                           : ESP_BT_CONTROLLER_STATUS_IDLE;
}

// ---- filesystem ------------------------------------------------------------
//
// /littlefs is an in-memory tree the page fills from its saved flash image
// before boot and writes back as files change.

esp_err_t hal_fs_init()
{
    mkdir("/littlefs", 0775);
    size_t total = 0, used = 0;
    esp_littlefs_info("storage", &total, &used);
    ESP_LOGI("hal_fs", "littlefs mounted at /littlefs (used %u / %u bytes)",
             (unsigned)used, (unsigned)total);
    return ESP_OK;
}

bool hal_fs_mkdir_p(const char *path)
{
    if (!path || !*path) return false;

    char buf[128];
    const size_t len = strlen(path);
    if (len >= sizeof(buf)) return false;
    memcpy(buf, path, len + 1);

    for (size_t i = 1; i < len; i++) {
        if (buf[i] != '/') continue;
        buf[i] = '\0';
        if (mkdir(buf, 0775) != 0 && errno != EEXIST) return false;
        buf[i] = '/';
    }
    return mkdir(buf, 0775) == 0 || errno == EEXIST;
}

bool hal_fs_usage(size_t *used_out, size_t *total_out)
{
    size_t total = 0, used = 0;
    if (esp_littlefs_info("storage", &total, &used) != ESP_OK) return false;
    if (used_out) *used_out = used;
    if (total_out) *total_out = total;
    return true;
}

esp_err_t hal_i2c_init(void) { return ESP_OK; }
i2c_master_bus_handle_t hal_i2c_bus(void) { return nullptr; }

// ---- light sleep -----------------------------------------------------------

static bool s_frozen = false;
static uint64_t s_timer_wakeup_us = 0;
static int64_t s_sleep_deadline_us = 0;
static esp_sleep_wakeup_cause_t s_wake_cause = ESP_SLEEP_WAKEUP_UNDEFINED;

bool emu_sleep_frozen() { return s_frozen; }

void emu_sleep_poll()
{
    if (!s_frozen) return;
    if (emu_start_wake_pending()) {
        s_frozen = false;
        s_wake_cause = ESP_SLEEP_WAKEUP_GPIO;
    } else if (s_timer_wakeup_us && emu_now_us() >= s_sleep_deadline_us) {
        s_frozen = false;
        s_wake_cause = ESP_SLEEP_WAKEUP_TIMER;
    }
}

extern "C" esp_err_t esp_sleep_enable_gpio_wakeup(void) { return ESP_OK; }

extern "C" esp_err_t esp_sleep_enable_timer_wakeup(uint64_t time_in_us)
{
    s_timer_wakeup_us = time_in_us;
    return ESP_OK;
}

extern "C" esp_err_t esp_light_sleep_start(void)
{
    // START is a level-triggered wake source: held low, the chip does not
    // stay asleep.
    if (emu_start_wake_pending()) {
        s_wake_cause = ESP_SLEEP_WAKEUP_GPIO;
        return ESP_OK;
    }
    s_frozen = true;
    s_sleep_deadline_us = emu_now_us() + (int64_t)s_timer_wakeup_us;
    // The caller reads the cause straight after this returns, before the
    // freeze takes effect; "timer" is the cause it treats as "keep sleeping".
    s_wake_cause = ESP_SLEEP_WAKEUP_TIMER;
    return ESP_OK;
}

extern "C" esp_sleep_wakeup_cause_t esp_sleep_get_wakeup_cause(void)
{
    return s_wake_cause;
}
