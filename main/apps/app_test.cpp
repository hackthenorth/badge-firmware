#include "app.h"
#include "app_launcher.h"
#include "app_registry.h"

extern "C" const lv_image_dsc_t icon_test;
#include "hal_accel.h"
#include "hal_config.h"
#include "hal_lights.h"
#include "hal_nfc.h"
#include "hal_identity.h"

#include "esp_system.h"

#include <cstdio>
#include <cstring>

// Hardware bring-up checklist. Always registered so an engineer can reveal it
// through the internal-app shortcut, but never visible in attendee modes.
class TestApp : public App {
public:
    const char *name() const override { return "Test"; }
    const char *icon() const override { return "TST"; }
    const lv_image_dsc_t *icon_image() const override { return &icon_test; }
    const char *slug() const override { return "test"; }
    AppAccess access() const override { return AppAccess::Internal; }
    bool handles_home_button() const override { return true; }
    bool wants_wake_lock() const override { return true; }

    void on_enter(lv_obj_t *root) override
    {
        root_ = root;
        step_ = Step::Buttons;
        memset(buttons_seen_, 0, sizeof(buttons_seen_));
        lights_tick_ = 0;
        nfc_card_seen_ = false;
        nfc_skipped_ = false;
        nfc_enabled_ = false;
        home_held_ = false;
        home_hold_ticks_ = 0;
        build_step_ui();
    }

    void on_exit() override
    {
        if (nfc_enabled_) {
            // Registry will also force-disable, but be explicit.
            hal_nfc_disable();
            nfc_enabled_ = false;
        }
        hal_lights_clear();
        hal_lights_show();
        root_ = nullptr;
        title_ = hint_ = nullptr;
        for (auto &p : pads_) p = nullptr;
        for (auto &l : accel_labels_) l = nullptr;
        nfc_status_ = nullptr;
        home_held_ = false;
        home_hold_ticks_ = 0;
    }

    void on_tick() override
    {
        // HOME is part of the nine-switch test, so a tap must be recorded
        // rather than treated as Home. A deliberate hold still provides an
        // escape hatch from any point in the hardware flow.
        if (home_held_ && ++home_hold_ticks_ >= kExitHoldTicks) {
            home_held_ = false;
            home_hold_ticks_ = 0;
            exit_test();
            return;
        }

        switch (step_) {
            case Step::Accel:    tick_accel();  break;
            case Step::Lights:   tick_lights(); break;
            case Step::NFC:      tick_nfc();    break;
            default: break;
        }
    }

    void on_button(ButtonEvent ev) override
    {
        if (ev.button == Button::Home) {
            if (ev.kind == ButtonEventKind::Pressed) {
                home_held_ = true;
                home_hold_ticks_ = 0;
            } else {
                home_held_ = false;
                home_hold_ticks_ = 0;
                return;
            }
        }
        if (ev.kind != ButtonEventKind::Pressed) return;

        switch (step_) {
            case Step::Buttons:   handle_buttons_step(ev.button); break;
            case Step::Screen:    if (ev.button == Button::A) advance(); break;
            case Step::Accel:     if (ev.button == Button::A) advance(); break;
            case Step::Lights:    if (ev.button == Button::A) advance(); break;
            case Step::NFC:
                if (ev.button == Button::A) {
                    nfc_skipped_ = !nfc_card_seen_;
                    advance();
                }
                break;
            case Step::Checklist: if (ev.button == Button::A) finish_test(); break;
        }
    }

private:
    enum class Step { Buttons, Screen, Accel, Lights, NFC, Checklist };
    static constexpr uint32_t kExitHoldTicks = 75; // 1.5 seconds at 20 ms/tick

    // ---- Step transitions --------------------------------------------------

    void advance()
    {
        switch (step_) {
            case Step::Buttons:   step_ = Step::Screen;    break;
            case Step::Screen:    step_ = Step::Accel;     break;
            case Step::Accel:     step_ = Step::Lights;    break;
            case Step::Lights:    step_ = Step::NFC;       break;
            case Step::NFC:       step_ = Step::Checklist; break;
            case Step::Checklist: return; // Use finish_and_reboot
        }
        build_step_ui();
    }

    void exit_test()
    {
        if (hal_identity_is_provisioned()) app_registry_return_to_launcher();
        else                               app_registry_launch(app_badge_id_instance());
    }

    void finish_test()
    {
        hal_config_set_int("test", "passed", 1);
        if (hal_identity_is_provisioned()) esp_restart();
        else                               app_registry_launch(app_badge_id_instance());
    }

    // ---- UI scaffolding ----------------------------------------------------

    void build_step_ui()
    {
        if (!root_) return;

        // Tear down NFC if we're leaving the NFC step.
        if (step_ != Step::NFC && nfc_enabled_) {
            hal_nfc_disable();
            nfc_enabled_ = false;
        }
        // Tear down lights if we're leaving the lights step.
        if (step_ != Step::Lights) {
            hal_lights_clear();
            hal_lights_show();
        }

        lv_obj_clean(root_);
        title_ = hint_ = nullptr;
        for (auto &p : pads_) p = nullptr;
        for (auto &l : accel_labels_) l = nullptr;
        nfc_status_ = nullptr;

        title_ = lv_label_create(root_);
        lv_obj_set_style_text_color(title_, lv_color_white(), 0);
        lv_obj_set_style_text_font(title_, &lv_font_montserrat_20, 0);
        lv_obj_align(title_, LV_ALIGN_TOP_MID, 0, 6);

        hint_ = lv_label_create(root_);
        lv_obj_set_style_text_color(hint_, lv_color_hex(BadgeTheme::TEXT_MUTED), 0);
        lv_obj_set_style_text_font(hint_, &lv_font_montserrat_14, 0);
        lv_obj_align(hint_, LV_ALIGN_BOTTOM_MID, 0, -6);

        switch (step_) {
            case Step::Buttons:   build_buttons();   break;
            case Step::Screen:    build_screen();    break;
            case Step::Accel:     build_accel();     break;
            case Step::Lights:    build_lights();    break;
            case Step::NFC:       build_nfc();       break;
            case Step::Checklist: build_checklist(); break;
        }
    }

    // ---- Step 1: Buttons ---------------------------------------------------

    void build_buttons()
    {
        lv_label_set_text(title_, "Press every button");
        lv_label_set_text(hint_, "All 9 green  |  Hold HOME: exit");

        make_pad(Button::Aux1,  52,  61, "SideSwitch", 96);
        make_pad(Button::Up,    82,  99, "UP");
        make_pad(Button::Left,  46, 135, "LEFT");
        make_pad(Button::Right,118, 135, "RIGHT");
        make_pad(Button::Down,  82, 171, "DOWN");
        make_pad(Button::B,    224, 112, "B", 44);
        make_pad(Button::A,    280, 112, "A", 44);
        make_pad(Button::Home, 216, 168, "Home", 60);
        make_pad(Button::Start,280, 168, "Start", 60);

        refresh_pads();
    }

    void make_pad(Button b, int cx, int cy, const char *label, int w = 48)
    {
        constexpr int h = 30;
        lv_obj_t *pad = lv_obj_create(root_);
        lv_obj_remove_style_all(pad);
        lv_obj_set_size(pad, w, h);
        lv_obj_set_pos(pad, cx - w / 2, cy - h / 2);
        lv_obj_set_style_radius(pad, 0, 0);
        lv_obj_set_style_bg_opa(pad, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(pad, 2, 0);
        lv_obj_clear_flag(pad, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *lbl = lv_label_create(pad);
        lv_label_set_text(lbl, label);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
        lv_obj_center(lbl);

        pads_[(int)b] = pad;
    }

    void refresh_pads()
    {
        for (int i = 0; i < (int)Button::COUNT; i++) {
            update_pad((Button)i);
        }
    }

    void update_pad(Button b)
    {
        lv_obj_t *pad = pads_[(int)b];
        if (!pad) return;
        bool seen = buttons_seen_[(int)b];
        uint32_t bg     = seen ? 0x00a050 : BadgeTheme::SURFACE;
        uint32_t border = seen ? 0xffffff : BadgeTheme::BORDER;
        uint32_t text   = seen ? 0xffffff : BadgeTheme::TEXT;
        lv_obj_set_style_bg_color(pad, lv_color_hex(bg), 0);
        lv_obj_set_style_border_color(pad, lv_color_hex(border), 0);
        lv_obj_t *lbl = lv_obj_get_child(pad, 0);
        if (lbl) lv_obj_set_style_text_color(lbl, lv_color_hex(text), 0);
    }

    void handle_buttons_step(Button b)
    {
        if ((int)b >= (int)Button::COUNT) return;
        if (buttons_seen_[(int)b]) return;

        buttons_seen_[(int)b] = true;
        update_pad(b);

        if (all_buttons_seen()) advance();
    }

    bool all_buttons_seen() const
    {
        for (int i = 0; i < (int)Button::COUNT; i++) {
            if (!buttons_seen_[i]) return false;
        }
        return true;
    }

    // ---- Step 2: Screen ----------------------------------------------------

    void build_screen()
    {
        lv_label_set_text(title_, "Screen test");
        lv_label_set_text(hint_, "A: next  |  Hold HOME: exit");

        // 320x240 screen; place 10x10 white squares flush at the four corners.
        constexpr int sz = 10;
        constexpr int w  = 320;
        constexpr int h  = 240;
        const struct { int x, y; } corners[4] = {
            { 0,        0        },
            { w - sz,   0        },
            { 0,        h - sz   },
            { w - sz,   h - sz   },
        };
        for (auto &c : corners) {
            lv_obj_t *sq = lv_obj_create(root_);
            lv_obj_remove_style_all(sq);
            lv_obj_set_size(sq, sz, sz);
            lv_obj_set_pos(sq, c.x, c.y);
            lv_obj_set_style_bg_color(sq, lv_color_white(), 0);
            lv_obj_set_style_bg_opa(sq, LV_OPA_COVER, 0);
        }

        lv_obj_t *msg = lv_label_create(root_);
        lv_obj_set_style_text_color(msg, lv_color_white(), 0);
        lv_label_set_text(msg, "Check fiducials at all four corners");
        lv_obj_align(msg, LV_ALIGN_CENTER, 0, 0);
    }

    // ---- Step 3: Accelerometer --------------------------------------------

    void build_accel()
    {
        lv_label_set_text(title_, "Accelerometer");
        lv_label_set_text(hint_, "A: next  |  Hold HOME: exit");

        const char *labels[3] = {"X", "Y", "Z"};
        for (int i = 0; i < 3; i++) {
            lv_obj_t *axis = lv_label_create(root_);
            lv_label_set_text(axis, labels[i]);
            lv_obj_set_style_text_color(axis, lv_color_hex(BadgeTheme::TEXT_MUTED), 0);
            lv_obj_set_style_text_font(axis, &lv_font_montserrat_20, 0);
            lv_obj_set_pos(axis, 60, 60 + i * 40);

            accel_labels_[i] = lv_label_create(root_);
            lv_obj_set_style_text_color(accel_labels_[i], lv_color_white(), 0);
            lv_obj_set_style_text_font(accel_labels_[i], &lv_font_montserrat_20, 0);
            lv_obj_set_pos(accel_labels_[i], 100, 60 + i * 40);
            lv_label_set_text(accel_labels_[i], "    0 mg");
        }
    }

    void tick_accel()
    {
        AccelSample s;
        if (hal_accel_read(&s) != ESP_OK) return;
        const float vals[3] = {s.x_mg, s.y_mg, s.z_mg};
        for (int i = 0; i < 3; i++) {
            if (!accel_labels_[i]) continue;
            char buf[16];
            snprintf(buf, sizeof(buf), "%+5d mg", (int)vals[i]);
            lv_label_set_text(accel_labels_[i], buf);
        }
    }

    // ---- Step 4: Lights ----------------------------------------------------

    void build_lights()
    {
        lv_label_set_text(title_, "Lights test");
        lv_label_set_text(hint_, "A: next  |  Hold HOME: exit");

        lv_obj_t *msg = lv_label_create(root_);
        lv_obj_set_style_text_color(msg, lv_color_white(), 0);
        lv_label_set_text(msg, "All 6 LEDs cycle R / G / B / W");
        lv_obj_align(msg, LV_ALIGN_CENTER, 0, 0);

        lights_tick_ = 0;
    }

    void tick_lights()
    {
        // on_tick runs ~50 Hz (20ms). 25 ticks ~= 500ms per color.
        constexpr uint32_t ticks_per_color = 25;
        uint32_t phase = (lights_tick_ / ticks_per_color) % 4;
        lights_tick_++;

        uint8_t r = 0, g = 0, b = 0;
        switch (phase) {
            case 0: r = 64;            break; // Red
            case 1: g = 64;            break; // Green
            case 2: b = 64;            break; // Blue
            case 3: r = g = b = 64;    break; // White
        }
        for (size_t i = 0; i < LED_COUNT; i++) hal_lights_set(i, r, g, b);
        hal_lights_show();
    }

    // ---- Step 5: NFC -------------------------------------------------------

    void build_nfc()
    {
        lv_label_set_text(title_, "NFC test");
        lv_label_set_text(hint_, "Scan card  |  A: skip  |  Hold HOME: exit");

        hal_nfc_clear_last_card();
        hal_nfc_enable();
        nfc_enabled_ = true;
        nfc_card_seen_ = false;

        nfc_status_ = lv_label_create(root_);
        lv_obj_set_style_text_color(nfc_status_, lv_color_white(), 0);
        lv_label_set_text(nfc_status_, "Waiting for card...");
        lv_obj_align(nfc_status_, LV_ALIGN_CENTER, 0, 0);
    }

    void tick_nfc()
    {
        if (nfc_card_seen_) return;
        NfcCard card;
        if (!hal_nfc_last_card(&card)) return;

        char uid_buf[40];
        int n = 0;
        for (size_t i = 0; i < card.uid_len && n < (int)sizeof(uid_buf) - 3; i++) {
            n += snprintf(uid_buf + n, sizeof(uid_buf) - n, "%02X ", card.uid[i]);
        }
        if (nfc_status_) lv_label_set_text_fmt(nfc_status_, "UID: %s", uid_buf);

        nfc_card_seen_ = true;
        if (hint_) lv_label_set_text(hint_, "A: next  |  Hold HOME: exit");
    }

    // ---- Step 6: Checklist -------------------------------------------------

    void build_checklist()
    {
        lv_label_set_text(title_, "Hardware test complete");
        lv_label_set_text(hint_, hal_identity_is_provisioned()
            ? "A: finish/reboot  |  Hold HOME: exit"
            : "A: finish  |  Hold HOME: exit");

        const char *items[5] = {
            "[x] Buttons",
            "[x] Screen",
            "[x] Accelerometer",
            "[x] Lights",
            nfc_skipped_ ? "[-] NFC (skipped)" : "[x] NFC",
        };
        for (int i = 0; i < 5; i++) {
            lv_obj_t *row = lv_label_create(root_);
            lv_obj_set_style_text_color(row, lv_color_hex(BadgeTheme::ACCENT_DETAIL), 0);
            lv_obj_set_style_text_font(row, &lv_font_montserrat_20, 0);
            lv_obj_set_pos(row, 70, 50 + i * 26);
            lv_label_set_text(row, items[i]);
        }
    }

    // ---- State -------------------------------------------------------------

    Step step_ = Step::Buttons;
    lv_obj_t *root_ = nullptr;
    lv_obj_t *title_ = nullptr;
    lv_obj_t *hint_ = nullptr;

    bool buttons_seen_[(int)Button::COUNT] = {};
    lv_obj_t *pads_[(int)Button::COUNT] = {};

    lv_obj_t *accel_labels_[3] = {};

    uint32_t lights_tick_ = 0;

    bool nfc_enabled_ = false;
    bool nfc_card_seen_ = false;
    bool nfc_skipped_ = false;
    lv_obj_t *nfc_status_ = nullptr;

    bool home_held_ = false;
    uint32_t home_hold_ticks_ = 0;
};

static TestApp s_instance;
App *app_test_instance() { return &s_instance; }
