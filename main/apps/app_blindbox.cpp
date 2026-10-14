#include "app.h"
#include "app_launcher.h"
#include "bbx_proto.h"
#include "blindbox_web_serial.h"
#include "hal_config.h"
#include "hal_radio.h"
#include "json_util.h"

#include "cJSON.h"
#include "hal_identity.h"
#include "solana_wallet.h"
#include "pal_catalog.h"
#include "opened_boxes.h"
#include "pal_picker.h"
#include "hal_lights.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <cstdio>
#include <cstring>

// Solana Scavenger Hunt. One trigger: walk up to a station.
//
// The app listens for relay beacons and fires when the smoothed RSSI says
// you're close enough. The radio scans in idle; the relay POSTs the request
// to the quest server and broadcasts the reply back.
//
// Connectionless BLE 5 extended advertising through hal_radio (see
// bbx_proto.h and tools/relay_firmware). The badge has no WiFi — the relay
// owns HTTP because the badge can't reach the server on its own.
namespace {

constexpr char kSlug[] = "blindbox";

// Palette matches app_launcher.cpp.
constexpr uint32_t COLOR_BG      = BadgeTheme::BACKGROUND;
constexpr uint32_t COLOR_PANEL   = BadgeTheme::PANEL;
constexpr uint32_t COLOR_BORDER  = BadgeTheme::BORDER;
constexpr uint32_t COLOR_ACCENT  = BadgeTheme::ACCENT_DETAIL;
constexpr uint32_t COLOR_TEXT    = BadgeTheme::TEXT;
constexpr uint32_t COLOR_DIM     = BadgeTheme::TEXT_MUTED;
constexpr uint32_t COLOR_GOLD    = 0xffd166;
constexpr uint32_t COLOR_MAGENTA = 0xff5c8a;
constexpr uint32_t COLOR_GREEN   = 0x36e07a;

// Registry tick timer runs at 50 Hz, so 1 tick = 20 ms.
constexpr int OPEN_TIMEOUT_TICKS = 600;  // 12 s waiting for a response
constexpr int ACK_TICKS          = 50;   // 1 s of ACK bursts before radio off
constexpr int PULSE_PERIOD       = 40;   // 0.8 s border pulse while opening

// Longest box id we accept. BBX_MAX_BOX_ID (39) is this minus the terminator;
// they must agree.
constexpr size_t kBoxIdCap = 40;
static_assert(kBoxIdCap == BBX_MAX_BOX_ID + 1, "box id caps must agree");

// --- Proximity trigger -----------------------------------------------------
//
// Averaging window for beacon RSSI. Long enough to smooth out the 10-20 dB
// swings you get from body position and badge orientation, short enough that
// walking up to a box still feels immediate.
constexpr uint32_t kSampleWindowMs = 1500;

// One lucky packet shouldn't open a box.
constexpr int kMinSamples = 3;

// How many distinct boxes we track at once. Stations placed near each other
// are expected; strongest average wins.
constexpr int kMaxCandidates = 4;

// Re-arm needs BOTH: the cooldown to elapse, and the signal to fall this far
// below the trigger threshold. Hysteresis stops flapping right at the edge,
// the cooldown stops a loop while someone lingers.
constexpr uint32_t kRearmCooldownMs = 5000;
constexpr int      kRearmHysteresis = 10;   // dB

// Trigger threshold in dBm. Roughly arm's length to ~1 m at the badge's
// -3 dBm TX power. Fixed — no live tuning.
constexpr int kRssiMin = -60;

// The scan report's "no reading" sentinel. Must be rejected explicitly — as a
// raw number it would look like an impossibly strong signal.
constexpr int8_t kRssiUnavailable = 127;

// json_escape() now comes from json_util.h, shared with the Sync app's
// snapshot builder. Two behaviour changes versus the local version this
// replaces, both strictly more correct:
//
//   * Control characters are \u00XX-escaped rather than dropped. The old
//     comment argued box ids and card names have no business containing them,
//     which is true, but "drop it" is the wrong response to a field that
//     turned out to contain one.
//   * Malformed UTF-8 is dropped rather than passed through. A display_name
//     with a truncated multi-byte sequence used to produce a body the server
//     could not parse at all; now it loses one character instead.
//
// A control character expands from 1 byte to 6, so the escaped-length
// assumptions at the call sites matter: the shared helper truncates within the
// buffer and never emits a partial escape, so the worst case is a short field,
// not a corrupt request.

const char *pal_name(const char *id)
{
    static char fallback[32];
    const PalDefinition *pal = pal_catalog_find_item(id);
    if (pal) return pal->name;
    snprintf(fallback, sizeof(fallback), "Reward %s", id);
    return fallback;
}

// Sentinel: the quest server sends new_item "0" when the attendee must
// finish on their phone to earn the final pal. It is not a real inventory
// id, so it never enters the wallet and never looks up the pal catalog.
constexpr char kFinalQuestItem[] = "0";
constexpr char kFinalQuestMsg[] =
    "Complete the quest at the QR code below to get your final pal!";

// Copy a cJSON string field into a fixed buffer. Leaves `out` empty if the
// field is missing or isn't a string. Numbers are accepted too so a bare
// 0 from the server still reads as the final-quest sentinel above.
void copy_json_str(const cJSON *obj, const char *key, char *out, size_t out_sz)
{
    out[0] = '\0';
    const cJSON *f = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsString(f) && f->valuestring) {
        snprintf(out, out_sz, "%s", f->valuestring);
    } else if (cJSON_IsNumber(f)) {
        snprintf(out, out_sz, "%d", (int)f->valuedouble);
    }
}

extern "C" const lv_image_dsc_t icon_blindbox;

class BlindBoxApp : public App {
public:
    const char *name() const override { return "Solana Scavenger Hunt"; }
    const char *icon() const override { return "SOL"; }
    const lv_image_dsc_t *icon_image() const override { return &icon_blindbox; }
    const char *slug() const override { return kSlug; }
    AppAccess access() const override { return AppAccess::Default; }

    // The radio scans the whole time this app is open so a station fires the
    // moment you're close — don't let the sleep manager dim mid-hunt.
    bool wants_wake_lock() const override { return true; }

    void on_enter(lv_obj_t *root) override
    {
        if (!rx_mutex_) rx_mutex_ = xSemaphoreCreateMutex();

        // Fresh entry into the app is always armed — the cooldown exists only
        // to stop a just-opened box re-firing.
        armed_ = true;
        cooldown_until_ms_ = 0;

        root_ = root;
        lv_obj_set_style_bg_color(root_, lv_color_hex(COLOR_BG), 0);
        lv_obj_set_style_bg_opa(root_, LV_OPA_COVER, 0);

        view_ = View::Hunt;
        if (!hal_config_get_int(kSlug, "intro_seen", 0)) show_help();
        else {
            load_opened_boxes();
            enter_idle();
            // Back to the last QR if there is one; A drops into the hunt.
            restore_last_reveal();
        }
    }

    void on_exit() override
    {
        // Registry disables the radio and NFC for us on the way out, but the
        // callback holds a `this` pointer — drop it first.
        hal_radio_set_recv_cb(RadioRecvCb{});
        clear_rx_slot();
        active_req_id_ = 0;

        title_ = panel_ = status_ = detail_ = hint_ = relay_dot_ = nullptr;
        root_ = nullptr;
    }

    void on_tick() override
    {
        if (view_ != View::Hunt) return;
        switch (state_) {
            case State::Idle:    tick_idle();    break;
            case State::Opening: tick_opening(); break;
            case State::Reveal:  tick_reveal();  break;
            case State::Failed:                  break;
        }
    }

    void on_button(ButtonEvent ev) override
    {
        if (ev.kind != ButtonEventKind::Pressed) return;

        if (view_ == View::Help) {
            if (ev.button == Button::A || ev.button == Button::B || ev.button == Button::Start) {
                hal_config_set_int(kSlug, "intro_seen", 1);
                enter_idle();
            }
            return;
        }
        if (view_ == View::Inventory) {
            if (picker_.on_button(ev)) enter_idle();
            return;
        }
        if (state_ == State::Idle || state_ == State::Failed) {
            if (ev.button == Button::Start) { show_help(); return; }
            if (ev.button == Button::B || (ev.button == Button::A && state_ == State::Idle)) {
                prepare_page(View::Inventory);
                picker_.show(root_);
                return;
            }
        }

        // B opens the link QR on a reveal that carries one, and dismisses it.
        if (ev.button == Button::B && (state_ == State::Reveal || showing_qr_)) {
            if (showing_qr_) render_reveal();
            else if (reveal_link_[0]) show_qr();
            return;
        }

        if (ev.button != Button::A) return;
        if (state_ == State::Reveal || state_ == State::Failed || showing_qr_) {
            enter_idle();
        }
    }

private:
    enum class State { Idle, Opening, Reveal, Failed };
    enum class View { Hunt, Help, Inventory };
    View view_ = View::Hunt;
    PalPicker picker_;

    void prepare_page(View view)
    {
        // Only entered from idle/failure: never interrupt a request or ACK.
        hal_radio_set_recv_cb(RadioRecvCb{});
        clear_rx_slot();
        view_ = view;
        lv_obj_clean(root_);
        title_ = panel_ = status_ = detail_ = hint_ = relay_dot_ = nullptr;
    }

    void show_help()
    {
        prepare_page(View::Help);
        lv_obj_t *title = lv_label_create(root_);
        lv_label_set_text(title, "Solana Scavenger Hunt");
        lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(title, lv_color_hex(COLOR_TEXT), 0);
        lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 9);
        lv_obj_t *body = lv_label_create(root_);
        lv_obj_set_width(body, 292);
        lv_label_set_text(body,
            "Find boxes with the Solana logo around the venue.\n\n"
            "Keep this app open and tap your badge against a box.\n\n"
            "Unlock exclusive pals to customize your My Badge QR code page!");
        lv_obj_set_style_text_font(body, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(body, lv_color_hex(COLOR_TEXT), 0);
        lv_obj_align(body, LV_ALIGN_TOP_MID, 0, 43);
        lv_obj_t *hint = lv_label_create(root_);
        lv_label_set_text(hint, "A: start hunting   Start: help anytime");
        lv_obj_set_style_text_font(hint, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(hint, lv_color_hex(COLOR_DIM), 0);
        lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -6);
    }

    // A box we're currently hearing beacons from. Smoothed rather than used
    // raw, because single-packet RSSI is far too noisy to gate on.
    //
    // An exponential moving average rather than a windowed mean: a plain sum
    // would average over the entire time you stand there, so it would lag
    // badly when you walk up. The EMA weights recent packets and needs no
    // window bookkeeping. Stored as RSSI * 16 to keep a little precision.
    struct BeaconSample {
        char     box_id[kBoxIdCap];
        int32_t  ema_q4;
        int16_t  count;
        uint32_t last_ms;
    };

    static int ema_dbm(const BeaconSample &c) { return (int)(c.ema_q4 >> 4); }

    static uint32_t now_ms()
    {
        return (uint32_t)(esp_timer_get_time() / 1000);
    }

    // ---- UI ------------------------------------------------------------

    void build_chrome()
    {
        title_ = lv_label_create(root_);
        lv_label_set_text(title_, name());
        lv_obj_set_style_text_color(title_, lv_color_hex(COLOR_TEXT), 0);
        lv_obj_set_style_text_font(title_, &lv_font_montserrat_20, 0);
        lv_obj_align(title_, LV_ALIGN_TOP_MID, 0, 10);

        panel_ = lv_obj_create(root_);
        lv_obj_remove_style_all(panel_);
        lv_obj_set_size(panel_, 280, 132);
        lv_obj_align(panel_, LV_ALIGN_CENTER, 0, 10);
        lv_obj_set_style_bg_color(panel_, lv_color_hex(COLOR_PANEL), 0);
        lv_obj_set_style_bg_opa(panel_, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(panel_, 0, 0);
        lv_obj_set_style_border_width(panel_, 2, 0);
        lv_obj_set_style_border_color(panel_, lv_color_hex(COLOR_BORDER), 0);
        lv_obj_clear_flag(panel_, LV_OBJ_FLAG_SCROLLABLE);

        status_ = lv_label_create(panel_);
        lv_label_set_long_mode(status_, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(status_, 252);
        lv_obj_set_style_text_align(status_, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_font(status_, &lv_font_montserrat_20, 0);
        lv_obj_align(status_, LV_ALIGN_CENTER, 0, -14);

        detail_ = lv_label_create(panel_);
        lv_label_set_long_mode(detail_, LV_LABEL_LONG_DOT);
        lv_obj_set_size(detail_, 252, 42);
        lv_obj_set_style_text_align(detail_, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(detail_, lv_color_hex(COLOR_DIM), 0);
        lv_obj_set_style_text_font(detail_, &lv_font_montserrat_12, 0);
        lv_obj_align(detail_, LV_ALIGN_CENTER, 0, 32);

        hint_ = lv_label_create(root_);
        lv_obj_set_width(hint_, 304);
        lv_label_set_long_mode(hint_, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_align(hint_, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(hint_, lv_color_hex(COLOR_DIM), 0);
        lv_obj_set_style_text_font(hint_, &lv_font_montserrat_12, 0);
        lv_obj_align(hint_, LV_ALIGN_BOTTOM_MID, 0, -6);

        // Lights up once we hear a relay BEACON, so you can tell "no relay in
        // range" apart from "relay heard me but the request failed".
        relay_dot_ = lv_label_create(root_);
        lv_label_set_text(relay_dot_, "relay");
        lv_obj_set_style_text_font(relay_dot_, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(relay_dot_, lv_color_hex(COLOR_BORDER), 0);
        lv_obj_align(relay_dot_, LV_ALIGN_TOP_RIGHT, -8, 40);
    }

    // Always restores the default font — the reveal screen bumps it to 24 and
    // every other state needs it back at 20.
    void set_status(const char *s, uint32_t color)
    {
        if (!status_) return;
        lv_label_set_text(status_, s);
        lv_obj_set_style_text_color(status_, lv_color_hex(color), 0);
        lv_obj_set_style_text_font(status_, &lv_font_montserrat_20, 0);
    }

    void set_detail(const char *s)
    {
        if (detail_) lv_label_set_text(detail_, s ? s : "");
    }

    void set_idle_prompt()
    {
        set_status("Bring your badge close to a\nSolana Scavenger Hunt box",
                   COLOR_ACCENT);
        lv_obj_set_style_text_font(status_, &lv_font_montserrat_16, 0);
    }

    void set_hint(const char *s)
    {
        if (hint_) lv_label_set_text(hint_, s ? s : "");
    }

    void set_border(uint32_t color)
    {
        if (panel_) lv_obj_set_style_border_color(panel_, lv_color_hex(color), 0);
    }

    void set_relay_seen(bool seen)
    {
        if (!relay_dot_) return;
        lv_obj_set_style_text_color(
            relay_dot_, lv_color_hex(seen ? COLOR_GREEN : COLOR_BORDER), 0);
    }

    // ---- States --------------------------------------------------------

    void enter_idle()
    {
        view_ = View::Hunt;
        // The QR view owns the whole screen. Rebuild the normal chrome when
        // A starts another round.
        if (!panel_ && root_) {
            lv_obj_clean(root_);
            build_chrome();
        }
        clear_rx_slot();
        active_req_id_ = 0;
        relay_seen_ = false;
        showing_qr_ = false;
        ticks_ = 0;
        ack_ticks_ = 0;
        detail_shown_[0] = '\0';

        set_border(COLOR_BORDER);
        set_relay_seen(false);
        state_ = State::Idle;

        clear_candidates();

        if (hal_radio_enable() != ESP_OK) {
            enter_failed("Radio wouldn't start", "Reboot the badge");
            return;
        }
        // Radio must be running before generating the wallet's random seed.
        solana_wallet_ensure_keypair();
        hal_radio_local_mac(my_mac_);
        register_rx_cb();

        // Deliberately does NOT touch armed_/cooldown_until_ms_. Opening the
        // app while already standing at a box should fire straight away; a
        // just-opened box is suppressed by the finish_open cooldown and the
        // persisted opened set instead, and that state has to survive the
        // trip back through here.

        set_idle_prompt();
        set_detail("scanning...");
        set_hint("A: pals   Start: how to play   Home: apps");
    }

    void register_rx_cb()
    {
        hal_radio_set_recv_cb([this](const uint8_t *, int8_t rssi,
                                     const uint8_t *data, size_t n) {
            on_radio_rx(rssi, data, n);
        });
    }

    void tick_idle()
    {
        tick_idle_scan();
    }

    void start_open()
    {
        if (!hal_identity_is_provisioned()) {
            enter_failed("Badge isn't registered", "Provision an identity first");
            return;
        }

        if (hal_radio_enable() != ESP_OK) {
            enter_failed("Radio wouldn't start", "Reboot the badge");
            return;
        }
        hal_radio_local_mac(my_mac_);

        // 0 is reserved as "no request in flight".
        do {
            active_req_id_ = (uint16_t)(esp_random() & 0xFFFF);
        } while (active_req_id_ == 0);

        uint8_t frame[BBX_MAX_FRAME];
        size_t len = build_request(frame, sizeof(frame));

        clear_rx_slot();
        register_rx_cb();

        if (hal_radio_send(frame, len) != ESP_OK) {
            enter_failed("Couldn't send", "Reboot the badge");
            return;
        }

        char msg[80];
        snprintf(msg, sizeof(msg), "Opening %s", box_id_);
        set_status(msg, COLOR_GOLD);
        set_detail("");
        set_hint("");
        set_relay_seen(false);
        ticks_ = 0;
        state_ = State::Opening;
    }

    // ---- Beacon sampling --------------------------------------------------

    // Runs on the NimBLE host task, so: try-lock only, no allocation, no LVGL.
    // Accumulates beacons per box; tick_idle_scan() does the averaging and
    // the deciding.
    void sample_beacon(int8_t rssi, const char *id, size_t id_len)
    {
        if (rssi == kRssiUnavailable) return;      // sentinel, not a strong signal
        if (id_len == 0 || id_len > BBX_MAX_BOX_ID) return;  // unconfigured relay

        if (!rx_mutex_) return;
        if (xSemaphoreTake(rx_mutex_, 0) != pdTRUE) return;   // beacons repeat

        uint32_t t = now_ms();

        // Match first across ALL slots, then take a free one, then evict the
        // least recently heard. Breaking early on the first free slot would
        // create a second entry for a box already tracked further along.
        int slot = -1;
        for (int i = 0; i < kMaxCandidates; i++) {
            if (cand_[i].count > 0 && strncmp(cand_[i].box_id, id, id_len) == 0 &&
                cand_[i].box_id[id_len] == '\0') { slot = i; break; }
        }
        if (slot < 0) {
            for (int i = 0; i < kMaxCandidates; i++) {
                if (cand_[i].count == 0) { slot = i; break; }
            }
        }
        if (slot < 0) {
            int oldest = 0;
            for (int i = 1; i < kMaxCandidates; i++) {
                if (cand_[i].last_ms < cand_[oldest].last_ms) oldest = i;
            }
            slot = oldest;
        }

        BeaconSample &c = cand_[slot];
        bool same = c.count > 0 && strncmp(c.box_id, id, id_len) == 0 &&
                    c.box_id[id_len] == '\0';
        if (!same) {
            memcpy(c.box_id, id, id_len);
            c.box_id[id_len] = '\0';
            c.ema_q4 = (int32_t)rssi << 4;
            c.count = 0;
        } else {
            // alpha = 1/4; with ~5 beacons/sec that settles in well under a second.
            c.ema_q4 += (((int32_t)rssi << 4) - c.ema_q4) >> 2;
        }
        if (c.count < 1000) c.count++;
        c.last_ms = t;

        xSemaphoreGive(rx_mutex_);
    }

    void clear_candidates()
    {
        if (!rx_mutex_) return;
        if (xSemaphoreTake(rx_mutex_, portMAX_DELAY) == pdTRUE) {
            for (auto &c : cand_) { c.count = 0; c.ema_q4 = 0; c.box_id[0] = '\0'; }
            xSemaphoreGive(rx_mutex_);
        }
    }

    // Strongest average over the window wins. Returns false if nothing is
    // currently in range with enough samples to judge.
    bool best_candidate(char *out_id, size_t out_sz, int *out_avg)
    {
        bool found = false;
        int best_avg = -127;

        if (!rx_mutex_) return false;
        if (xSemaphoreTake(rx_mutex_, 0) != pdTRUE) return false;

        uint32_t t = now_ms();
        for (auto &c : cand_) {
            if (c.count == 0) continue;
            if (t - c.last_ms > kSampleWindowMs) {   // gone quiet, drop it
                c.count = 0;
                c.ema_q4 = 0;
                c.box_id[0] = '\0';
                continue;
            }
            if (c.count < kMinSamples) continue;
            int avg = ema_dbm(c);
            if (!found || avg > best_avg) {
                best_avg = avg;
                snprintf(out_id, out_sz, "%s", c.box_id);
                found = true;
            }
        }
        xSemaphoreGive(rx_mutex_);

        if (found) *out_avg = best_avg;
        return found;
    }

    void tick_idle_scan()
    {
        char best_id[kBoxIdCap] = {};
        int  best_avg = 0;
        bool have = best_candidate(best_id, sizeof(best_id), &best_avg);

        if (relay_seen_flag_ && !relay_seen_) {
            relay_seen_ = true;
            set_relay_seen(true);
        }

        // Re-arm needs the cooldown AND the signal to have genuinely fallen
        // away, so lingering at a box can't loop it open.
        if (!armed_) {
            bool cooled = (int32_t)(now_ms() - cooldown_until_ms_) >= 0;
            bool walked_away = !have || best_avg < kRssiMin - kRearmHysteresis;
            if (cooled && walked_away) armed_ = true;
        }

        // An opened box never fires again on this badge — and failures
        // never record, so an errored box stays immediately retryable.
        // The replayable final booth is the exception: it fires every visit.
        bool dup = have && !OpenedBoxes::replayable(best_id) &&
                   opened_boxes_.suppressed(best_id);

        // Repainting every tick is wasteful; only touch LVGL when it changes.
        char line[80];
        if (dup) {
            snprintf(line, sizeof(line), "%s opened - try another", best_id);
        } else if (have) {
            snprintf(line, sizeof(line), "%s   %d dBm", best_id, best_avg);
        } else {
            snprintf(line, sizeof(line), "no boxes in range");
        }
        if (strcmp(line, detail_shown_) != 0) {
            snprintf(detail_shown_, sizeof(detail_shown_), "%s", line);
            set_detail(line);
        }

        if (!armed_ || !have || dup) return;
        if (best_avg < kRssiMin) return;

        snprintf(box_id_, sizeof(box_id_), "%s", best_id);
        start_open();
    }

    // Body is the request JSON. The relay never looks inside it — it POSTs
    // these bytes verbatim to its configured webhook.
    size_t build_request(uint8_t *frame, size_t cap)
    {
        bbx_hdr_t hdr = {};
        bbx_set_magic(&hdr);
        hdr.type   = BBX_TYPE_REQ;
        hdr.req_id = active_req_id_;
        hdr.seq    = 0;
        hdr.total  = 1;
        memcpy(hdr.badge, my_mac_, 6);
        memcpy(frame, &hdr, sizeof(hdr));

        // Contract agreed with Solana, who own the server side:
        //   { box, user_id, name, public_key, email }
        // user_id is the HTN attendee id as a string — deliberately not the
        // BLE MAC, which the previous revision sent under the key "badge".
        const BadgeIdentity *id = hal_identity_get();

        char box_esc[kBoxIdCap * 2 + 2];
        char name_esc[sizeof(id->display_name) * 2 + 2];
        char mail_esc[sizeof(id->account_email) * 2 + 2];
        json_escape(box_esc, sizeof(box_esc), box_id_);
        json_escape(name_esc, sizeof(name_esc), id->display_name);
        json_escape(mail_esc, sizeof(mail_esc), id->account_email);

        // base58 is JSON-safe by construction, but escape anyway rather than
        // trusting a field we may later populate from elsewhere.
        char pk_esc[128];
        json_escape(pk_esc, sizeof(pk_esc), solana_public_key());

        char *body = (char *)(frame + sizeof(hdr));
        int n = snprintf(body, cap - sizeof(hdr),
                         "{\"box\":\"%s\",\"user_id\":\"%lu\",\"name\":\"%s\","
                         "\"public_key\":\"%s\",\"email\":\"%s\"}",
                         box_esc, (unsigned long)id->attendee_id, name_esc,
                         pk_esc, mail_esc);
        if (n < 0) n = 0;
        size_t body_len = (size_t)n;
        if (body_len > cap - sizeof(hdr)) body_len = cap - sizeof(hdr);
        return sizeof(hdr) + body_len;
    }

    // Runs on the NimBLE host task. Validate, copy, get out — no LVGL, no
    // blocking. on_tick() does the real work.
    void on_radio_rx(int8_t rssi, const uint8_t *data, size_t len)
    {
        if (len < sizeof(bbx_hdr_t)) return;

        bbx_hdr_t hdr;
        memcpy(&hdr, data, sizeof(hdr));
        if (!bbx_magic_ok(&hdr)) return;

        if (hdr.type == BBX_TYPE_BEACON) {
            relay_seen_flag_ = true;
            if (state_ == State::Idle) {
                sample_beacon(rssi, (const char *)(data + sizeof(hdr)),
                              len - sizeof(hdr));
            }
            return;
        }
        if (hdr.type != BBX_TYPE_RESP) return;

        // Broadcast has no destination, so the frame carries ours. Match on
        // that rather than the advertiser address, which is byte-ordered by
        // NimBLE and easy to get backwards.
        if (active_req_id_ == 0 || hdr.req_id != active_req_id_) return;
        if (memcmp(hdr.badge, my_mac_, 6) != 0) return;

        relay_seen_flag_ = true;

        size_t body_len = len - sizeof(hdr);
        if (body_len > sizeof(rx_body_) - 1) body_len = sizeof(rx_body_) - 1;

        if (!rx_mutex_) return;
        if (xSemaphoreTake(rx_mutex_, 0) != pdTRUE) return;   // busy, it repeats
        if (!rx_ready_) {
            memcpy(rx_body_, data + sizeof(hdr), body_len);
            rx_body_[body_len] = '\0';
            rx_status_ = hdr.status;
            rx_flags_  = hdr.flags;
            rx_ready_  = true;
        }
        xSemaphoreGive(rx_mutex_);
    }

    void tick_opening()
    {
        if (relay_seen_flag_ && !relay_seen_) {
            relay_seen_ = true;
            set_relay_seen(true);
        }

        // Border pulse between border and gold, so it reads as "working".
        int phase = ticks_ % PULSE_PERIOD;
        set_border(phase < PULSE_PERIOD / 2 ? COLOR_GOLD : COLOR_BORDER);

        char body[sizeof(rx_body_)];
        uint8_t status = 0, flags = 0;
        if (take_rx_slot(body, sizeof(body), &status, &flags)) {
            finish_open(body, status, flags);
            return;
        }

        if (++ticks_ >= OPEN_TIMEOUT_TICKS) {
            enter_failed(relay_seen_ ? "Relay didn't answer" : "No relay in range",
                         relay_seen_ ? "It may be busy — try again"
                                     : "Move closer to the station");
        }
    }

    void finish_open(const char *body, uint8_t status, uint8_t flags)
    {
        // Tell the relay we got it so it can free its response slot early,
        // then keep advertising the ACK for a beat before powering down.
        send_ack();
        active_req_id_ = 0;
        ticks_ = 0;

        // Don't re-fire on the box the user is still standing in front of.
        armed_ = false;
        cooldown_until_ms_ = now_ms() + kRearmCooldownMs;

        if (status != BBX_ST_OK) {
            // No stale QR: B on this screen must do nothing.
            reveal_item_[0] = '\0';
            reveal_link_[0] = '\0';
            state_ = State::Reveal;
            show_relay_error(status, body);
            return;
        }

        // Response: { new_item, chain_link, all_items }
        // all_items is the authoritative inventory, not a delta. If a reply
        // is lost over the lossy BLE link, the next box resyncs everything —
        // so replace local state with it wholesale rather than merging.
        char item[48] = {};
        char chain_link[192] = {};
        int  item_count = 0;
        cJSON *root = cJSON_Parse(body);
        if (root) {
            copy_json_str(root, "new_item", item, sizeof(item));
            copy_json_str(root, "chain_link", chain_link, sizeof(chain_link));

            cJSON *all = cJSON_GetObjectItemCaseSensitive(root, "all_items");
            if (cJSON_IsArray(all)) {
                const char *items[SOLANA_MAX_ITEMS] = {};
                size_t count = 0;
                const cJSON *entry = nullptr;
                cJSON_ArrayForEach(entry, all) {
                    if (count >= SOLANA_MAX_ITEMS) break;
                    if (cJSON_IsString(entry) && entry->valuestring
                        && *entry->valuestring
                        && strcmp(entry->valuestring, kFinalQuestItem) != 0) {
                        items[count++] = entry->valuestring;
                    }
                }
                solana_wallet_replace_items(items, count);
                item_count = solana_wallet_item_count();
            }
            cJSON_Delete(root);
        }
        if (chain_link[0]) solana_wallet_set_last_link(chain_link);

        state_ = State::Reveal;
        if (strcmp(item, kFinalQuestItem) == 0) {
            // Final quest: no pal to reveal. Show the saved QR so the
            // attendee can finish on their phone — prefer this reply's
            // link, fall back to the wallet's saved one.
            if (!chain_link[0]) {
                snprintf(chain_link, sizeof(chain_link), "%s",
                         solana_wallet_last_link());
            }
            hal_lights_flash_success();
            snprintf(reveal_item_, sizeof(reveal_item_), "%s", kFinalQuestItem);
            snprintf(reveal_link_, sizeof(reveal_link_), "%s", chain_link);
            snprintf(reveal_box_, sizeof(reveal_box_), "%s", box_id_);
            reveal_count_ = item_count;
            if (!OpenedBoxes::replayable(box_id_) &&
                opened_boxes_.record(box_id_)) save_opened_boxes();
            save_last_reveal();
            render_reveal();
            if (reveal_link_[0]) show_qr();
            return;
        }
        if (item[0] == '\0') {
            reveal_item_[0] = '\0';
            reveal_link_[0] = '\0';
            set_border(COLOR_MAGENTA);
            set_status("Empty box?", COLOR_MAGENTA);
            set_detail((flags & BBX_FLAG_TRUNC) ? "Reply was too long to fit"
                                                : "The server sent no item");
            set_hint("A: try another box");
            return;
        }

        hal_lights_flash_success();
        snprintf(reveal_item_, sizeof(reveal_item_), "%s", item);
        snprintf(reveal_link_, sizeof(reveal_link_), "%s", chain_link);
        snprintf(reveal_box_, sizeof(reveal_box_), "%s", box_id_);
        reveal_count_ = item_count;
        if (!OpenedBoxes::replayable(box_id_) &&
            opened_boxes_.record(box_id_)) save_opened_boxes();
        save_last_reveal();
        render_reveal();
        // The link QR is the phone flow — put it up straight away when the
        // server sent one. B flips back to the item view.
        if (reveal_link_[0]) show_qr();
    }

    void load_opened_boxes()
    {
        hal_config_get_string(kSlug, "opened", opened_joined_,
                              sizeof(opened_joined_), "");
        opened_boxes_.parse(opened_joined_);
    }

    void save_opened_boxes()
    {
        opened_boxes_.serialize(opened_joined_, sizeof(opened_joined_));
        hal_config_set_string(kSlug, "opened", opened_joined_);
    }

    // Last reveal survives app exits in hal_config, so reopening the app
    // lands back on the QR. Only link-carrying reveals persist — an
    // item-only tap must not clobber a usable QR, and a run with no link
    // at all boots into the hunt.
    void save_last_reveal()
    {
        if (!reveal_link_[0]) return;
        hal_config_set_string(kSlug, "last_item", reveal_item_);
        hal_config_set_string(kSlug, "last_link", reveal_link_);
        hal_config_set_string(kSlug, "last_box", reveal_box_);
        hal_config_set_int(kSlug, "last_count", reveal_count_);
    }

    // Returns true when a saved QR took over the screen. Count refreshes
    // from the wallet — later taps may have grown it since the save.
    bool restore_last_reveal()
    {
        char link[sizeof(reveal_link_)] = {};
        if (!hal_config_get_string(kSlug, "last_link", link, sizeof(link), "") ||
            link[0] == '\0') {
            return false;
        }
        hal_config_get_string(kSlug, "last_item", reveal_item_,
                              sizeof(reveal_item_), "");
        hal_config_get_string(kSlug, "last_box", reveal_box_,
                              sizeof(reveal_box_), "");
        snprintf(reveal_link_, sizeof(reveal_link_), "%s", link);
        reveal_count_ = solana_wallet_item_count();
        state_ = State::Reveal;
        render_reveal();
        show_qr();
        return true;
    }

    // Main reveal: what you won, how many you've collected, where. When the
    // server sent a link, finish_open() puts the QR up first so the tap
    // already hands the attendee their phone flow; B flips back here.
    void render_reveal()
    {
        if (!panel_ && root_) {
            lv_obj_clean(root_);
            build_chrome();
        }
        showing_qr_ = false;

        set_border(COLOR_GOLD);
        if (status_) {
            if (strcmp(reveal_item_, kFinalQuestItem) == 0) {
                lv_label_set_text(status_, kFinalQuestMsg);
                lv_obj_set_style_text_color(status_, lv_color_hex(COLOR_GOLD), 0);
                lv_obj_set_style_text_font(status_, &lv_font_montserrat_16, 0);
            } else {
                lv_label_set_text(status_, pal_name(reveal_item_));
                lv_obj_set_style_text_color(status_, lv_color_hex(COLOR_GOLD), 0);
                lv_obj_set_style_text_font(status_, &lv_font_montserrat_24, 0);
            }
        }
        char sub[80];
        if (reveal_count_ > 0) snprintf(sub, sizeof(sub), "%d collected - %s",
                                        reveal_count_, reveal_box_);
        else                   snprintf(sub, sizeof(sub), "%s", reveal_box_);
        set_detail(sub);
        set_hint(reveal_link_[0] ? "B: details   A: continue"
                                 : "A: continue");
    }

    // Full-screen QR to the server's link (quest console or chain explorer).
    // B dismisses back to the reveal; A scans again from here too.
    void show_qr()
    {
        if (!root_ || !reveal_link_[0]) return;
        lv_obj_clean(root_);
        title_ = panel_ = status_ = detail_ = hint_ = relay_dot_ = nullptr;
        showing_qr_ = true;

        const bool final_quest = strcmp(reveal_item_, kFinalQuestItem) == 0;

        lv_obj_t *title = lv_label_create(root_);
        lv_label_set_text(title, final_quest ? kFinalQuestMsg
                                             : pal_name(reveal_item_));
        lv_obj_set_style_text_color(title, lv_color_hex(COLOR_GOLD), 0);
        if (final_quest) {
            // Two-line wrapped message, so the QR shifts down to clear it.
            lv_obj_set_width(title, 292);
            lv_label_set_long_mode(title, LV_LABEL_LONG_WRAP);
            lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);
        } else {
            lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
        }
        lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 5);

        lv_obj_t *qr = lv_qrcode_create(root_);
        lv_qrcode_set_size(qr, 144);
        lv_qrcode_set_dark_color(qr, lv_color_hex(0x000000));
        lv_qrcode_set_light_color(qr, lv_color_hex(0xffffff));
        lv_qrcode_set_data(qr, reveal_link_);
        lv_obj_align(qr, LV_ALIGN_TOP_MID, 0, final_quest ? 52 : 34);

        lv_obj_t *cap = lv_label_create(root_);
        lv_label_set_text(cap, "Scan to continue on your phone");
        lv_obj_set_style_text_font(cap, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(cap, lv_color_hex(COLOR_TEXT), 0);
        lv_obj_align(cap, LV_ALIGN_TOP_MID, 0, final_quest ? 200 : 186);

        lv_obj_t *again = lv_label_create(root_);
        lv_label_set_text(again, "B: back   A: continue");
        lv_obj_set_style_text_font(again, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(again, lv_color_hex(COLOR_DIM), 0);
        lv_obj_align(again, LV_ALIGN_BOTTOM_MID, 0, -4);
    }

    void show_relay_error(uint8_t status, const char *body)
    {
        const char *what;
        switch (status) {
            case BBX_ST_NO_WIFI:   what = "Relay is offline";   break;
            case BBX_ST_TRANSPORT: what = "Relay couldn't connect"; break;
            case BBX_ST_HTTP:      what = "Server refused it";   break;
            case BBX_ST_BUSY:      what = "Relay is busy";       break;
            default:               what = "Relay error";         break;
        }
        set_border(COLOR_MAGENTA);
        set_status(what, COLOR_MAGENTA);
        set_detail(body && body[0] ? body : "");
        set_hint("A: retry   B: pals   Start: help");
    }

    void send_ack()
    {
        bbx_hdr_t hdr = {};
        bbx_set_magic(&hdr);
        hdr.type  = BBX_TYPE_ACK;
        hdr.seq   = 0;
        hdr.total = 1;
        // req_id is deliberately kept so the relay can match the right slot.
        hdr.req_id = active_req_id_;
        memcpy(hdr.badge, my_mac_, 6);
        hal_radio_send(&hdr, sizeof(hdr));
        ack_ticks_ = ACK_TICKS;
    }

    void tick_reveal()
    {
        // Deliberately does NOT call hal_radio_disable(). That blocks up to 2 s
        // waiting for the NimBLE host task to unwind, and doing it from here
        // stalls the LVGL timer callback that owns the lock — the suspected
        // cause of the reveal-time crash. It would also be outright wrong,
        // since idle needs the radio up to hear beacons.
        //
        // The radio still gets torn down: the registry force-disables it on
        // every app switch. Cost is that it stays powered while the reveal
        // sits open.
        if (ack_ticks_ > 0) ack_ticks_--;
    }

    void enter_failed(const char *msg, const char *detail)
    {
        // Same reasoning as tick_reveal() — this runs on tick paths too, so no
        // blocking teardown here.
        clear_rx_slot();
        active_req_id_ = 0;
        ack_ticks_ = 0;

        set_border(COLOR_MAGENTA);
        set_status(msg, COLOR_MAGENTA);
        set_detail(detail);
        set_hint("A: retry   B: pals   Start: help");
        state_ = State::Failed;
    }

    // ---- RX slot -------------------------------------------------------

    void clear_rx_slot()
    {
        relay_seen_flag_ = false;
        if (!rx_mutex_) return;
        if (xSemaphoreTake(rx_mutex_, portMAX_DELAY) == pdTRUE) {
            rx_ready_ = false;
            xSemaphoreGive(rx_mutex_);
        }
    }

    bool take_rx_slot(char *out, size_t out_sz, uint8_t *status, uint8_t *flags)
    {
        bool got = false;
        if (rx_mutex_ && xSemaphoreTake(rx_mutex_, 0) == pdTRUE) {
            if (rx_ready_) {
                snprintf(out, out_sz, "%s", rx_body_);
                *status = rx_status_;
                *flags  = rx_flags_;
                rx_ready_ = false;
                got = true;
            }
            xSemaphoreGive(rx_mutex_);
        }
        return got;
    }

    lv_obj_t *root_      = nullptr;
    lv_obj_t *title_     = nullptr;
    lv_obj_t *panel_     = nullptr;
    lv_obj_t *status_    = nullptr;
    lv_obj_t *detail_    = nullptr;
    lv_obj_t *hint_      = nullptr;
    lv_obj_t *relay_dot_ = nullptr;

    // Read from the host task in on_radio_rx() to gate beacon sampling.
    volatile State state_ = State::Idle;
    int      ticks_     = 0;
    int      ack_ticks_ = 0;
    bool     relay_seen_ = false;
    bool     showing_qr_ = false;
    // Read by the NimBLE host task in on_radio_rx(); written here only while
    // no callback is registered or when retiring a request.
    volatile uint16_t active_req_id_ = 0;
    uint8_t  my_mac_[6]      = {};
    char     box_id_[kBoxIdCap] = {};

    // Last reveal, kept so B can flip between the item view and the link QR.
    char     reveal_item_[48]  = {};
    char     reveal_link_[192] = {};
    char     reveal_box_[kBoxIdCap] = {};
    int      reveal_count_ = 0;

    // Boxes this badge has opened: once ever, never again — except the
    // replayable final booth, which never enters the set. Loaded from
    // hal_config on entry (so it survives reboots) and saved on every open.
    OpenedBoxes opened_boxes_;
    char opened_joined_[OpenedBoxes::kJoinCap] = {};

    bool     armed_             = true;
    uint32_t cooldown_until_ms_ = 0;
    // Last string pushed to the detail label, so the 50 Hz tick only touches
    // LVGL when the readout actually changed.
    char     detail_shown_[80]  = {};

    // Written from the NimBLE host task, read from on_tick().
    volatile bool     relay_seen_flag_ = false;
    SemaphoreHandle_t rx_mutex_        = nullptr;
    bool              rx_ready_        = false;
    uint8_t           rx_status_       = 0;
    uint8_t           rx_flags_        = 0;
    char              rx_body_[BLINDBOX_WEB_MAX_RESPONSE + 1] = {};
    // Guarded by rx_mutex_ alongside the RX slot.
    BeaconSample      cand_[kMaxCandidates] = {};
};

}  // namespace

static BlindBoxApp s_instance;
App *app_blindbox_instance() { return &s_instance; }

// The USB Web Serial trigger was removed when the app went proximity-only.
// Kept as a stub because hal_console's bbx_web command still links against
// it; it now always rejects so the console reports BBX_WEB_REJECTED.
bool blindbox_web_serial_submit(uint16_t /*request_id*/, const char * /*json*/,
                                size_t /*len*/)
{
    return false;
}
