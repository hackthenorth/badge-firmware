#include "app.h"
#include "app_registry.h"
#include "bump_detector.h"
#include "bump_proto.h"
#include "button_bump.h"
#include "contacts_store.h"
#include "hal_accel.h"
#include "hal_identity.h"
#include "hal_lights.h"
#include "hal_radio.h"
#include "launcher_slugs.h"
#include "ripple_state.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_random.h"
#include "esp_timer.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <string>
#include <vector>

// Connect: bump-to-exchange and the contacts book in one app.
//
// Opens directly into the mutual-bump exchange. B/Start toggles between
// bumping and saved contacts. The exchange privacy design is unchanged:
// advertising stays paused until a detected bump or A press starts a short transmit
// burst. Receiving is window-gated too: the radio is fully off at idle,
// physical bumps get one full second, and A taps/holds get 1-2 seconds plus
// a bounded acknowledgement tail. Contacts mode
// is the saved-contacts list and detail views. There are deliberately no
// tuning settings: combined-axis impact threshold and RSSI floor are fixed.

// Reuses the badge-bump launcher icon until dedicated Connect artwork lands
// (assets/icons/connect.png + regen-assets.py, which generates icon_connect).
extern "C" const lv_image_dsc_t icon_badgebump;

static constexpr uint32_t COLOR_BG       = BadgeTheme::BACKGROUND;
static constexpr uint32_t COLOR_PANEL    = BadgeTheme::PANEL;
static constexpr uint32_t COLOR_FOCUS    = BadgeTheme::SURFACE;
static constexpr uint32_t COLOR_BORDER   = BadgeTheme::BORDER;
static constexpr uint32_t COLOR_ACCENT   = BadgeTheme::ACCENT_DETAIL;
static constexpr uint32_t COLOR_TEXT     = BadgeTheme::TEXT;
static constexpr uint32_t COLOR_TEXT_DIM = BadgeTheme::TEXT_MUTED;

// Require a deliberate physical impact and a strong nearby signal.
static constexpr int kBumpThresholdMg = 400;
// Heuristic proximity filter for the 10 cm guidance, not a distance measurement.
static constexpr int kBumpRssiMin     = -64;

static const char *CONTACTS_DIR = "/littlefs/config/contacts";

struct Contact {
    std::string file;        // "<badge_id>.cfg"
    std::string badge_id;
    std::string name;
    std::string email;
    std::string phone;
    std::string linkedin;
    std::string discord;
    std::string instagram;
    std::string x;
};

enum class ContactLinkKind : uint8_t {
    Email,
    Phone,
    LinkedIn,
    Discord,
    Instagram,
    X,
};

struct ContactLink {
    ContactLinkKind kind;
    const char *label;
    const std::string *value;
};

static constexpr int kMaxContactLinks = 6;

static const char *without_at(const char *value)
{
    return value && value[0] == '@' ? value + 1 : value;
}

static bool is_web_url(const char *value)
{
    return value && (strncmp(value, "https://", 8) == 0
        || strncmp(value, "http://", 7) == 0);
}

static bool is_numeric(const char *value)
{
    if (!value || !*value) return false;
    for (const char *p = value; *p; ++p) {
        if (!std::isdigit(static_cast<unsigned char>(*p))) return false;
    }
    return true;
}

static std::string trim_eol(const char *s)
{
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r')) n--;
    return std::string(s, n);
}

static bool parse_contact_file(const std::string &full_path, Contact &out)
{
    FILE *f = fopen(full_path.c_str(), "r");
    if (!f) return false;
    char line[320];
    while (fgets(line, sizeof(line), f)) {
        const char *eq = strchr(line, '=');
        if (!eq) continue;
        std::string key(line, eq - line);
        std::string val = trim_eol(eq + 1);
        if      (key == "badge_id")      out.badge_id = val;
        else if (key == "name")          out.name = val;
        else if (key == "email")         out.email = val;
        else if (key == "phone")         out.phone = val;
        else if (key == "linkedin")      out.linkedin = val;
        else if (key == "discord")       out.discord = val;
        else if (key == "instagram")     out.instagram = val;
        else if (key == "x")             out.x = val;
    }
    fclose(f);
    return true;
}

class ConnectApp : public App {
public:
    const char *name() const override { return "Connect"; }
    const char *icon() const override { return "CON"; }
    const char *slug() const override { return "connect"; }
    AppAccess access() const override { return AppAccess::Default; }
    const lv_image_dsc_t *icon_image() const override { return &icon_badgebump; }
    bool wants_wake_lock() const override { return true; }

    void on_enter(lv_obj_t *root) override
    {
        root_ = root;
        lv_obj_set_style_bg_color(root_, lv_color_hex(COLOR_BG), 0);
        lv_obj_set_style_bg_opa(root_, LV_OPA_COVER, 0);
        if (!rx_mutex_) rx_mutex_ = xSemaphoreCreateMutex();
        enter_bump();
    }

    void on_exit() override
    {
        teardown_current();
        root_ = nullptr;
    }

    void on_button(ButtonEvent ev) override
    {
        if (mode_ == Mode::Bump && ev.button == Button::A
            && ev.kind == ButtonEventKind::Released) {
            button_attempt_.release(esp_timer_get_time());
            return;
        }
        if (ev.kind != ButtonEventKind::Pressed) return;
        if (mode_ == Mode::Contacts) {
            if (ev.button == Button::Start) enter_bump();
            else                            handle_contacts_button(ev);
        } else if (ev.button == Button::B || ev.button == Button::Start) {
            enter_contacts();
        } else if (ev.button == Button::A) {
            start_button_bump();
        }
    }

    void on_tick() override
    {
        if (mode_ == Mode::Bump) bump_tick();
    }

private:
    enum class Mode { Bump, Contacts };

    // ---- mode switching -------------------------------------------------

    void teardown_current()
    {
        if (mode_ == Mode::Bump) bump_radio_teardown();
        if (root_) lv_obj_clean(root_);
        null_all();
        contacts_.clear();
    }

    void null_all()
    {
        view_hint_ = nullptr;
        bump_status_ = bump_last_synced_ = bump_contact_count_ = nullptr;
        for (auto &r : bump_rings_) r = nullptr;
        contacts_title_ = contacts_list_panel_ = nullptr;
        for (auto &row : contacts_rows_) row = nullptr;
        detail_panel_ = nullptr;
        detail_name_ = nullptr;
        for (auto &row : detail_link_rows_) row = nullptr;
    }

    void enter_bump()
    {
        teardown_current();
        mode_ = Mode::Bump;
        pending_ack_[0] = '\0';
        bump_radio_setup();
        load_owner_card();
        contact_count_ = contacts_store_count();
        build_bump_ui(root_);
        if (!radio_ok_ && bump_status_) {
            lv_label_set_text(bump_status_, "NO RADIO");
            lv_obj_set_style_text_color(bump_status_,
                                        lv_color_hex(BadgeTheme::ACCENT_DETAIL), 0);
        }
        bump_detector_.reset();
        result_until_ = 0;
        result_failed_ = false;
        last_sync_was_duplicate_ = false;
        bump_started_at_ = bump_until_ = 0;
        tx_dirty_ = false;
        tx_retry_at_ = 0;
        ring_phase_ = 0;
        ring_age_[0] = 0;
        ring_age_[1] = kRingLifeTicks / 2;
        for (auto &p : recent_peers_) p.in_use = false;
    }

    void enter_contacts()
    {
        teardown_current();
        mode_ = Mode::Contacts;
        viewing_detail_ = false;
        viewing_qr_ = false;
        sel_ = 0;
        scroll_ = 0;
        detail_link_sel_ = 0;
        build_contacts_list_ui();
        load_contacts();
        render_contacts_list();
    }

    void set_view_hint(const char *text)
    {
        if (!view_hint_) {
            view_hint_ = lv_label_create(root_);
            lv_obj_set_width(view_hint_, 308);
            lv_obj_set_style_text_font(view_hint_, &lv_font_montserrat_12, 0);
            lv_obj_set_style_text_color(view_hint_, lv_color_hex(COLOR_TEXT_DIM), 0);
            lv_obj_set_style_text_align(view_hint_, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_align(view_hint_, LV_ALIGN_BOTTOM_MID, 0, -7);
        }
        lv_label_set_text(view_hint_, text);
    }

    void handle_contacts_button(ButtonEvent ev)
    {
        if (viewing_qr_)          handle_qr_button(ev);
        else if (viewing_detail_) handle_detail_button(ev);
        else                      handle_contacts_list_button(ev);
    }

    // ---- bump view ------------------------------------------------------
    // The controller repeats the current payload every 30 ms, without HCI
    // restarts between packets. Keep scanning and TX alive even after ACK.
    static constexpr int64_t kSyncWindowUs = 1000000;
    static constexpr int64_t kTxRetryUs = 100000;
    static constexpr int64_t kResultUs = 3000000;
    static constexpr int64_t kFailureUs = 2000000;
    // Result LEDs run at half logical brightness (>> 1). Gamma in the lights
    // HAL squares the value, so this quarters physical LED current while
    // staying clearly visible. All six LEDs still light.
    static constexpr int kResultDimShift = 1;
    static constexpr uint32_t kYellow = 0xffcc00;
    static constexpr uint32_t kFailureRed = 0xff5364;
    static constexpr uint32_t kBurstAdvIntervalMs = 30;
    static constexpr uint32_t kScanIntervalMs     = 100;
    static constexpr uint32_t kScanWindowMs       = 100;  // 100% duty for the short window
    static constexpr size_t kRxQueueMax = 8;

    // Idle pulse rings: emitted every kRingEmitTicks, each living
    // kRingLifeTicks while expanding and fading. Two rings overlap so the
    // pulse never gaps. Tick-driven; no lv_anim use in this tree.
    static constexpr int kRingEmitTicks = 23;
    static constexpr int kRingLifeTicks = 46;
    static constexpr int kRingMinDia    = 52;
    static constexpr int kRingMaxDia    = 200;

    struct PeerSeen {
        bool in_use = false;
        bool saved_this_bump = false;
        uint8_t mac[6] = {};
        BumpCard card = {};
    };

    struct Incoming {
        uint8_t mac[6];
        int8_t  rssi;
        uint16_t len;
        int64_t received_at;
        uint8_t data[BUMP_MAX_FRAME];
    };

    void bump_radio_setup()
    {
        // The return used to be ignored, so a dead radio presented a working
        // WAITING screen and bumps silently never left the badge. Surface it.
        // Warm the stack now (the first enable pays init_once) then shut RF
        // straight back off: idle never listens, so holding the receiver on
        // would burn power for nothing. Each bump re-enables for one sync
        // window via bump_radio_listen_begin().
        radio_ok_ = (hal_radio_enable() == ESP_OK);
        hal_radio_disable();
        radio_on_ = false;

        // Set while down, so remembered for the next enable. The receiver is
        // off at idle, making 100% duty inside this one-second proximity gate
        // a small power cost for a much more reliable bump. The registry
        // resets these timings on every app switch.
        hal_radio_set_adv_interval_ms(kBurstAdvIntervalMs, kBurstAdvIntervalMs);
        hal_radio_set_scan_duty_ms(kScanIntervalMs, kScanWindowMs);
    }

    void bump_radio_listen_begin()
    {
        if (radio_on_) return;
        radio_ok_ = (hal_radio_enable() == ESP_OK);
        if (!radio_ok_) return;
        // Live retune in case anything reset the timings while we were off.
        hal_radio_set_adv_interval_ms(kBurstAdvIntervalMs, kBurstAdvIntervalMs);
        hal_radio_set_scan_duty_ms(kScanIntervalMs, kScanWindowMs);
        radio_on_ = true;
        register_bump_rx();
    }

    void bump_radio_listen_end()
    {
        if (!radio_on_) return;
        radio_on_ = false;
        hal_radio_set_recv_cb(RadioRecvCb{});
        hal_radio_adv_pause();          // never leave PII on the air
        hal_radio_disable();            // stops scanning and resets timings
        if (rx_mutex_ && xSemaphoreTake(rx_mutex_, portMAX_DELAY) == pdTRUE) {
            rx_head_ = rx_count_ = 0;
            xSemaphoreGive(rx_mutex_);
        }
    }

    void register_bump_rx()
    {
        hal_radio_set_recv_cb([this](const uint8_t mac[6], int8_t rssi,
                                     const uint8_t *data, size_t len) {
            // Runs on the NimBLE host task: copy and get out. Decoding and
            // anything touching LVGL happens in bump_tick().
            if (len < BUMP_HDR_SIZE || len > BUMP_MAX_FRAME) return;
            if (memcmp(data, BUMP_MAGIC, 4) != 0
                && memcmp(data, ButtonBump::MAGIC, 4) != 0) return;

            if (rx_mutex_ && xSemaphoreTake(rx_mutex_, 0) == pdTRUE) {
                if (rx_count_ < kRxQueueMax) {
                    Incoming &in = rx_queue_[(rx_head_ + rx_count_) % kRxQueueMax];
                    memcpy(in.mac, mac, 6);
                    in.rssi = rssi;
                    in.len = len;
                    in.received_at = esp_timer_get_time();
                    memcpy(in.data, data, len);
                    ++rx_count_;
                }
                xSemaphoreGive(rx_mutex_);
            }
        });
    }

    void bump_radio_teardown()
    {
        button_attempt_ = {};
        button_saved_ = false;
        bump_started_at_ = bump_until_ = 0;
        bump_radio_listen_end();
        hal_lights_clear();
        hal_lights_show();
    }

    void build_bump_ui(lv_obj_t *root)
    {
        for (int i = 0; i < 2; i++) {
            lv_obj_t *ring = lv_obj_create(root);
            lv_obj_remove_style_all(ring);
            lv_obj_set_style_bg_opa(ring, LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_width(ring, 3, 0);
            lv_obj_set_style_border_color(ring, lv_color_hex(COLOR_ACCENT), 0);
            lv_obj_set_style_border_opa(ring, LV_OPA_TRANSP, 0);
            lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
            lv_obj_clear_flag(ring, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_add_flag(ring, LV_OBJ_FLAG_HIDDEN);
            bump_rings_[i] = ring;
        }

        lv_obj_t *title = lv_label_create(root);
        lv_label_set_text(title, "CONNECT BADGES");
        lv_obj_set_style_text_color(title, lv_color_hex(COLOR_TEXT), 0);
        lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
        lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 6);

        bump_status_ = lv_label_create(root);
        lv_obj_set_width(bump_status_, 310);
        lv_obj_set_style_text_align(bump_status_, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_font(bump_status_, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(bump_status_, lv_color_hex(COLOR_TEXT_DIM), 0);
        lv_label_set_text(bump_status_, owner_ready_ ? "Bump badges or\nPress A" : "NO ID");
        lv_obj_set_height(bump_status_, 52);
        lv_obj_align(bump_status_, LV_ALIGN_TOP_MID, 0, 50);

        bump_last_synced_ = lv_label_create(root);
        lv_obj_set_style_text_font(bump_last_synced_, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(bump_last_synced_, lv_color_hex(COLOR_ACCENT), 0);
        lv_label_set_text(bump_last_synced_, owner_ready_
            ? "Hold badges within 10 cm"
                          : "Not registered - see badge desk");
        lv_obj_set_size(bump_last_synced_, 296, 38);
        lv_obj_set_style_text_align(bump_last_synced_, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_long_mode(bump_last_synced_, LV_LABEL_LONG_DOT);
        lv_obj_align(bump_last_synced_, LV_ALIGN_TOP_MID, 0, 105);

        bump_contact_count_ = lv_label_create(root);
        lv_obj_set_style_text_font(bump_contact_count_, &lv_font_montserrat_18, 0);
        lv_obj_set_style_text_color(bump_contact_count_, lv_color_hex(COLOR_TEXT), 0);
        lv_obj_align(bump_contact_count_, LV_ALIGN_TOP_MID, 0, 154);
        refresh_contact_count();
        set_view_hint("Hold A for up to 2 seconds\nB/Start  Contacts     Home  Apps");
    }

    // Owner card comes from the provisioned identity. There are deliberately
    // no fallback values: an unprovisioned badge must broadcast NOTHING
    // rather than a placeholder or a developer's real contact details.
    void load_owner_card()
    {
        memset(&owner_, 0, sizeof(owner_));
        owner_ready_ = false;

        if (!hal_identity_is_provisioned()) return;
        const BadgeIdentity *id = hal_identity_get();

        snprintf(owner_.badge_id,      sizeof(owner_.badge_id),      "%s", id->badge_id);
        snprintf(owner_.display_name,  sizeof(owner_.display_name),  "%s", id->display_name);
        snprintf(owner_.net_email,     sizeof(owner_.net_email),     "%s", id->net_email);
        snprintf(owner_.net_phone,     sizeof(owner_.net_phone),     "%s", id->net_phone);
        snprintf(owner_.net_linkedin,  sizeof(owner_.net_linkedin),  "%s", id->net_linkedin);
        snprintf(owner_.net_discord,   sizeof(owner_.net_discord),   "%s", id->net_discord);
        snprintf(owner_.net_instagram, sizeof(owner_.net_instagram), "%s", id->net_instagram);
        snprintf(owner_.net_x,         sizeof(owner_.net_x),         "%s", id->net_x);
        owner_.role = id->role;
        const RippleState ripple = ripple_state_load();
        owner_.ripple_active = ripple.active;
        owner_.ripple_depth = ripple.depth;
        owner_ready_ = owner_.badge_id[0] != '\0';
    }

    void bump_tick()
    {
        const int64_t now = esp_timer_get_time();
        if (result_until_ && now >= result_until_ && !radio_on_) {
            result_until_ = 0;
            hal_lights_clear();
            hal_lights_show();
        }
        if (button_attempt_.started()) {
            if (!button_attempt_.active(now)) {
                stop_transmit();
                bump_radio_listen_end();
                button_attempt_ = {};
                bump_detector_.reset();
                if (!button_saved_) show_result(true);
            } else {
                drain_rx();
                if (!button_attempt_.advertise(now))
                    hal_radio_adv_pause();
                else pump_transmit();
            }
            update_bump_status();
            update_rings();
            return; // Button movement must not start a physical bump too.
        }

        const bool local_bump = detect_bump();
        if (local_bump && !bump_until_ && owner_ready_ && radio_ok_) {
            bump_radio_listen_begin();
            if (radio_on_) {
                clear_result();
                for (auto &peer : recent_peers_) peer = {};
                pending_ack_[0] = '\0';
                bump_saved_ = false;
                bump_started_at_ = esp_timer_get_time();
                bump_until_ = bump_started_at_ + kSyncWindowUs;
                tx_dirty_ = true;
                tx_retry_at_ = 0;
                pump_transmit();
                emit_ring();
            }
        }

        if (bump_until_) {
            // Drain packets by their receive timestamps before closing the
            // window, including an on-time packet queued just before expiry.
            drain_rx();
            if (esp_timer_get_time() >= bump_until_) {
                stop_transmit();
                bump_radio_listen_end();
                bump_started_at_ = bump_until_ = 0;
                if (!bump_saved_) show_result(true);
            } else {
                pump_transmit();
            }
        }
        update_bump_status();
        update_rings();
    }

    bool detect_bump()
    {
        AccelSample s;
        if (hal_accel_read(&s) != ESP_OK) return false;
        return bump_detector_.update(s, (float)kBumpThresholdMg);
    }

    void pump_transmit()
    {
        const int64_t now = esp_timer_get_time();
        if (!owner_ready_ || !radio_on_ || now < tx_retry_at_) return;
        const bool button = button_attempt_.started();
        if (button ? !button_attempt_.advertise(now) : !bump_until_ || now >= bump_until_) return;
        tx_retry_at_ = now + kTxRetryUs;
        if (tx_dirty_) {
            uint8_t frame[BUMP_MAX_FRAME];
            size_t size;
            if (button) {
                ButtonBump::Frame message;
                message.nonce = button_attempt_.nonce();
                message.ack_nonce = button_attempt_.peer_nonce();
                message.card = owner_;
                size = ButtonBump::encode(frame, sizeof(frame), message);
            } else {
                size = bump_encode(frame, sizeof(frame), owner_, pending_ack_);
            }
            if (!size || hal_radio_send(frame, size) != ESP_OK) return;
            tx_dirty_ = false;
        }
        // A failed send/resume is retried without extending the deadline.
        // Once running, the controller continuously repeats the same frame.
        hal_radio_adv_resume();
    }

    void stop_transmit()
    {
        tx_dirty_ = false;
        hal_radio_adv_pause();
    }

    void start_button_bump()
    {
        const int64_t now = esp_timer_get_time();
        if (!owner_ready_ || !radio_ok_ || button_attempt_.active(now)) return;
        bump_radio_listen_begin();
        if (!radio_on_) return;
        stop_transmit();
        bump_started_at_ = bump_until_ = 0;
        pending_ack_[0] = '\0';
        for (auto &peer : recent_peers_) peer.in_use = false;
        const uint32_t nonce = esp_random();
        button_attempt_.begin_held(esp_timer_get_time(), nonce ? nonce : 1);
        button_saved_ = false;
        clear_result();
        if (bump_status_) {
            lv_label_set_text(bump_status_, "PAIRING...");
            lv_obj_set_style_text_color(bump_status_,
                                        lv_color_hex(BadgeTheme::ACCENT_DETAIL), 0);
        }
        if (bump_last_synced_)
            lv_label_set_text(bump_last_synced_, "Both press or hold A\nStay within 10 cm");
        emit_ring();
        tx_dirty_ = true;
        tx_retry_at_ = 0;
        pump_transmit();
    }

    void drain_rx()
    {
        // Bounded, allocation-free queue: process outside the radio callback
        // lock, and never let a busy channel starve the UI or deadline check.
        for (size_t n = 0; n < kRxQueueMax; ++n) {
            Incoming in;
            bool received = false;
            if (rx_mutex_ && xSemaphoreTake(rx_mutex_, 0) == pdTRUE) {
                if (rx_count_) {
                    in = rx_queue_[rx_head_];
                    rx_head_ = (rx_head_ + 1) % kRxQueueMax;
                    --rx_count_;
                    received = true;
                }
                xSemaphoreGive(rx_mutex_);
            }
            if (!received) break;
            // Proximity gate. A bump is contact range, so a weak signal is a
            // different conversation across the room, not our partner.
            // 127 is the "reading unavailable" sentinel, not a strong signal.
            if (in.rssi == 127) continue;
            if (in.rssi < kBumpRssiMin) continue;

            if (memcmp(in.data, ButtonBump::MAGIC, 4) == 0) {
                ButtonBump::Frame frame;
                if (!owner_ready_ || !button_attempt_.active(esp_timer_get_time())
                    || !ButtonBump::decode(in.data, in.len, frame)
                    || strcmp(frame.card.badge_id, owner_.badge_id) == 0) continue;
                const uint32_t previous_peer = button_attempt_.peer_nonce();
                if (!button_attempt_.receive(in.received_at, in.mac, frame)) continue;
                bool newly_saved = false;
                if (button_attempt_.confirmed() && !button_saved_) {
                    button_saved_ = save_contact(frame.card);
                    newly_saved = button_saved_;
                }
                if (previous_peer != button_attempt_.peer_nonce() || newly_saved) {
                    tx_dirty_ = true;
                    tx_retry_at_ = 0;
                    pump_transmit();
                }
                continue;
            }
            if (button_attempt_.started()) continue;
            if (!bump_until_ || in.received_at < bump_started_at_
                || in.received_at > bump_until_) continue;

            BumpCard card;
            if (!bump_decode(in.data, in.len, &card, nullptr, 0)) continue;
            if (owner_ready_ && strcmp(card.badge_id, owner_.badge_id) == 0) continue;  // our own echo

            PeerSeen *slot = get_or_alloc_peer(in.mac);
            if (!slot) continue;
            slot->card = card;
            // Send ACKs for compatibility, but never stop our burst early.
            if (strncmp(pending_ack_, card.badge_id, sizeof(pending_ack_)) != 0) {
                snprintf(pending_ack_, sizeof(pending_ack_), "%s", card.badge_id);
                tx_dirty_ = true;
                tx_retry_at_ = 0;
            }

            save_peer_contact(*slot);
        }
    }

    PeerSeen *get_or_alloc_peer(const uint8_t mac[6])
    {
        PeerSeen *empty = nullptr;
        for (auto &p : recent_peers_) {
            if (p.in_use && memcmp(p.mac, mac, 6) == 0) return &p;
            if (!p.in_use && !empty) empty = &p;
        }
        PeerSeen *slot = empty;
        if (!slot) return nullptr;
        slot->in_use = true;
        slot->saved_this_bump = false;
        memcpy(slot->mac, mac, 6);
        return slot;
    }

    void save_peer_contact(PeerSeen &peer)
    {
        if (peer.saved_this_bump) return;
        peer.saved_this_bump = save_contact(peer.card);
        bump_saved_ |= peer.saved_this_bump;
    }

    bool save_contact(const BumpCard &card)
    {
        bool added = false;
        if (!contacts_store_save(card, &added)) return false;
        // Always derive the visible total from storage. Besides keeping this
        // authoritative, it self-corrects a stale in-memory count if a VFS
        // existence check ever misreports a replacement as a new file.
        contact_count_ = contacts_store_count();
        // Ripple never participates in deciding whether a bump succeeds. It
        // only observes an already-successful contact exchange, and a failed
        // state write leaves networking behavior unchanged.
        if (card.ripple_active && ripple_state_adopt(card.ripple_depth)) {
            const RippleState ripple = ripple_state_load();
            owner_.ripple_active = ripple.active;
            owner_.ripple_depth = ripple.depth;
            // Refresh an in-flight frame so the newly adopted state is
            // available immediately, without extending the normal hard cap.
            tx_dirty_ = true;
        }

        const bool conversation_unlocked = launcher_slugs_append("conversation");
        refresh_contact_count();
        last_sync_was_duplicate_ = !added;
        show_result(false);
        if (bump_last_synced_) {
            lv_obj_set_style_text_color(bump_last_synced_,
                lv_color_hex(last_sync_was_duplicate_ ? kYellow : COLOR_ACCENT), 0);
            if (!added) {
                lv_label_set_text_fmt(bump_last_synced_, "Already connected: %s",
                    card.display_name[0] ? card.display_name : "Unnamed badge");
            } else if (conversation_unlocked) {
                lv_label_set_text(bump_last_synced_, "Conversation Starters unlocked!");
            } else {
                lv_label_set_text(bump_last_synced_, card.display_name);
            }
        }
        return true;
    }

    void clear_result()
    {
        result_until_ = 0;
        result_failed_ = false;
        last_sync_was_duplicate_ = false;
        if (bump_last_synced_) {
            lv_label_set_text(bump_last_synced_, "Hold badges within 10 cm");
            lv_obj_set_style_text_color(bump_last_synced_, lv_color_hex(COLOR_ACCENT), 0);
        }
        hal_lights_clear();
        hal_lights_show();
    }

    void show_result(bool failed)
    {
        result_failed_ = failed;
        result_until_ = esp_timer_get_time() + (failed ? kFailureUs : kResultUs);
        const uint32_t colour = failed ? kFailureRed
            : last_sync_was_duplicate_ ? kYellow : 0x36e07a;
        const uint8_t r = ((colour >> 16) & 0xff) >> kResultDimShift;
        const uint8_t g = ((colour >> 8) & 0xff) >> kResultDimShift;
        const uint8_t b = (colour & 0xff) >> kResultDimShift;
        for (size_t i = 0; i < LED_COUNT; ++i)
            hal_lights_set(i, r, g, b);
        hal_lights_show();
    }

    void refresh_contact_count()
    {
        // Seeded from saved contacts on entry and refreshed after a successful
        // save. Repeated advertisements/bumps therefore always show the number
        // of canonical badge-ID files on disk, never an accumulated event count.
        if (bump_contact_count_) {
            lv_label_set_text_fmt(bump_contact_count_, "Contacts: %u",
                                  static_cast<unsigned>(contact_count_));
        }
    }

    void update_bump_status()
    {
        if (!bump_status_) return;
        if (!radio_ok_) {
            lv_label_set_text(bump_status_, "NO RADIO");
            lv_obj_set_style_text_color(bump_status_,
                                        lv_color_hex(BadgeTheme::ACCENT_DETAIL), 0);
            return;
        }
        if (!owner_ready_) {
            lv_label_set_text(bump_status_, "NO ID");
            lv_obj_set_style_text_color(bump_status_, lv_color_hex(0xff6b35), 0);
            return;
        }
        const int64_t now = esp_timer_get_time();
        if (result_until_ && (result_until_ > now || radio_on_)) {
            if (result_failed_) {
                lv_label_set_text(bump_status_, "No badge found,\ntry again");
                lv_obj_set_style_text_color(bump_status_, lv_color_hex(kFailureRed), 0);
                return;
            }
            lv_label_set_text(bump_status_, last_sync_was_duplicate_
                                            ? "Already connected" : "SYNCED");
            lv_obj_set_style_text_color(bump_status_, lv_color_hex(
                last_sync_was_duplicate_ ? kYellow : 0x36e07a), 0);
        } else if (button_attempt_.active(now) && !button_saved_) {
            lv_label_set_text(bump_status_, "PAIRING...");
            lv_obj_set_style_text_color(bump_status_,
                                        lv_color_hex(BadgeTheme::ACCENT_DETAIL), 0);
        } else if (bump_until_ > now) {
            lv_label_set_text(bump_status_, "BUMP!");
            lv_obj_set_style_text_color(bump_status_, lv_color_hex(BadgeTheme::ACCENT_DETAIL), 0);
        } else {
            lv_label_set_text(bump_status_, "Bump badges or\nPress A");
            lv_obj_set_style_text_color(bump_status_, lv_color_hex(COLOR_TEXT_DIM), 0);
        }
    }

    void emit_ring()
    {
        int older = (ring_age_[0] > ring_age_[1]) ? 0 : 1;
        ring_age_[older] = 0;
    }

    void update_rings()
    {
        bool idle = radio_ok_ && owner_ready_
                 && !radio_on_ && result_until_ <= esp_timer_get_time();
        if (!idle) {
            for (auto &r : bump_rings_) {
                if (r) lv_obj_add_flag(r, LV_OBJ_FLAG_HIDDEN);
            }
            return;
        }
        if (++ring_phase_ % kRingEmitTicks == 0) emit_ring();
        for (int i = 0; i < 2; i++) {
            lv_obj_t *r = bump_rings_[i];
            if (!r) continue;
            if (ring_age_[i] >= kRingLifeTicks) {
                lv_obj_add_flag(r, LV_OBJ_FLAG_HIDDEN);
                continue;
            }
            ring_age_[i]++;
            int dia = kRingMinDia + (kRingMaxDia - kRingMinDia)
                                      * ring_age_[i] / kRingLifeTicks;
            int opa = 140 * (kRingLifeTicks - ring_age_[i]) / kRingLifeTicks;
            lv_obj_set_size(r, dia, dia);
            lv_obj_align(r, LV_ALIGN_CENTER, 0, 0);
            lv_obj_set_style_border_opa(r, opa, 0);
            lv_obj_clear_flag(r, LV_OBJ_FLAG_HIDDEN);
        }
    }

    // ---- contacts view --------------------------------------------------
    // The list uses Up/Down, details use Up/Down for links and Left/Right for
    // contacts, and A opens the selected link as a QR. B backs up one view;
    // Start returns directly to bumping from anywhere in Contacts.

    static constexpr int kVisibleRows = 9;
    static constexpr int kRowH        = 18;
    static constexpr int kListTop     = 28;
    static constexpr int kListX       = 6;
    static constexpr int kListW       = 308;

    void handle_contacts_list_button(ButtonEvent ev)
    {
        switch (ev.button) {
            case Button::Up:
                if (sel_ > 0) { sel_--; ensure_visible(); render_contacts_list(); }
                break;
            case Button::Down:
                if (sel_ + 1 < (int)contacts_.size()) { sel_++; ensure_visible(); render_contacts_list(); }
                break;
            case Button::A:
            case Button::Right:
                detail_link_sel_ = 0;
                open_detail();
                break;
            case Button::B:
                enter_bump();
                break;
            default: break;
        }
    }

    void handle_detail_button(ButtonEvent ev)
    {
        switch (ev.button) {
            case Button::Left:
                if (cycle_contact(-1)) render_detail();
                break;
            case Button::Right:
                if (cycle_contact(1)) render_detail();
                break;
            case Button::Up:
                if (select_detail_link(-1)) render_detail();
                break;
            case Button::Down:
                if (select_detail_link(1)) render_detail();
                break;
            case Button::A:
                show_contact_qr();
                break;
            case Button::B:
                close_detail();
                break;
            default: break;
        }
    }

    void handle_qr_button(ButtonEvent ev)
    {
        switch (ev.button) {
            case Button::Left:
                if (cycle_contact(-1)) show_contact_qr();
                break;
            case Button::Right:
                if (cycle_contact(1)) show_contact_qr();
                break;
            case Button::Up:
                if (select_detail_link(-1)) show_contact_qr();
                break;
            case Button::Down:
                if (select_detail_link(1)) show_contact_qr();
                break;
            case Button::B:
                close_contact_qr();
                break;
            default: break;
        }
    }

    static void contact_links(const Contact &c,
                              ContactLink out[kMaxContactLinks])
    {
        out[0] = { ContactLinkKind::Email,     "Email",     &c.email };
        out[1] = { ContactLinkKind::Phone,     "Phone",     &c.phone };
        out[2] = { ContactLinkKind::LinkedIn,  "LinkedIn",  &c.linkedin };
        out[3] = { ContactLinkKind::Discord,   "Discord",   &c.discord };
        out[4] = { ContactLinkKind::Instagram, "Instagram", &c.instagram };
        out[5] = { ContactLinkKind::X,         "X",         &c.x };
    }

    bool selected_contact_link(ContactLink *out) const
    {
        if (!out || sel_ < 0 || sel_ >= static_cast<int>(contacts_.size()))
            return false;
        ContactLink links[kMaxContactLinks];
        contact_links(contacts_[sel_], links);
        *out = links[detail_link_sel_];
        return true;
    }

    bool select_detail_link(int delta)
    {
        if (contacts_.empty()) return false;
        detail_link_sel_ = (detail_link_sel_ + kMaxContactLinks + delta)
                         % kMaxContactLinks;
        return true;
    }

    bool cycle_contact(int delta)
    {
        const int count = static_cast<int>(contacts_.size());
        if (count < 2) return false;
        sel_ = (sel_ + count + delta) % count;
        ensure_visible();
        return true;
    }

    static void contact_qr_payload(const ContactLink &link,
                                   char *out, size_t out_size)
    {
        const char *value = link.value->c_str();
        switch (link.kind) {
            case ContactLinkKind::Email:
                snprintf(out, out_size, "mailto:%s", value);
                break;
            case ContactLinkKind::Phone:
                snprintf(out, out_size, "tel:%s", value);
                break;
            case ContactLinkKind::LinkedIn:
                if (is_web_url(value)) snprintf(out, out_size, "%s", value);
                else snprintf(out, out_size, "https://www.linkedin.com/in/%s", value);
                break;
            case ContactLinkKind::Discord:
                if (is_web_url(value)) snprintf(out, out_size, "%s", value);
                else if (is_numeric(value))
                    snprintf(out, out_size, "https://discord.com/users/%s", value);
                else snprintf(out, out_size, "Discord: %s", without_at(value));
                break;
            case ContactLinkKind::Instagram:
                if (is_web_url(value)) snprintf(out, out_size, "%s", value);
                else snprintf(out, out_size, "https://www.instagram.com/%s", without_at(value));
                break;
            case ContactLinkKind::X:
                if (is_web_url(value)) snprintf(out, out_size, "%s", value);
                else snprintf(out, out_size, "https://x.com/%s", without_at(value));
                break;
        }
    }

    void build_contacts_list_ui()
    {
        contacts_title_ = lv_label_create(root_);
        lv_obj_set_style_text_font(contacts_title_, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(contacts_title_, lv_color_hex(COLOR_ACCENT), 0);
        lv_obj_set_pos(contacts_title_, kListX, 6);
        lv_obj_set_width(contacts_title_, kListW);
        lv_label_set_long_mode(contacts_title_, LV_LABEL_LONG_DOT);

        contacts_list_panel_ = lv_obj_create(root_);
        lv_obj_remove_style_all(contacts_list_panel_);
        lv_obj_set_pos(contacts_list_panel_, kListX, kListTop);
        lv_obj_set_size(contacts_list_panel_, kListW, kVisibleRows * kRowH + 8);
        lv_obj_set_style_bg_color(contacts_list_panel_, lv_color_hex(COLOR_PANEL), 0);
        lv_obj_set_style_bg_opa(contacts_list_panel_, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(contacts_list_panel_, 1, 0);
        lv_obj_set_style_border_color(contacts_list_panel_, lv_color_hex(COLOR_BORDER), 0);
        lv_obj_set_style_radius(contacts_list_panel_, 0, 0);
        lv_obj_set_style_pad_all(contacts_list_panel_, 4, 0);
        lv_obj_clear_flag(contacts_list_panel_, LV_OBJ_FLAG_SCROLLABLE);

        for (int i = 0; i < kVisibleRows; i++) {
            lv_obj_t *row = lv_label_create(contacts_list_panel_);
            lv_obj_set_style_text_font(row, &lv_font_montserrat_14, 0);
            lv_obj_set_pos(row, 0, i * kRowH);
            lv_obj_set_size(row, kListW - 8, kRowH);
            lv_label_set_long_mode(row, LV_LABEL_LONG_DOT);
            contacts_rows_[i] = row;
        }
    }

    void load_contacts()
    {
        contacts_.clear();
        sel_ = 0;
        scroll_ = 0;

        DIR *d = opendir(CONTACTS_DIR);
        if (!d) return;
        struct dirent *de;
        while ((de = readdir(d)) != nullptr) {
            const char *fname = de->d_name;
            if (fname[0] == '.') continue;
            size_t flen = strlen(fname);
            if (flen < 5 || strcmp(fname + flen - 4, ".cfg") != 0) continue;

            std::string full = std::string(CONTACTS_DIR) + "/" + fname;
            Contact c;
            c.file = fname;
            if (!parse_contact_file(full, c)) continue;
            contacts_.push_back(std::move(c));
        }
        closedir(d);

        std::sort(contacts_.begin(), contacts_.end(), [](const Contact &a, const Contact &b) {
            const std::string &an = a.name.empty() ? a.file : a.name;
            const std::string &bn = b.name.empty() ? b.file : b.name;
            return an < bn;
        });
    }

    void ensure_visible()
    {
        if (sel_ < scroll_) scroll_ = sel_;
        else if (sel_ >= scroll_ + kVisibleRows) scroll_ = sel_ - kVisibleRows + 1;
    }

    void render_contacts_list()
    {
        if (contacts_title_) {
            char tbuf[64];
            snprintf(tbuf, sizeof(tbuf), "Contacts (%d)", (int)contacts_.size());
            lv_label_set_text(contacts_title_, tbuf);
        }

        for (int i = 0; i < kVisibleRows; i++) {
            lv_obj_t *row = contacts_rows_[i];
            int idx = scroll_ + i;
            if (idx >= (int)contacts_.size()) {
                lv_label_set_text(row, "");
                lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
                continue;
            }
            const Contact &c = contacts_[idx];
            const char *display = c.name.empty() ? "(no name)" : c.name.c_str();
            bool focused = (idx == sel_);
            char buf[160];
            snprintf(buf, sizeof(buf), "%c %s", focused ? '>' : ' ', display);
            lv_label_set_text(row, buf);

            lv_obj_set_style_bg_color(row, lv_color_hex(COLOR_FOCUS), 0);
            lv_obj_set_style_bg_opa(row, focused ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
            lv_obj_set_style_radius(row, 0, 0);
            lv_obj_set_style_text_color(row,
                lv_color_hex(focused ? COLOR_ACCENT : COLOR_TEXT), 0);
        }

        if (contacts_.empty()) {
            lv_label_set_text(contacts_rows_[0], " No Contacts Yet");
            lv_label_set_text(contacts_rows_[1], " Bump Someone!");
            for (int i = 0; i < 2; ++i) {
                lv_obj_set_style_text_color(contacts_rows_[i], lv_color_hex(COLOR_TEXT_DIM), 0);
            }
        }
        set_view_hint(contacts_.empty() ? "B/Start  Bump     Home  Apps"
                                       : "Up/Down  Browse     A  Details\nB/Start  Bump     Home  Apps");
    }

    void open_detail()
    {
        if (sel_ < 0 || sel_ >= (int)contacts_.size()) return;
        viewing_detail_ = true;
        if (contacts_list_panel_) lv_obj_add_flag(contacts_list_panel_, LV_OBJ_FLAG_HIDDEN);

        detail_panel_ = lv_obj_create(root_);
        lv_obj_remove_style_all(detail_panel_);
        lv_obj_set_pos(detail_panel_, kListX, kListTop);
        // Keep the two-line controls below the contact card.
        lv_obj_set_size(detail_panel_, kListW, 240 - kListTop - 42);
        lv_obj_set_style_bg_color(detail_panel_, lv_color_hex(COLOR_PANEL), 0);
        lv_obj_set_style_bg_opa(detail_panel_, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(detail_panel_, 1, 0);
        lv_obj_set_style_border_color(detail_panel_, lv_color_hex(COLOR_BORDER), 0);
        lv_obj_set_style_radius(detail_panel_, 0, 0);
        lv_obj_set_style_pad_all(detail_panel_, 8, 0);
        lv_obj_clear_flag(detail_panel_, LV_OBJ_FLAG_SCROLLABLE);

        detail_name_ = lv_label_create(detail_panel_);
        lv_obj_set_style_text_font(detail_name_, &lv_font_montserrat_18, 0);
        lv_obj_set_style_text_color(detail_name_, lv_color_hex(COLOR_TEXT), 0);
        lv_obj_set_pos(detail_name_, 0, 0);
        lv_obj_set_size(detail_name_, kListW - 16, 22);
        lv_label_set_long_mode(detail_name_, LV_LABEL_LONG_DOT);

        for (int i = 0; i < kMaxContactLinks; i++) {
            lv_obj_t *row = lv_label_create(detail_panel_);
            lv_obj_set_style_text_font(row, &lv_font_montserrat_12, 0);
            lv_obj_set_pos(row, 0, 29 + i * 20);
            lv_obj_set_size(row, kListW - 16, 19);
            lv_obj_set_style_radius(row, 0, 0);
            lv_label_set_long_mode(row, LV_LABEL_LONG_DOT);
            detail_link_rows_[i] = row;
        }

        render_detail();
        set_view_hint("Up/Down Link   A QR   Left/Right Contact\nB List     Start Bump     Home Apps");
    }

    void render_detail()
    {
        if (!detail_panel_) return;
        const Contact &c = contacts_[sel_];

        if (contacts_title_) {
            char tbuf[64];
            snprintf(tbuf, sizeof(tbuf), "Contact %d/%d", sel_ + 1, (int)contacts_.size());
            lv_label_set_text(contacts_title_, tbuf);
        }

        lv_label_set_text(detail_name_, c.name.empty() ? "(no name)" : c.name.c_str());

        ContactLink links[kMaxContactLinks];
        contact_links(c, links);
        for (int i = 0; i < kMaxContactLinks; i++) {
            lv_obj_t *row = detail_link_rows_[i];
            const bool focused = i == detail_link_sel_;
            const bool shared = !links[i].value->empty();
            lv_label_set_text_fmt(row, "%c %s  %s", focused ? '>' : ' ',
                                  links[i].label,
                                  shared ? links[i].value->c_str() : "Not shared");
            lv_obj_set_style_bg_color(row, lv_color_hex(COLOR_FOCUS), 0);
            lv_obj_set_style_bg_opa(row, focused ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
            lv_obj_set_style_text_color(row,
                lv_color_hex(focused ? COLOR_ACCENT
                                     : (shared ? COLOR_TEXT : COLOR_TEXT_DIM)), 0);
        }
    }

    void show_contact_qr()
    {
        ContactLink link;
        if (!selected_contact_link(&link)) return;
        const Contact &contact = contacts_[sel_];

        viewing_qr_ = true;
        lv_obj_clean(root_);
        null_all();

        lv_obj_t *title = lv_label_create(root_);
        lv_label_set_text_fmt(title, "%s - %s", link.label,
                              contact.name.empty() ? "Contact" : contact.name.c_str());
        lv_obj_set_width(title, 304);
        lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(title, lv_color_hex(COLOR_ACCENT), 0);
        lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 5);

        if (link.value->empty()) {
            lv_obj_t *missing = lv_label_create(root_);
            lv_label_set_text_fmt(missing, "%s did not share\n%s",
                                  contact.name.empty() ? "This contact" : contact.name.c_str(),
                                  link.label);
            lv_obj_set_width(missing, 288);
            lv_obj_set_style_text_align(missing, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_set_style_text_font(missing, &lv_font_montserrat_18, 0);
            lv_obj_set_style_text_color(missing, lv_color_hex(COLOR_TEXT_DIM), 0);
            lv_obj_align(missing, LV_ALIGN_CENTER, 0, -4);
        } else {
            char payload[160];
            contact_qr_payload(link, payload, sizeof(payload));

            lv_obj_t *qr = lv_qrcode_create(root_);
            lv_qrcode_set_size(qr, 144);
            lv_qrcode_set_dark_color(qr, lv_color_hex(0x000000));
            lv_qrcode_set_light_color(qr, lv_color_hex(0xffffff));
            lv_qrcode_set_data(qr, payload);
            lv_obj_align(qr, LV_ALIGN_TOP_MID, 0, 34);

            lv_obj_t *value = lv_label_create(root_);
            lv_label_set_text(value, link.value->c_str());
            lv_obj_set_width(value, 304);
            lv_label_set_long_mode(value, LV_LABEL_LONG_DOT);
            lv_obj_set_style_text_align(value, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_set_style_text_font(value, &lv_font_montserrat_14, 0);
            lv_obj_set_style_text_color(value, lv_color_hex(COLOR_TEXT), 0);
            lv_obj_align(value, LV_ALIGN_TOP_MID, 0, 184);
        }

        set_view_hint("Up/Down Link   Left/Right Contact\nB Details   Start Bump   Home Apps");
    }

    void close_contact_qr()
    {
        viewing_qr_ = false;
        lv_obj_clean(root_);
        null_all();
        build_contacts_list_ui();
        open_detail();
    }

    void close_detail()
    {
        viewing_detail_ = false;
        if (detail_panel_) {
            lv_obj_del(detail_panel_);
            detail_panel_ = nullptr;
            detail_name_ = nullptr;
            for (auto &row : detail_link_rows_) row = nullptr;
        }
        if (contacts_list_panel_) lv_obj_clear_flag(contacts_list_panel_, LV_OBJ_FLAG_HIDDEN);
        render_contacts_list();
    }

    lv_obj_t *root_ = nullptr;
    Mode mode_ = Mode::Contacts;
    lv_obj_t *view_hint_ = nullptr;

    // Bump view widgets and engine state.
    lv_obj_t *bump_status_ = nullptr;
    lv_obj_t *bump_last_synced_ = nullptr;
    lv_obj_t *bump_contact_count_ = nullptr;
    lv_obj_t *bump_rings_[2] = {};

    BumpCard owner_{};
    bool owner_ready_ = false;
    bool radio_ok_ = false;
    bool radio_on_ = false;  // receiver powered only inside the sync window
    char pending_ack_[sizeof(BumpCard::badge_id)] = {};
    ButtonBump::Attempt button_attempt_;
    bool button_saved_ = false;

    CombinedAxisBumpDetector bump_detector_;
    int64_t result_until_ = 0;
    bool result_failed_ = false;
    bool last_sync_was_duplicate_ = false;

    int64_t bump_started_at_ = 0;
    int64_t bump_until_ = 0;
    bool bump_saved_ = false;
    bool tx_dirty_ = false;
    int64_t tx_retry_at_ = 0;
    size_t contact_count_ = 0;

    int ring_phase_ = 0;
    int ring_age_[2] = {};

    std::array<PeerSeen, 4> recent_peers_{};

    SemaphoreHandle_t rx_mutex_ = nullptr;
    std::array<Incoming, kRxQueueMax> rx_queue_{};
    size_t rx_head_ = 0;
    size_t rx_count_ = 0;

    // Contacts view widgets and state.
    lv_obj_t *contacts_title_ = nullptr;
    lv_obj_t *contacts_list_panel_ = nullptr;
    lv_obj_t *contacts_rows_[kVisibleRows] = {};

    lv_obj_t *detail_panel_ = nullptr;
    lv_obj_t *detail_name_ = nullptr;
    lv_obj_t *detail_link_rows_[kMaxContactLinks] = {};

    std::vector<Contact> contacts_;
    int sel_ = 0;
    int scroll_ = 0;
    int detail_link_sel_ = 0;
    bool viewing_detail_ = false;
    bool viewing_qr_ = false;
};

static ConnectApp s_instance;
App *app_connect_instance() { return &s_instance; }
