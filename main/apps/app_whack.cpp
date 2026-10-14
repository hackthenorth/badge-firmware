#include "app.h"
#include "hal_config.h"
#include "hal_lights.h"

#include "esp_random.h"
#include <cstdio>

// Whack-a-mole on a 3x3 grid. D-pad moves the cursor, A whacks.
// 30 seconds, most bonks wins.

extern "C" const lv_image_dsc_t icon_whack;

class WhackApp : public GameApp {
public:
    const char *name() const override { return "Whack"; }
    const char *icon() const override { return "WHK"; }
    const lv_image_dsc_t *icon_image() const override { return &icon_whack; }
    const char *slug() const override { return "whack"; }

    void on_enter(lv_obj_t *root) override
    {
        best_ = hal_config_get_int("whack", "best", 0);

        title_ = lv_label_create(root);
        lv_label_set_text(title_, "Whack");
        lv_obj_set_style_text_color(title_, lv_color_hex(BadgeTheme::TEXT), 0);
        lv_obj_set_style_text_font(title_, &lv_font_montserrat_20, 0);
        lv_obj_align(title_, LV_ALIGN_TOP_MID, 0, 6);

        score_ = lv_label_create(root);
        lv_obj_set_style_text_color(score_, lv_color_hex(BadgeTheme::ACCENT_DETAIL), 0);
        lv_obj_set_style_text_font(score_, &lv_font_montserrat_18, 0);
        lv_obj_align(score_, LV_ALIGN_TOP_MID, 0, 34);

        constexpr int kCell = 46, kPad = 6;
        int bx = 160 - (3 * kCell + 2 * kPad) / 2;
        int by = 62;
        for (int i = 0; i < 9; i++) {
            lv_obj_t *cell = lv_obj_create(root);
            lv_obj_remove_style_all(cell);
            lv_obj_set_size(cell, kCell, kCell);
            lv_obj_set_pos(cell, bx + (i % 3) * (kCell + kPad),
                           by + (i / 3) * (kCell + kPad));
            lv_obj_set_style_bg_color(cell, lv_color_hex(BadgeTheme::SURFACE), 0);
            lv_obj_set_style_bg_opa(cell, LV_OPA_COVER, 0);
            lv_obj_set_style_radius(cell, 8, 0);
            lv_obj_set_style_border_width(cell, 2, 0);
            lv_obj_set_style_border_color(cell, lv_color_hex(BadgeTheme::BORDER), 0);
            lv_obj_clear_flag(cell, LV_OBJ_FLAG_SCROLLABLE);

            lv_obj_t *dot = lv_obj_create(cell);
            lv_obj_remove_style_all(dot);
            lv_obj_set_size(dot, 22, 22);
            lv_obj_center(dot);
            lv_obj_set_style_bg_color(dot, lv_color_hex(BadgeTheme::TRACK), 0);
            lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
            lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
            lv_obj_clear_flag(dot, LV_OBJ_FLAG_SCROLLABLE);

            cells_[i] = cell;
            dots_[i] = dot;
        }

        hint_ = lv_label_create(root);
        lv_obj_set_style_text_color(hint_, lv_color_hex(BadgeTheme::TEXT_MUTED), 0);
        lv_obj_align(hint_, LV_ALIGN_BOTTOM_MID, 0, -10);

        reset();
    }

    void on_exit() override
    {
        hal_lights_clear();
        hal_lights_show();
        title_ = score_ = hint_ = nullptr;
        for (int i = 0; i < 9; i++) cells_[i] = dots_[i] = nullptr;
    }

    void checkpoint_progress() override
    {
        if (score_n_ <= best_) return;
        best_ = score_n_;
        hal_config_set_int("whack", "best", best_);
    }

    void on_button(ButtonEvent ev) override
    {
        if (ev.kind != ButtonEventKind::Pressed) return;
        if (!live_ && ev.button == Button::A) {
            reset();
            return;
        }
        if (!live_) return;
        switch (ev.button) {
            case Button::Up:    if (cursor_ > 2) cursor_ -= 3; break;
            case Button::Down:  if (cursor_ < 6) cursor_ += 3; break;
            case Button::Left:  if (cursor_ % 3 > 0) cursor_--; break;
            case Button::Right: if (cursor_ % 3 < 2) cursor_++; break;
            case Button::A:     whack(); break;
            default: break;
        }
        render();
    }

    void on_tick() override
    {
        if (!live_) return;
        if (--time_left_ <= 0) {
            finish();
            return;
        }
        if (mole_ >= 0 && --mole_left_ <= 0) {
            mole_ = -1;
            render();
        }
        if (mole_ < 0) {
            mole_ = (int)(esp_random() % 9);
            mole_left_ = 25 + (int)(esp_random() % 25); // 0.5 - 1.0 s
            flash_mole_led();
            render();
        }
    }

private:
    void reset()
    {
        score_n_ = 0;
        cursor_ = 4;
        mole_ = -1;
        time_left_ = 30 * 50; // 30 s at 20 ms ticks
        live_ = true;
        hal_lights_clear();
        hal_lights_show();
        render();
    }

    void finish()
    {
        live_ = false;
        mole_ = -1;
        if (score_n_ > best_) {
            checkpoint_progress();
            hal_lights_flash_success();
        } else {
            hal_lights_clear();
            hal_lights_show();
        }
        render();
    }

    void whack()
    {
        if (mole_ >= 0 && cursor_ == mole_) {
            score_n_++;
            mole_ = -1;
            flash_hit_led();
        }
    }

    void flash_mole_led()
    {
        hal_lights_clear();
        // Map the 3x3 cell onto the 6-LED ring: left/center/right columns.
        int col = mole_ % 3;
        for (size_t i = 0; i < LED_COUNT; i++) {
            int led_col = (i == 0 || i == 5 || i == 4) ? 0 : (i == 1 || i == 2 || i == 3) ? 2 : 1;
            if (led_col == col) hal_lights_set(i, 255, 160, 0);
        }
        hal_lights_show();
    }

    void flash_hit_led()
    {
        for (size_t i = 0; i < LED_COUNT; i++) hal_lights_set(i, 0, 255, 0);
        hal_lights_show();
    }

    void render()
    {
        if (score_) {
            lv_label_set_text_fmt(score_, "%d  (%ds)  best %d", score_n_,
                                  (time_left_ + 49) / 50, best_);
        }
        for (int i = 0; i < 9; i++) {
            if (!cells_[i] || !dots_[i]) continue;
            bool cursor = (i == cursor_) && live_;
            lv_obj_set_style_border_color(cells_[i],
                lv_color_hex(cursor ? 0xffffff : BadgeTheme::BORDER), 0);
            lv_obj_set_style_border_width(cells_[i], cursor ? 3 : 2, 0);
            lv_obj_set_style_bg_color(dots_[i],
                lv_color_hex(i == mole_ ? 0xff9e2c : BadgeTheme::TRACK), 0);
        }
        if (hint_) {
            lv_label_set_text(hint_, live_ ? "D-pad: aim  A: whack" : "A: play again");
        }
    }

    lv_obj_t *title_ = nullptr;
    lv_obj_t *score_ = nullptr;
    lv_obj_t *hint_ = nullptr;
    lv_obj_t *cells_[9] = {};
    lv_obj_t *dots_[9] = {};
    int cursor_ = 4;
    int mole_ = -1;
    int mole_left_ = 0;
    int time_left_ = 0;
    int score_n_ = 0;
    int best_ = 0;
    bool live_ = false;
};

static WhackApp s_instance;
App *app_whack_instance() { return &s_instance; }
