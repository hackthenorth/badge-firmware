#include "app.h"
#include "hal_config.h"
#include "hal_lights.h"

#include "esp_random.h"

#include <cstdio>

// Invaders: hold Left/Right to slide the cannon, A to fire. Three rows
// Martian down the screen and speed up as they thin out; their bolts and
// your bolts chew through the three shields. Waves, lives, best score.

extern "C" const lv_image_dsc_t icon_invaders;
extern "C" const lv_image_dsc_t spr_inv1;
extern "C" const lv_image_dsc_t spr_inv2;
extern "C" const lv_image_dsc_t spr_ship;

class InvadersApp : public GameApp {
public:
    const char *name() const override { return "Invaders"; }
    const char *icon() const override { return "IV"; }
    const lv_image_dsc_t *icon_image() const override { return &icon_invaders; }
    const char *slug() const override { return "invaders"; }

    bool wants_wake_lock() const override { return true; }

    void on_enter(lv_obj_t *root) override
    {
        best_ = hal_config_get_int("invaders", "best", 0);

        hud_ = lv_label_create(root);
        lv_obj_set_style_text_color(hud_, lv_color_hex(BadgeTheme::TEXT), 0);
        lv_obj_set_style_text_font(hud_, &lv_font_montserrat_14, 0);
        lv_obj_set_width(hud_, 312);
        lv_obj_set_style_text_align(hud_, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_long_mode(hud_, LV_LABEL_LONG_WRAP);
        lv_obj_align(hud_, LV_ALIGN_TOP_MID, 0, 4);

        ship_ = lv_image_create(root);
        lv_image_set_src(ship_, &spr_ship);
        lv_obj_clear_flag(ship_, LV_OBJ_FLAG_SCROLLABLE);

        for (int r = 0; r < kRows; r++) {
            for (int c = 0; c < kCols; c++) {
                int i = r * kCols + c;
                inv_[i] = lv_image_create(root);
                lv_image_set_src(inv_[i], r == 0 ? &spr_inv2 : &spr_inv1);
                lv_obj_clear_flag(inv_[i], LV_OBJ_FLAG_SCROLLABLE);
            }
        }
        for (int i = 0; i < kPBolts; i++) {
            pbolt_[i] = lv_obj_create(root);
            style_bolt(pbolt_[i], BadgeTheme::TEXT);
            lv_obj_add_flag(pbolt_[i], LV_OBJ_FLAG_HIDDEN);
        }
        for (int i = 0; i < kEBolts; i++) {
            ebolt_[i] = lv_obj_create(root);
            style_bolt(ebolt_[i], 0xff5c8a);
            lv_obj_add_flag(ebolt_[i], LV_OBJ_FLAG_HIDDEN);
        }
        for (int s = 0; s < 3; s++) {
            for (int b = 0; b < kShieldBlocks; b++) {
                int i = s * kShieldBlocks + b;
                shield_[i] = lv_obj_create(root);
                lv_obj_remove_style_all(shield_[i]);
                lv_obj_set_size(shield_[i], kBlock, 8);
                lv_obj_set_style_bg_color(shield_[i], lv_color_hex(0x53d769), 0);
                lv_obj_set_style_bg_opa(shield_[i], LV_OPA_COVER, 0);
                lv_obj_set_style_radius(shield_[i], 1, 0);
                lv_obj_clear_flag(shield_[i], LV_OBJ_FLAG_SCROLLABLE);
            }
        }

        reset_game();
    }

    void on_exit() override
    {
        hal_lights_clear();
        hal_lights_show();
        hud_ = ship_ = nullptr;
        for (int i = 0; i < kRows * kCols; i++) inv_[i] = nullptr;
        for (int i = 0; i < kPBolts; i++) pbolt_[i] = nullptr;
        for (int i = 0; i < kEBolts; i++) ebolt_[i] = nullptr;
        for (int i = 0; i < 3 * kShieldBlocks; i++) shield_[i] = nullptr;
        left_ = right_ = false;
    }

    void checkpoint_progress() override
    {
        if (score_ <= best_) return;
        best_ = score_;
        hal_config_set_int("invaders", "best", best_);
    }

    void on_button(ButtonEvent ev) override
    {
        if (ev.kind == ButtonEventKind::Pressed) {
            if (ev.button == Button::A) {
                if (over_) reset_game();
                else fire_cool_ = 0; // allow immediate shot
                fire_ = true;
            }
            if (ev.button == Button::Left) left_ = true;
            if (ev.button == Button::Right) right_ = true;
        } else {
            if (ev.button == Button::A) fire_ = false;
            if (ev.button == Button::Left) left_ = false;
            if (ev.button == Button::Right) right_ = false;
        }
    }

    void on_tick() override
    {
        if (over_) return;
        // Ship.
        if (left_ && !right_) shipx_ -= 3.4f;
        else if (right_ && !left_) shipx_ += 3.4f;
        if (shipx_ < 8.0f) shipx_ = 8.0f;
        if (shipx_ > 288.0f) shipx_ = 288.0f;

        // Player fire (hold A, capped rate + pool).
        fire_cool_--;
        if (fire_ && fire_cool_ <= 0) {
            for (int i = 0; i < kPBolts; i++) {
                if (!pby_[i].on) {
                    pby_[i].on = true;
                    pby_[i].x = shipx_ + 10.5f;
                    pby_[i].y = kShipY - 10.0f;
                    fire_cool_ = 12;
                    break;
                }
            }
        }

        // Swarm: march, drop on edges, accelerate as it thins.
        int alive = 0;
        for (int i = 0; i < kRows * kCols; i++) alive += alive_[i] ? 1 : 0;
        float spd = (base_spd_ + (kRows * kCols - alive) * 0.35f);
        swarmx_ += swarmdir_ * spd;
        bool edge = false;
        for (int i = 0; i < kRows * kCols; i++) {
            if (!alive_[i]) continue;
            float x = swarmx_ + (i % kCols) * kDx;
            if (x < 6.0f || x > 296.0f) {
                edge = true;
                break;
            }
        }
        if (edge) {
            swarmdir_ = -swarmdir_;
            swarmy_ += 10.0f;
            if (swarmy_ > 150.0f) {
                game_over();
                return;
            }
        }

        // Enemy fire from a random living column bottom.
        efire_in_--;
        if (efire_in_ <= 0) {
            enemy_fire();
            efire_in_ = efire_gap();
        }

        step_bolts();
        if (over_) return;
        if (alive == 0) {
            wave_++;
            score_ += 100;
            hal_lights_flash_success();
            start_wave();
        }
        draw();
    }

private:
    static constexpr int kRows = 3;
    static constexpr int kCols = 7;
    static constexpr float kDx = 34.0f;
    static constexpr float kDy = 26.0f;
    static constexpr float kShipY = 212.0f;
    static constexpr int kPBolts = 4;
    static constexpr int kEBolts = 6;
    static constexpr int kShieldBlocks = 12; // 4x3 cells, 2 notched off
    static constexpr int kBlock = 12;

    struct Bolt {
        float x = 0.0f, y = 0.0f;
        bool on = false;
    };

    static void style_bolt(lv_obj_t *o, uint32_t color)
    {
        lv_obj_remove_style_all(o);
        lv_obj_set_size(o, 3, 9);
        lv_obj_set_style_bg_color(o, lv_color_hex(color), 0);
        lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(o, 1, 0);
        lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    }

    void reset_game()
    {
        score_ = 0;
        lives_ = 3;
        wave_ = 1;
        over_ = false;
        shipx_ = 148.0f;
        fire_ = left_ = right_ = false;
        fire_cool_ = 0;
        for (int i = 0; i < kPBolts; i++) pby_[i].on = false;
        for (int i = 0; i < kEBolts; i++) eby_[i].on = false;
        build_shields();
        start_wave();
        hal_lights_clear();
        hal_lights_show();
        draw();
    }

    void start_wave()
    {
        base_spd_ = 0.35f + 0.12f * (float)(wave_ - 1);
        swarmx_ = 30.0f;
        swarmy_ = 34.0f;
        swarmdir_ = 1;
        for (int i = 0; i < kRows * kCols; i++) alive_[i] = true;
        efire_in_ = 60;
        draw();
    }

    void build_shields()
    {
        // 4x3 block with the bottom-middle notch missing (classic shape).
        for (int s = 0; s < 3; s++) {
            int sx = 40 + s * 104;
            int i = 0;
            for (int r = 0; r < 3; r++) {
                for (int c = 0; c < 4; c++) {
                    int idx = s * kShieldBlocks + i;
                    if (r == 2 && (c == 1 || c == 2)) {
                        shield_on_[idx] = false;
                        if (shield_[idx]) lv_obj_add_flag(shield_[idx], LV_OBJ_FLAG_HIDDEN);
                    } else {
                        shield_on_[idx] = true;
                        if (shield_[idx]) {
                            lv_obj_clear_flag(shield_[idx], LV_OBJ_FLAG_HIDDEN);
                            lv_obj_set_pos(shield_[idx], sx + c * kBlock, 168 + r * 8);
                        }
                    }
                    i++;
                }
            }
        }
    }

    int efire_gap() const
    {
        int g = 55 - wave_ * 5;
        if (g < 22) g = 22;
        return g + (int)(esp_random() % 30);
    }

    void enemy_fire()
    {
        // Pick a random column, fire from its lowest living invader.
        for (int tries = 0; tries < 7; tries++) {
            int c = (int)(esp_random() % kCols);
            for (int r = kRows - 1; r >= 0; r--) {
                int i = r * kCols + c;
                if (!alive_[i]) continue;
                for (int b = 0; b < kEBolts; b++) {
                    if (!eby_[b].on) {
                        eby_[b].on = true;
                        eby_[b].x = swarmx_ + c * kDx + 7.5f;
                        eby_[b].y = swarmy_ + r * kDy + 14.0f;
                        return;
                    }
                }
                return;
            }
        }
    }

    static bool hit(float ax, float ay, float aw, float ah, float bx, float by, float bw,
                    float bh)
    {
        return ax < bx + bw && ax + aw > bx && ay < by + bh && ay + ah > by;
    }

    bool shield_hit(float x, float y, float w, float h)
    {
        for (int s = 0; s < 3; s++) {
            int sx = 40 + s * 104;
            for (int r = 0; r < 3; r++) {
                for (int c = 0; c < 4; c++) {
                    int idx = s * kShieldBlocks + r * 4 + c;
                    if (!shield_on_[idx]) continue;
                    if (hit(x, y, w, h, (float)(sx + c * kBlock), (float)(168 + r * 8),
                            (float)kBlock, 8.0f)) {
                        shield_on_[idx] = false;
                        if (shield_[idx]) lv_obj_add_flag(shield_[idx], LV_OBJ_FLAG_HIDDEN);
                        return true;
                    }
                }
            }
        }
        return false;
    }

    void step_bolts()
    {
        // Player bolts rise; kill invaders topmost-first per bolt.
        for (int i = 0; i < kPBolts; i++) {
            Bolt &b = pby_[i];
            if (!b.on) continue;
            b.y -= 5.0f;
            if (b.y < 20.0f) {
                b.on = false;
                continue;
            }
            if (shield_hit(b.x, b.y, 3.0f, 9.0f)) {
                b.on = false;
                continue;
            }
            bool killed = false;
            for (int r = 0; r < kRows && !killed; r++) {
                for (int c = 0; c < kCols && !killed; c++) {
                    int k = r * kCols + c;
                    if (!alive_[k]) continue;
                    float ix = swarmx_ + c * kDx, iy = swarmy_ + r * kDy;
                    if (hit(b.x, b.y, 3.0f, 9.0f, ix, iy, 18.0f, 14.0f)) {
                        alive_[k] = false;
                        b.on = false;
                        score_ += (r == 0) ? 30 : 10;
                        killed = true;
                    }
                }
            }
        }
        // Enemy bolts fall; shields then ship.
        for (int i = 0; i < kEBolts; i++) {
            Bolt &b = eby_[i];
            if (!b.on) continue;
            b.y += 3.0f + 0.3f * (float)wave_;
            if (b.y > 232.0f) {
                b.on = false;
                continue;
            }
            if (shield_hit(b.x, b.y, 3.0f, 9.0f)) {
                b.on = false;
                continue;
            }
            if (hit(b.x, b.y, 3.0f, 9.0f, shipx_, kShipY, 24.0f, 16.0f)) {
                b.on = false;
                lives_--;
                for (size_t k = 0; k < LED_COUNT; k++) hal_lights_set(k, 255, 0, 0);
                hal_lights_show();
                if (lives_ <= 0) {
                    game_over();
                    return;
                }
            }
        }
    }

    void game_over()
    {
        over_ = true;
        checkpoint_progress();
        draw();
    }

    void draw()
    {
        if (ship_) {
            if (over_) lv_obj_add_flag(ship_, LV_OBJ_FLAG_HIDDEN);
            else {
                lv_obj_clear_flag(ship_, LV_OBJ_FLAG_HIDDEN);
                lv_obj_set_pos(ship_, (int)shipx_, (int)kShipY);
            }
        }
        for (int r = 0; r < kRows; r++) {
            for (int c = 0; c < kCols; c++) {
                int i = r * kCols + c;
                if (!inv_[i]) continue;
                if (!alive_[i] || over_) lv_obj_add_flag(inv_[i], LV_OBJ_FLAG_HIDDEN);
                else {
                    lv_obj_clear_flag(inv_[i], LV_OBJ_FLAG_HIDDEN);
                    lv_obj_set_pos(inv_[i], (int)(swarmx_ + c * kDx), (int)(swarmy_ + r * kDy));
                }
            }
        }
        for (int i = 0; i < kPBolts; i++) {
            if (!pbolt_[i]) continue;
            if (!pby_[i].on || over_) lv_obj_add_flag(pbolt_[i], LV_OBJ_FLAG_HIDDEN);
            else {
                lv_obj_clear_flag(pbolt_[i], LV_OBJ_FLAG_HIDDEN);
                lv_obj_set_pos(pbolt_[i], (int)pby_[i].x, (int)pby_[i].y);
            }
        }
        for (int i = 0; i < kEBolts; i++) {
            if (!ebolt_[i]) continue;
            if (!eby_[i].on || over_) lv_obj_add_flag(ebolt_[i], LV_OBJ_FLAG_HIDDEN);
            else {
                lv_obj_clear_flag(ebolt_[i], LV_OBJ_FLAG_HIDDEN);
                lv_obj_set_pos(ebolt_[i], (int)eby_[i].x, (int)eby_[i].y);
            }
        }
        if (hud_) {
            if (over_) {
                lv_label_set_text_fmt(hud_, "GAME OVER wave %d score %d best %d  A: again",
                                      wave_, score_, best_);
            } else {
                lv_label_set_text_fmt(hud_, "wave %d  %d pts  lives %d  best %d", wave_,
                                      score_, lives_, best_);
            }
        }
    }

    lv_obj_t *hud_ = nullptr;
    lv_obj_t *ship_ = nullptr;
    lv_obj_t *inv_[kRows * kCols] = {};
    lv_obj_t *pbolt_[kPBolts] = {};
    lv_obj_t *ebolt_[kEBolts] = {};
    lv_obj_t *shield_[3 * kShieldBlocks] = {};
    bool alive_[kRows * kCols] = {};
    bool shield_on_[3 * kShieldBlocks] = {};
    Bolt pby_[kPBolts] = {};
    Bolt eby_[kEBolts] = {};
    float shipx_ = 148.0f, swarmx_ = 30.0f, swarmy_ = 34.0f, base_spd_ = 0.35f;
    int swarmdir_ = 1;
    int score_ = 0, lives_ = 3, wave_ = 1, best_ = 0;
    int fire_cool_ = 0, efire_in_ = 60;
    bool over_ = false, fire_ = false, left_ = false, right_ = false;
};

static InvadersApp s_instance;
App *app_invaders_instance() { return &s_instance; }
