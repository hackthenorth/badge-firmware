#include "app.h"
#include "hal_config.h"
#include "hal_lights.h"

#include "esp_random.h"
#include <cstdio>

// Dance Floor: the LED-poi idea themed as a dancing game. Arrows fall
// down four lanes on the beat; hit the matching D-pad direction in the
// hit zone. Combo lights the LED ring; misses dim it. Every 8 notes the
// level rises (max 10): arrows fall faster, spawn denser, and the hit
// window tightens.

extern "C" const lv_image_dsc_t icon_dance;

class DanceApp : public GameApp {
public:
    const char *name() const override { return "Dance"; }
    const char *icon() const override { return "DNC"; }
    const lv_image_dsc_t *icon_image() const override { return &icon_dance; }
    const char *slug() const override { return "dance"; }

    bool wants_wake_lock() const override { return true; }

    void on_enter(lv_obj_t *root) override
    {
        best_ = hal_config_get_int("dance", "best", 0);

        score_ = lv_label_create(root);
        lv_obj_set_style_text_color(score_, lv_color_hex(BadgeTheme::TEXT), 0);
        lv_obj_set_style_text_font(score_, &lv_font_montserrat_18, 0);
        lv_obj_set_width(score_, 300);
        lv_obj_set_style_text_align(score_, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_long_mode(score_, LV_LABEL_LONG_WRAP);
        lv_obj_align(score_, LV_ALIGN_TOP_MID, 0, 4);

        field_ = lv_obj_create(root);
        lv_obj_set_style_pad_all(field_, 0, 0);
        lv_obj_set_size(field_, FIELD_W, FIELD_H);
        lv_obj_align(field_, LV_ALIGN_CENTER, 0, 10);
        lv_obj_set_style_bg_color(field_, lv_color_hex(BadgeTheme::PANEL), 0);
        lv_obj_set_style_border_width(field_, 0, 0);
        lv_obj_set_style_radius(field_, 4, 0);
        // Notes spawn above the top edge and fall past the bottom; without
        // this the field scrolls to chase them.
        lv_obj_clear_flag(field_, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_scrollbar_mode(field_, LV_SCROLLBAR_MODE_OFF);

        zone_ = lv_obj_create(field_);
        lv_obj_set_style_bg_color(zone_, lv_color_hex(BadgeTheme::ACCENT_DETAIL), 0);
        lv_obj_set_style_bg_opa(zone_, LV_OPA_20, 0);
        lv_obj_set_style_border_width(zone_, 1, 0);
        lv_obj_set_style_border_color(zone_,
                                      lv_color_hex(BadgeTheme::ACCENT_DETAIL), 0);
        lv_obj_set_style_radius(zone_, 0, 0);
        lv_obj_clear_flag(zone_, LV_OBJ_FLAG_SCROLLABLE);

        for (int i = 0; i < kMaxNotes; i++) {
            notes_[i].label = lv_label_create(field_);
            lv_obj_set_style_text_color(notes_[i].label, lv_color_hex(BadgeTheme::TEXT), 0);
            lv_obj_set_style_text_font(notes_[i].label, &lv_font_montserrat_20, 0);
            lv_obj_add_flag(notes_[i].label, LV_OBJ_FLAG_HIDDEN);
            notes_[i].active = false;
        }

        hint_ = lv_label_create(root);
        lv_obj_set_style_text_color(hint_, lv_color_hex(BadgeTheme::TEXT_MUTED), 0);
        lv_obj_set_width(hint_, 312);
        lv_obj_set_style_text_align(hint_, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_font(hint_, &lv_font_montserrat_12, 0);
        lv_obj_align(hint_, LV_ALIGN_BOTTOM_MID, 0, -5);

        reset();
    }

    void on_exit() override
    {
        hal_lights_clear();
        hal_lights_show();
        score_ = field_ = zone_ = hint_ = nullptr;
        for (int i = 0; i < kMaxNotes; i++) notes_[i].label = nullptr;
    }

    void checkpoint_progress() override
    {
        if (score_n_ <= best_) return;
        best_ = score_n_;
        hal_config_set_int("dance", "best", best_);
    }

    void on_button(ButtonEvent ev) override
    {
        if (ev.kind != ButtonEventKind::Pressed) return;
        if (!live_) {
            if (ev.button == Button::A) reset();
            return;
        }
        int dir = -1;
        switch (ev.button) {
            case Button::Left: dir = 0; break;
            case Button::Down: dir = 1; break;
            case Button::Up: dir = 2; break;
            case Button::Right: dir = 3; break;
            default: break;
        }
        if (dir >= 0) hit(dir);
    }

    void on_tick() override
    {
        if (!live_) return;
        tick_++;
        if (--time_left_ <= 0) {
            finish();
            return;
        }
        if (announce_left_ > 0) announce_left_--;
        if (--spawn_in_ <= 0) {
            spawn();
            spawn_in_ = spawn_interval();
            if ((++spawned_ % kSpawnsPerLevel) == 0 && level_ < kMaxLevel) {
                level_++;
                refresh_hit_zone();
                announce_left_ = 100; // 2 s "LEVEL n" banner
                hal_lights_flash_success();
            }
        }
        for (int i = 0; i < kMaxNotes; i++) {
            Note &n = notes_[i];
            if (!n.active) continue;
            n.y += speed();
            if (n.y > FIELD_H) {
                // Fumbled past the zone: combo drops.
                n.active = false;
                lv_obj_add_flag(n.label, LV_OBJ_FLAG_HIDDEN);
                combo_ = 0;
                miss();
                if (!live_) return;
            } else {
                lv_obj_set_pos(n.label, lane_x(n.dir), n.y);
            }
        }
        render();
    }

private:
    static constexpr int FIELD_W = 280;
    static constexpr int FIELD_H = 150;
    static constexpr int HIT_Y = 118;
    static constexpr int kMaxNotes = 10;
    static constexpr int kMaxLevel = 10;
    static constexpr int kSpawnsPerLevel = 8;
    static constexpr const char *kArrows[] = {
        LV_SYMBOL_LEFT, LV_SYMBOL_DOWN, LV_SYMBOL_UP, LV_SYMBOL_RIGHT,
    };

    struct Note {
        lv_obj_t *label = nullptr;
        bool active = false;
        int dir = 0;
        int y = 0;
    };

    // Fall speed in px/tick (50 ticks/s). Lv 1-5 ramps 3->6 px/tick as
    // before (3,4,5,6,6); past 5 it grows ~1 px every 3 levels, topping
    // out at 7 px/tick (350 px/s, ~0.5 s fall) at Lv 10 instead of 10
    // px/tick (500 px/s, ~0.3 s fall).
    int speed() const
    {
        if (level_ <= 5) return 3 + (level_ * 3) / 4;
        return 6 + (level_ - 5) / 3;
    }

    // Lv 1: a note every 24 ticks (~480 ms). Density still rises per
    // level, but the floor is 10 ticks (200 ms) so Lv 8+ never becomes
    // a wall of notes the way the old 5-tick floor did.
    int spawn_interval() const
    {
        int v = 26 - 2 * level_;
        return v < 10 ? 10 : v;
    }

    // Lv 1: +-19 px forgiveness. The floor stays at +-14 px so that at
    // the Lv 6+ speeds (~6-7 px/tick) a hit still allows +-2 ticks of
    // timing error instead of the old +-1.
    int hit_window() const
    {
        int v = 20 - level_;
        return v < 14 ? 14 : v;
    }

    static int lane_x(int dir) { return 20 + dir * 65; }

    void reset()
    {
        score_n_ = 0;
        combo_ = 0;
        misses_ = 0;
        level_ = 1;
        spawned_ = 0;
        tick_ = 0;
        announce_left_ = 0;
        time_left_ = 45 * 50; // 45 s track
        spawn_in_ = 20;
        live_ = true;
        for (int i = 0; i < kMaxNotes; i++) {
            notes_[i].active = false;
            if (notes_[i].label) lv_obj_add_flag(notes_[i].label, LV_OBJ_FLAG_HIDDEN);
        }
        hal_lights_clear();
        hal_lights_show();
        refresh_hit_zone();
        render();
    }

    void finish()
    {
        live_ = false;
        for (int i = 0; i < kMaxNotes; i++) {
            notes_[i].active = false;
            if (notes_[i].label) lv_obj_add_flag(notes_[i].label, LV_OBJ_FLAG_HIDDEN);
        }
        if (score_n_ > best_) {
            checkpoint_progress();
            hal_lights_flash_success();
        } else {
            hal_lights_clear();
            hal_lights_show();
        }
        render();
    }

    void spawn()
    {
        for (int i = 0; i < kMaxNotes; i++) {
            if (!notes_[i].active) {
                notes_[i].active = true;
                notes_[i].dir = (int)(esp_random() % 4);
                notes_[i].y = -20;
                lv_label_set_text(notes_[i].label, kArrows[notes_[i].dir]);
                lv_obj_set_pos(notes_[i].label, lane_x(notes_[i].dir), -20);
                lv_obj_clear_flag(notes_[i].label, LV_OBJ_FLAG_HIDDEN);
                return;
            }
        }
    }

    void hit(int dir)
    {
        int best_i = -1;
        int best_d = 1000;
        for (int i = 0; i < kMaxNotes; i++) {
            if (!notes_[i].active || notes_[i].dir != dir) continue;
            int d = notes_[i].y > HIT_Y ? notes_[i].y - HIT_Y : HIT_Y - notes_[i].y;
            if (d < best_d) {
                best_d = d;
                best_i = i;
            }
        }
        if (best_i >= 0 && best_d <= hit_window()) {
            notes_[best_i].active = false;
            lv_obj_add_flag(notes_[best_i].label, LV_OBJ_FLAG_HIDDEN);
            combo_++;
            if (misses_ > 0) --misses_;
            score_n_ += 10 + combo_ * 2;
            glow_leds();
        } else {
            // An early or late attempt consumes the arrow it targeted. Leaving
            // it active would charge one miss here and a second when that same
            // arrow falls off the field.
            if (best_i >= 0) {
                notes_[best_i].active = false;
                lv_obj_add_flag(notes_[best_i].label, LV_OBJ_FLAG_HIDDEN);
            }
            combo_ = 0;
            miss();
        }
        render();
    }

    void refresh_hit_zone()
    {
        if (!zone_) return;
        const int half_height = hit_window();
        lv_obj_set_size(zone_, FIELD_W, half_height * 2 + 1);
        lv_obj_set_pos(zone_, 0, HIT_Y - half_height);
    }

    static constexpr int kMissLimit = 5;
    void miss()
    {
        ++misses_;
        dim_leds();
        if (misses_ >= kMissLimit) finish();
    }

    void glow_leds()
    {
        // Combo heats the ring from violet toward white.
        uint8_t v = combo_ > 12 ? 255 : (uint8_t)(60 + combo_ * 16);
        for (size_t i = 0; i < LED_COUNT; i++) hal_lights_set(i, v, v / 3, v);
        hal_lights_show();
    }

    void dim_leds()
    {
        hal_lights_clear();
        hal_lights_show();
    }

    void render()
    {
        if (score_) {
            lv_label_set_text_fmt(score_, "Lv %d  %d pts  x%d  best %d", level_,
                                  score_n_, combo_, best_);
        }
        if (hint_) {
            if (!live_) lv_label_set_text(hint_, misses_ >= kMissLimit
                ? "GAME OVER - 5 misses. A: try again" : "TRACK COMPLETE! A: dance again");
            else if (announce_left_ > 0) {
                lv_label_set_text_fmt(hint_, "LEVEL %d - faster!  Misses %d/5", level_, misses_);
            } else {
                lv_label_set_text_fmt(hint_, "%ds left  Misses %d/5  Hit in box",
                                      (time_left_ + 49) / 50, misses_);
            }
        }
    }

    lv_obj_t *score_ = nullptr;
    lv_obj_t *field_ = nullptr;
    lv_obj_t *zone_ = nullptr;
    lv_obj_t *hint_ = nullptr;
    Note notes_[kMaxNotes];
    int score_n_ = 0;
    int combo_ = 0;
    int misses_ = 0;
    int best_ = 0;
    int level_ = 1;
    int announce_left_ = 0;
    int spawned_ = 0;
    int tick_ = 0;
    int time_left_ = 0;
    int spawn_in_ = 0;
    bool live_ = false;
};

static DanceApp s_instance;
App *app_dance_instance() { return &s_instance; }
