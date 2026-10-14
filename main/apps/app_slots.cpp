#include "app.h"
#include "hal_config.h"
#include "hal_lights.h"

#include "esp_random.h"
#include <cstdio>

// Slots. A starts the reels and stops them one at a time.
// Starts with 20 credits; the richest stack is remembered.

extern "C" const lv_image_dsc_t icon_slots;

class SlotsApp : public GameApp {
public:
    const char *name() const override { return "Slots"; }
    const char *icon() const override { return "7"; }
    const lv_image_dsc_t *icon_image() const override { return &icon_slots; }
    const char *slug() const override { return "slots"; }

    void on_enter(lv_obj_t *root) override
    {
        richest_ = hal_config_get_int("slots", "richest", 0);

        title_ = lv_label_create(root);
        lv_label_set_text(title_, "Slots");
        lv_obj_set_style_text_color(title_, lv_color_hex(BadgeTheme::TEXT), 0);
        lv_obj_set_style_text_font(title_, &lv_font_montserrat_20, 0);
        lv_obj_align(title_, LV_ALIGN_TOP_MID, 0, 6);

        credits_ = lv_label_create(root);
        lv_obj_set_style_text_color(credits_, lv_color_hex(BadgeTheme::ACCENT_DETAIL), 0);
        lv_obj_set_style_text_font(credits_, &lv_font_montserrat_18, 0);
        lv_obj_align(credits_, LV_ALIGN_TOP_MID, 0, 34);

        constexpr int kTile = 64, kPad = 12;
        int bx = 160 - (3 * kTile + 2 * kPad) / 2;
        for (int i = 0; i < 3; i++) {
            lv_obj_t *tile = lv_obj_create(root);
            lv_obj_remove_style_all(tile);
            lv_obj_set_size(tile, kTile, kTile);
            lv_obj_set_pos(tile, bx + i * (kTile + kPad), 76);
            lv_obj_set_style_bg_color(tile, lv_color_hex(BadgeTheme::SURFACE), 0);
            lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);
            lv_obj_set_style_radius(tile, 8, 0);
            lv_obj_set_style_border_width(tile, 2, 0);
            lv_obj_set_style_border_color(tile, lv_color_hex(BadgeTheme::BORDER), 0);
            lv_obj_clear_flag(tile, LV_OBJ_FLAG_SCROLLABLE);

            lv_obj_t *sym = lv_label_create(tile);
            lv_obj_set_style_text_font(sym, &lv_font_montserrat_24, 0);
            lv_obj_center(sym);
            tiles_[i] = tile;
            syms_[i] = sym;
        }

        msg_ = lv_label_create(root);
        lv_obj_set_style_text_color(msg_, lv_color_hex(BadgeTheme::TEXT_MUTED), 0);
        lv_obj_set_width(msg_, 300);
        lv_obj_set_style_text_align(msg_, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_long_mode(msg_, LV_LABEL_LONG_WRAP);
        lv_obj_align(msg_, LV_ALIGN_CENTER, 0, 56);

        hint_ = lv_label_create(root);
        lv_obj_set_style_text_color(hint_, lv_color_hex(BadgeTheme::TEXT_MUTED), 0);
        lv_label_set_text(hint_, "A: spin / stop");
        lv_obj_align(hint_, LV_ALIGN_BOTTOM_MID, 0, -10);

        credits_n_ = 20;
        spinning_ = 0;
        show_[0] = 0;
        show_[1] = 3;
        show_[2] = 5;
        set_leds(0, 0, 0);
        render();
    }

    void on_exit() override
    {
        hal_lights_clear();
        hal_lights_show();
        title_ = credits_ = msg_ = hint_ = nullptr;
        for (int i = 0; i < 3; i++) tiles_[i] = syms_[i] = nullptr;
    }

    void checkpoint_progress() override
    {
        if (credits_n_ <= richest_) return;
        richest_ = credits_n_;
        hal_config_set_int("slots", "richest", richest_);
    }

    void on_button(ButtonEvent ev) override
    {
        if (ev.kind != ButtonEventKind::Pressed || ev.button != Button::A) return;
        if (spinning_ == 0) {
            if (credits_n_ <= 0) {
                credits_n_ = 20; // house refills the broke
                if (msg_) lv_label_set_text(msg_, "on the house: 20");
            } else {
                credits_n_--;
            }
            spinning_ = 3;
            stopped_[0] = stopped_[1] = stopped_[2] = false;
            if (msg_) lv_label_set_text(msg_, "stopping: left first");
        } else {
            // Stop the leftmost still-spinning reel.
            for (int i = 0; i < 3; i++) {
                if (!stopped_[i]) {
                    stopped_[i] = true;
                    spinning_--;
                    break;
                }
            }
            if (spinning_ == 0) settle();
        }
        render();
    }

    void on_tick() override
    {
        if (spinning_ == 0) return;
        // Left reel spins fastest visually by updating least often looks odd;
        // simply advance every reel still running at different cadences.
        tick_++;
        for (int i = 0; i < 3; i++) {
            if (!stopped_[i] && (tick_ % (i + 1) == 0)) {
                show_[i] = (int)(esp_random() % kReelCount);
            }
        }
        render_reels();
    }

private:
    struct Reel {
        const char *sym;
        uint32_t color;
    };
    // Index 0 is the jackpot bell.
    static constexpr Reel kReels[] = {
        { LV_SYMBOL_BELL, 0xffc63a },   // gold jackpot
        { LV_SYMBOL_PLAY, 0x53d769 },   // green
        { LV_SYMBOL_CHARGE, 0xb087b1 }, // violet spark
        { LV_SYMBOL_GPS, 0xff9e2c },    // orange target
        { LV_SYMBOL_SAVE, 0xff5c8a },   // pink
        { LV_SYMBOL_OK, 0xf4f4ef },     // white tick
        { LV_SYMBOL_PLUS, 0xa0a09a },   // grey plus
    };
    static constexpr int kReelCount = sizeof(kReels) / sizeof(kReels[0]);

    void set_leds(uint8_t r, uint8_t g, uint8_t b)
    {
        for (size_t i = 0; i < LED_COUNT; i++) hal_lights_set(i, r, g, b);
        hal_lights_show();
    }

    static int payout(int a, int b, int c)
    {
        if (a == 0 && b == 0 && c == 0) return 50;
        if (a == b && b == c) return 10;
        if (a == b || b == c || a == c) return 2;
        return 0;
    }

    void settle()
    {
        int win = payout(show_[0], show_[1], show_[2]);
        credits_n_ += win;
        checkpoint_progress();
        if (win >= 10) {
            hal_lights_flash_success();
            if (msg_) lv_label_set_text_fmt(msg_, "JACKPOT +%d!", win);
        } else if (win > 0) {
            set_leds(0, 255, 0);
            if (msg_) lv_label_set_text_fmt(msg_, "+%d", win);
        } else {
            set_leds(255, 0, 0);
            if (msg_) lv_label_set_text(msg_, "no luck");
        }
    }

    void render_reels()
    {
        for (int i = 0; i < 3; i++) {
            if (!syms_[i]) continue;
            int s = show_[i];
            if (s < 0 || s >= kReelCount) s = 0;
            lv_label_set_text(syms_[i], kReels[s].sym);
            lv_obj_set_style_text_color(syms_[i], lv_color_hex(kReels[s].color), 0);
        }
    }

    void render()
    {
        if (credits_) {
            lv_label_set_text_fmt(credits_, "%d credits  richest %d", credits_n_, richest_);
        }
        render_reels();
    }

    lv_obj_t *title_ = nullptr;
    lv_obj_t *credits_ = nullptr;
    lv_obj_t *msg_ = nullptr;
    lv_obj_t *hint_ = nullptr;
    lv_obj_t *tiles_[3] = {};
    lv_obj_t *syms_[3] = {};
    int show_[3] = { 0, 3, 5 };
    bool stopped_[3] = { false, false, false };
    int spinning_ = 0;
    int tick_ = 0;
    int credits_n_ = 20;
    int richest_ = 0;
};

static SlotsApp s_instance;
App *app_slots_instance() { return &s_instance; }
