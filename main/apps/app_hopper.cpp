#include "app.h"
#include "hal_config.h"
#include "hal_lights.h"

#include <cstdio>

// Hopper: hop the frog to the top bank. Cars kill on the road, the river
// needs a log (which carries you). Fill all 5 home slots to level up and
// speed everything up. Best score is remembered.

extern "C" const lv_image_dsc_t icon_hopper;
extern "C" const lv_image_dsc_t spr_frog;
extern "C" const lv_image_dsc_t spr_car;
extern "C" const lv_image_dsc_t spr_log_long;
extern "C" const lv_image_dsc_t spr_log_short;

class HopperApp : public GameApp {
public:
    const char *name() const override { return "Hopper"; }
    const char *icon() const override { return "HP"; }
    const lv_image_dsc_t *icon_image() const override { return &icon_hopper; }
    const char *slug() const override { return "hopper"; }

    bool wants_wake_lock() const override { return true; }

    void on_enter(lv_obj_t *root) override
    {
        best_ = hal_config_get_int("hopper", "best", 0);

        hud_ = lv_label_create(root);
        lv_obj_set_style_text_color(hud_, lv_color_hex(BadgeTheme::TEXT), 0);
        lv_obj_set_style_text_font(hud_, &lv_font_montserrat_14, 0);
        lv_obj_set_width(hud_, 312);
        lv_obj_set_style_text_align(hud_, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_long_mode(hud_, LV_LABEL_LONG_WRAP);
        lv_obj_align(hud_, LV_ALIGN_TOP_MID, 0, 4);

        for (int r = 0; r < kRows; r++) {
            lv_obj_t *band = lv_obj_create(root);
            lv_obj_remove_style_all(band);
            lv_obj_set_size(band, kBoardW, kCellH);
            lv_obj_set_pos(band, kBx, kBy + r * kCellH);
            uint32_t col = BadgeTheme::PANEL;
            if (is_road(r)) col = 0x232323;
            else if (is_river(r)) col = BadgeTheme::TRACK;
            lv_obj_set_style_bg_color(band, lv_color_hex(col), 0);
            lv_obj_set_style_bg_opa(band, LV_OPA_COVER, 0);
            lv_obj_clear_flag(band, LV_OBJ_FLAG_SCROLLABLE);
        }

        for (int i = 0; i < 5; i++) {
            home_[i] = lv_obj_create(root);
            lv_obj_remove_style_all(home_[i]);
            lv_obj_set_size(home_[i], 12, 12);
            lv_obj_set_pos(home_[i], kBx + (int)(i * 62.4f + 62.4f / 2 - 6),
                           kBy + kRows * 0 + 5);
            lv_obj_set_style_bg_color(home_[i], lv_color_hex(0xffc63a), 0);
            lv_obj_set_style_bg_opa(home_[i], LV_OPA_COVER, 0);
            lv_obj_set_style_radius(home_[i], 2, 0);
            lv_obj_clear_flag(home_[i], LV_OBJ_FLAG_SCROLLABLE);
        }

        for (int i = 0; i < kCars; i++) {
            cars_[i] = lv_image_create(root);
            lv_image_set_src(cars_[i], &spr_car);
            lv_obj_clear_flag(cars_[i], LV_OBJ_FLAG_SCROLLABLE);
        }
        for (int i = 0; i < kLogs; i++) {
            logs_[i] = lv_image_create(root);
            lv_obj_clear_flag(logs_[i], LV_OBJ_FLAG_SCROLLABLE);
        }
        frog_ = lv_image_create(root);
        lv_image_set_src(frog_, &spr_frog);
        lv_obj_clear_flag(frog_, LV_OBJ_FLAG_SCROLLABLE);

        hint_ = lv_label_create(root);
        lv_label_set_text(hint_, "D-pad: hop to the top bank");
        lv_obj_set_style_text_color(hint_, lv_color_hex(BadgeTheme::TEXT_MUTED), 0);
        lv_obj_align(hint_, LV_ALIGN_BOTTOM_MID, 0, -6);

        reset_run();
    }

    void on_exit() override
    {
        hal_lights_clear();
        hal_lights_show();
        hud_ = frog_ = hint_ = nullptr;
        for (int i = 0; i < kCars; i++) cars_[i] = nullptr;
        for (int i = 0; i < kLogs; i++) logs_[i] = nullptr;
        for (int i = 0; i < 5; i++) home_[i] = nullptr;
    }

    void checkpoint_progress() override
    {
        if (score_ <= best_) return;
        best_ = score_;
        hal_config_set_int("hopper", "best", best_);
    }

    void on_button(ButtonEvent ev) override
    {
        if (ev.kind != ButtonEventKind::Pressed) return;
        if (over_) {
            reset_run();
            return;
        }
        switch (ev.button) {
            case Button::Up: if (row_ > 0) row_--; break;
            case Button::Down: if (row_ < kRows - 1) row_++; break;
            case Button::Left: if (col_ > 0) col_--; break;
            case Button::Right: if (col_ < kCols - 1) col_++; break;
            default: return;
        }
        fx_ = (float)(kBx + col_ * kCellW + 2);
        if (row_ == 0) reach_top();
        else draw();
    }

    void on_tick() override
    {
        if (over_) return;
        float mult = 1.0f + 0.12f * (float)level_;
        // Cars.
        for (int i = 0; i < kCars; i++) {
            Car &c = car_[i];
            c.x += c.dir * c.speed * mult;
            if (c.dir > 0 && c.x > kBx + kBoardW + 10) c.x = kBx - 40;
            if (c.dir < 0 && c.x < kBx - 40) c.x = kBx + kBoardW + 10;
            if (cars_[i]) lv_obj_set_pos(cars_[i], (int)c.x, lane_y(c.lane) + 3);
        }
        // Logs (+ carry the frog).
        bool on_log = false;
        for (int i = 0; i < kLogs; i++) {
            Log &l = log_[i];
            l.x += l.dir * l.speed * mult;
            int w = l.big ? 64 : 40;
            if (l.dir > 0 && l.x > kBx + kBoardW + 10) l.x = kBx - w - 10;
            if (l.dir < 0 && l.x < kBx - w - 10) l.x = kBx + kBoardW + 10;
            if (logs_[i]) {
                lv_image_set_src(logs_[i], l.big ? &spr_log_long : &spr_log_short);
                lv_obj_set_pos(logs_[i], (int)l.x, lane_y(l.lane) + 3);
            }
            if (row_ == l.lane && frog_cx() + 10 > l.x && frog_cx() + 10 < l.x + w) {
                on_log = true;
                fx_ += l.dir * l.speed * mult;
                col_ = (int)((fx_ - kBx) / kCellW);
                if (col_ < 0) col_ = 0;
                if (col_ > kCols - 1) col_ = kCols - 1;
            }
        }
        if (is_river(row_) && !on_log) {
            die();
            return;
        }
        if ((fx_ < kBx - 18 || fx_ > kBx + kBoardW - 2) && is_river(row_)) {
            die(); // carried offscreen
            return;
        }
        // Car hits.
        if (is_road(row_)) {
            float fxx = fx_ + 1, fyy = lane_y(row_) + 5, fw = 18, fh = 11;
            for (int i = 0; i < kCars; i++) {
                Car &c = car_[i];
                if (c.lane != row_) continue;
                if (fxx < c.x + 28 && fxx + fw > c.x + 2 && fyy < lane_y(row_) + 15 &&
                    fyy + fh > lane_y(row_) + 5) {
                    die();
                    return;
                }
            }
        }
        draw();
    }

private:
    static constexpr int kCols = 13;
    static constexpr int kRows = 8;
    static constexpr int kCellW = 24;
    static constexpr int kCellH = 22;
    static constexpr int kBoardW = 312;
    static constexpr int kBx = 4;
    static constexpr int kBy = 34;
    static constexpr int kCars = 9;
    static constexpr int kLogs = 4;

    static bool is_road(int r) { return r >= 4 && r <= 6; }
    static bool is_river(int r) { return r == 1 || r == 2; }
    int lane_y(int r) const { return kBy + r * kCellH; }
    float frog_cx() const { return fx_; }

    struct Car {
        float x;
        float speed;
        int dir;
        int lane;
        uint32_t color;
    };
    struct Log {
        float x;
        float speed;
        int dir;
        int lane;
        bool big;
    };

    void reset_run()
    {
        score_ = 0;
        lives_ = 3;
        level_ = 0;
        reaches_ = 0;
        over_ = false;
        for (int i = 0; i < 5; i++) {
            homes_full_[i] = false;
            if (home_[i]) {
                lv_obj_set_style_bg_color(home_[i], lv_color_hex(0xffc63a), 0);
            }
        }
        // 3 cars per road lane, alternating directions.
        int ci = 0;
        for (int lane = 4; lane <= 6; lane++) {
            int dir = (lane % 2 == 0) ? 1 : -1;
            float speed = 1.2f + 0.4f * (float)(lane - 4);
            uint32_t cols[3] = { 0xff5252, 0xffc63a, 0xb087b1 };
            for (int k = 0; k < 3; k++) {
                car_[ci].lane = lane;
                car_[ci].dir = dir;
                car_[ci].speed = speed;
                car_[ci].x = (float)(kBx + k * 115 + (lane - 4) * 37);
                car_[ci].color = cols[(lane + k) % 3];
                if (cars_[ci]) {
                    lv_obj_set_style_image_recolor(cars_[ci], lv_color_hex(car_[ci].color), 0);
                    lv_obj_set_style_image_recolor_opa(cars_[ci], LV_OPA_70, 0);
                }
                ci++;
            }
        }
        // 2 logs per river lane, opposite directions.
        log_[0] = { (float)kBx + 20, 1.0f, -1, 1, true };
        log_[1] = { (float)kBx + 200, 1.0f, -1, 1, false };
        log_[2] = { (float)kBx + 60, 1.3f, 1, 2, false };
        log_[3] = { (float)kBx + 230, 1.3f, 1, 2, true };
        respawn();
        hal_lights_clear();
        hal_lights_show();
        draw();
    }

    void respawn()
    {
        col_ = 6;
        row_ = kRows - 1;
        fx_ = (float)(kBx + col_ * kCellW + 2);
        draw();
    }

    void reach_top()
    {
        int slot = (int)(fx_ / 62.4f);
        if (slot < 0) slot = 0;
        if (slot > 4) slot = 4;
        score_ += 100 + level_ * 25;
        reaches_++;
        if (!homes_full_[slot]) {
            homes_full_[slot] = true;
            score_ += 150;
            if (home_[slot]) {
                lv_obj_set_style_bg_color(home_[slot], lv_color_hex(0x53d769), 0);
            }
            bool all = true;
            for (int i = 0; i < 5; i++) all = all && homes_full_[i];
            if (all) {
                level_++;
                if (level_ > hal_config_get_int("hopper", "levels_completed", 0))
                    hal_config_set_int("hopper", "levels_completed", level_);
                score_ += 500;
                for (int i = 0; i < 5; i++) {
                    homes_full_[i] = false;
                    if (home_[i]) {
                        lv_obj_set_style_bg_color(home_[i], lv_color_hex(0xffc63a), 0);
                    }
                }
            }
        }
        green_flash();
        respawn();
    }

    void die()
    {
        lives_--;
        for (size_t i = 0; i < LED_COUNT; i++) hal_lights_set(i, 255, 0, 0);
        hal_lights_show();
        if (lives_ <= 0) {
            over_ = true;
            checkpoint_progress();
        } else {
            respawn();
        }
        draw();
    }

    void green_flash()
    {
        for (size_t i = 0; i < LED_COUNT; i++) hal_lights_set(i, 0, 255, 0);
        hal_lights_show();
    }

    void draw()
    {
        if (frog_) {
            if (over_) lv_obj_add_flag(frog_, LV_OBJ_FLAG_HIDDEN);
            else {
                lv_obj_clear_flag(frog_, LV_OBJ_FLAG_HIDDEN);
                lv_obj_set_pos(frog_, (int)fx_, lane_y(row_) + 3);
            }
        }
        if (hud_) {
            if (over_) {
                lv_label_set_text_fmt(hud_, "GAME OVER %d  best %d  A: again", score_, best_);
            } else {
                lv_label_set_text_fmt(hud_, "%d pts  Lv %d  lives %d  best %d", score_,
                                      level_ + 1, lives_, best_);
            }
        }
    }

    lv_obj_t *hud_ = nullptr;
    lv_obj_t *hint_ = nullptr;
    lv_obj_t *frog_ = nullptr;
    lv_obj_t *cars_[kCars] = {};
    lv_obj_t *logs_[kLogs] = {};
    lv_obj_t *home_[5] = {};
    Car car_[kCars] = {};
    Log log_[kLogs] = {};
    bool homes_full_[5] = {};
    int col_ = 6, row_ = 7;
    float fx_ = 0.0f;
    int score_ = 0, lives_ = 3, level_ = 0, reaches_ = 0, best_ = 0;
    bool over_ = false;
};

static HopperApp s_instance;
App *app_hopper_instance() { return &s_instance; }
