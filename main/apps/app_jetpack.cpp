#include "app.h"
#include "hal_config.h"
#include "hal_lights.h"

#include "esp_random.h"

#include <cmath>
#include <cstdio>

// Jetpack: hold A (or Up) to thrust, release to fall. Dodge the zappers,
// grab coins, outrun the ever-faster scroll. Distance + coins is the
// score; the best run is remembered.

extern "C" const lv_image_dsc_t icon_jetpack;
extern "C" const lv_image_dsc_t spr_jet;
extern "C" const lv_image_dsc_t spr_jet_fire;
extern "C" const lv_image_dsc_t spr_coin;
extern "C" const lv_image_dsc_t spr_zap;

class JetpackApp : public GameApp {
public:
    const char *name() const override { return "Jetpack"; }
    const char *icon() const override { return "JP"; }
    const lv_image_dsc_t *icon_image() const override { return &icon_jetpack; }
    const char *slug() const override { return "jetpack"; }

    bool wants_wake_lock() const override { return true; }

    void on_enter(lv_obj_t *root) override
    {
        best_ = hal_config_get_int("jetpack", "best", 0);

        hud_ = lv_label_create(root);
        lv_obj_set_style_text_color(hud_, lv_color_hex(BadgeTheme::TEXT), 0);
        lv_obj_set_style_text_font(hud_, &lv_font_montserrat_14, 0);
        lv_obj_set_width(hud_, 312);
        lv_obj_set_style_text_align(hud_, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_long_mode(hud_, LV_LABEL_LONG_WRAP);
        lv_obj_align(hud_, LV_ALIGN_TOP_MID, 0, 4);

        floor_ = lv_obj_create(root);
        lv_obj_remove_style_all(floor_);
        lv_obj_set_size(floor_, 320, 4);
        lv_obj_set_pos(floor_, 0, kFloor);
        lv_obj_set_style_bg_color(floor_, lv_color_hex(BadgeTheme::BORDER), 0);
        lv_obj_set_style_bg_opa(floor_, LV_OPA_COVER, 0);
        lv_obj_clear_flag(floor_, LV_OBJ_FLAG_SCROLLABLE);

        for (int i = 0; i < kStars; i++) {
            stars_[i] = lv_obj_create(root);
            lv_obj_remove_style_all(stars_[i]);
            lv_obj_set_size(stars_[i], 2, 2);
            lv_obj_set_style_bg_color(stars_[i], lv_color_hex(BadgeTheme::TEXT_DIM), 0);
            lv_obj_set_style_bg_opa(stars_[i], LV_OPA_COVER, 0);
            lv_obj_set_style_radius(stars_[i], LV_RADIUS_CIRCLE, 0);
            lv_obj_clear_flag(stars_[i], LV_OBJ_FLAG_SCROLLABLE);
            starx_[i] = (float)((i * 173) % 320);
            stary_[i] = (float)(28 + (i * 97) % 180);
        }

        player_ = lv_image_create(root);
        lv_image_set_src(player_, &spr_jet);
        lv_obj_clear_flag(player_, LV_OBJ_FLAG_SCROLLABLE);
        flame_ = lv_image_create(root);
        lv_image_set_src(flame_, &spr_jet_fire);
        lv_obj_clear_flag(flame_, LV_OBJ_FLAG_SCROLLABLE);

        for (int i = 0; i < kCoins; i++) {
            coins_[i] = lv_image_create(root);
            lv_image_set_src(coins_[i], &spr_coin);
            lv_obj_clear_flag(coins_[i], LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_add_flag(coins_[i], LV_OBJ_FLAG_HIDDEN);
        }
        for (int i = 0; i < kZaps; i++) {
            zaps_[i] = lv_image_create(root);
            lv_image_set_src(zaps_[i], &spr_zap);
            lv_obj_clear_flag(zaps_[i], LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_add_flag(zaps_[i], LV_OBJ_FLAG_HIDDEN);
        }

        reset_run();
    }

    void on_exit() override
    {
        hal_lights_clear();
        hal_lights_show();
        hud_ = floor_ = player_ = flame_ = nullptr;
        for (int i = 0; i < kStars; i++) stars_[i] = nullptr;
        for (int i = 0; i < kCoins; i++) coins_[i] = nullptr;
        for (int i = 0; i < kZaps; i++) zaps_[i] = nullptr;
        thrust_ = false;
    }

    void checkpoint_progress() override
    {
        if (score_ <= best_) return;
        best_ = score_;
        hal_config_set_int("jetpack", "best", best_);
    }

    void on_button(ButtonEvent ev) override
    {
        if (ev.kind == ButtonEventKind::Pressed) {
            if (ev.button == Button::A) {
                if (over_) reset_run();
                else thrust_ = true;
            }
            if (ev.button == Button::Up) {
                if (!over_) thrust_up_ = true;
            }
        } else {
            if (ev.button == Button::A) thrust_ = false;
            if (ev.button == Button::Up) thrust_up_ = false;
        }
    }

    void on_tick() override
    {
        if (over_) return;
        ticks_++;
        float speed = base_speed_ + 9.0f * (ticks_ / 50);
        if (speed > 420.0f) speed = 420.0f;

        // Player vertical.
        bool thrusting = thrust_ || thrust_up_;
        if (thrusting) vy_ -= 2600.0f * DT;
        vy_ += 1500.0f * DT;
        if (vy_ > 430.0f) vy_ = 430.0f;
        if (vy_ < -470.0f) vy_ = -470.0f;
        py_ += vy_ * DT;
        if (py_ < kCeil) { py_ = kCeil; vy_ = 0.0f; }
        if (py_ > kFloor - kPH) { py_ = kFloor - kPH; vy_ = 0.0f; }

        // Stars parallax.
        for (int i = 0; i < kStars; i++) {
            starx_[i] -= speed * 0.4f * DT;
            if (starx_[i] < -4) {
                starx_[i] = 324;
                stary_[i] = (float)(28 + (ticks_ * 37 + i * 53) % 180);
            }
            if (stars_[i]) lv_obj_set_pos(stars_[i], (int)starx_[i], (int)stary_[i]);
        }

        // Coins drift left; collect on touch.
        coin_in_ -= 1;
        if (coin_in_ <= 0) {
            spawn_coins();
            coin_in_ = 70 + (int)(esp_random() % 60);
        }
        for (int i = 0; i < kCoins; i++) {
            Coin &c = coin_[i];
            if (!c.on) continue;
            c.x -= speed * DT;
            if (c.x < -20) {
                c.on = false;
                if (coins_[i]) lv_obj_add_flag(coins_[i], LV_OBJ_FLAG_HIDDEN);
                continue;
            }
            if (overlap(kPx + 3, py_ + 3, 14, 20, c.x, c.y, 14, 14)) {
                c.on = false;
                coins_n_++;
                if (coins_[i]) lv_obj_add_flag(coins_[i], LV_OBJ_FLAG_HIDDEN);
                coin_flash_ = 8;
            } else if (coins_[i]) {
                lv_obj_set_pos(coins_[i], (int)c.x, (int)c.y);
            }
        }

        // Zappers drift left; touch kills.
        zap_in_ -= 1;
        if (zap_in_ <= 0) {
            spawn_zap(speed);
            zap_in_ = 80 + (int)(esp_random() % 70);
        }
        for (int i = 0; i < kZaps; i++) {
            Zap &z = zap_[i];
            if (!z.on) continue;
            z.x -= (speed + z.vx) * DT;
            if (z.x < -24) {
                z.on = false;
                if (zaps_[i]) lv_obj_add_flag(zaps_[i], LV_OBJ_FLAG_HIDDEN);
                continue;
            }
            if (zaps_[i]) lv_obj_set_pos(zaps_[i], (int)z.x, (int)z.y);
            if (overlap(kPx + 4, py_ + 4, 12, 18, z.x + 2, z.y, 10, 34)) {
                crash();
                return;
            }
        }

        score_ = (int)(dist_ / 50.0f) + coins_n_ * 25;
        dist_ += speed * DT;
        draw(thrusting, speed);
    }

private:
    static constexpr float DT = 0.02f;
    static constexpr int kPx = 64;
    static constexpr int kPH = 26;
    static constexpr float kCeil = 30.0f;
    static constexpr float kFloor = 218.0f;
    static constexpr int kStars = 20;
    static constexpr int kCoins = 10;
    static constexpr int kZaps = 5;

    struct Coin {
        float x, y;
        bool on = false;
    };
    struct Zap {
        float x, y, vx;
        bool on = false;
    };

    static bool overlap(float ax, float ay, float aw, float ah, float bx, float by, float bw,
                        float bh)
    {
        return ax < bx + bw && ax + aw > bx && ay < by + bh && ay + ah > by;
    }

    void reset_run()
    {
        py_ = 120.0f;
        vy_ = 0.0f;
        dist_ = 0.0f;
        score_ = 0;
        coins_n_ = 0;
        ticks_ = 0;
        base_speed_ = 190.0f;
        coin_in_ = 40;
        zap_in_ = 120;
        coin_flash_ = 0;
        over_ = false;
        thrust_ = thrust_up_ = false;
        for (int i = 0; i < kCoins; i++) {
            coin_[i].on = false;
            if (coins_[i]) lv_obj_add_flag(coins_[i], LV_OBJ_FLAG_HIDDEN);
        }
        for (int i = 0; i < kZaps; i++) {
            zap_[i].on = false;
            if (zaps_[i]) lv_obj_add_flag(zaps_[i], LV_OBJ_FLAG_HIDDEN);
        }
        hal_lights_clear();
        hal_lights_show();
        draw(false, base_speed_);
    }

    void spawn_coins()
    {
        float y = 50.0f + (float)(esp_random() % 130);
        int n = 0;
        for (int i = 0; i < kCoins && n < 5; i++) {
            if (coin_[i].on) continue;
            coin_[i].on = true;
            coin_[i].x = 330.0f + n * 20.0f;
            coin_[i].y = y + 10.0f * sinf(n * 1.2f);
            if (coins_[i]) {
                lv_obj_clear_flag(coins_[i], LV_OBJ_FLAG_HIDDEN);
                lv_obj_set_pos(coins_[i], (int)coin_[i].x, (int)coin_[i].y);
            }
            n++;
        }
    }

    void spawn_zap(float speed)
    {
        (void)speed;
        for (int i = 0; i < kZaps; i++) {
            if (zap_[i].on) continue;
            zap_[i].on = true;
            zap_[i].x = 330.0f;
            zap_[i].y = 44.0f + (float)(esp_random() % 130);
            zap_[i].vx = 0.0f;
            // Sometimes a pair with a gap to thread.
            if ((esp_random() % 3) == 0) {
                for (int j = 0; j < kZaps; j++) {
                    if (zap_[j].on) continue;
                    zap_[j].on = true;
                    zap_[j].x = 330.0f;
                    zap_[j].vx = 0.0f;
                    float pair_y = (zap_[i].y > 120.0f) ? zap_[i].y - 110.0f
                                                       : zap_[i].y + 110.0f;
                    if (pair_y < 40.0f) pair_y = 40.0f;
                    if (pair_y > 180.0f) pair_y = 180.0f;
                    zap_[j].y = pair_y;
                    if (zaps_[j]) lv_obj_clear_flag(zaps_[j], LV_OBJ_FLAG_HIDDEN);
                    break;
                }
            }
            if (zaps_[i]) lv_obj_clear_flag(zaps_[i], LV_OBJ_FLAG_HIDDEN);
            return;
        }
    }

    void crash()
    {
        over_ = true;
        checkpoint_progress();
        for (size_t i = 0; i < LED_COUNT; i++) hal_lights_set(i, 255, 0, 0);
        hal_lights_show();
        draw(false, 0.0f);
    }

    void draw(bool thrusting, float speed)
    {
        if (player_) lv_obj_set_pos(player_, kPx, (int)py_);
        if (flame_) {
            if (thrusting && !over_) {
                lv_obj_clear_flag(flame_, LV_OBJ_FLAG_HIDDEN);
                lv_obj_set_pos(flame_, kPx + 4, (int)(py_ + kPH));
            } else {
                lv_obj_add_flag(flame_, LV_OBJ_FLAG_HIDDEN);
            }
        }
        if (hud_) {
            if (over_) {
                lv_label_set_text_fmt(hud_, "WRECKED %d  best %d  A: again", score_, best_);
            } else {
                lv_label_set_text_fmt(hud_, "%d pts  %d coins  best %d", score_, coins_n_,
                                      best_);
            }
        }
        if (coin_flash_ > 0) {
            coin_flash_--;
            for (size_t i = 0; i < LED_COUNT; i++) hal_lights_set(i, 200, 160, 0);
        } else if (!over_) {
            uint8_t v = (uint8_t)(60.0f + speed * 0.5f);
            if (v > 230) v = 230;
            for (size_t i = 0; i < LED_COUNT; i++) {
                hal_lights_set(i, (uint8_t)(v * 0x66 / 255), (uint8_t)(v * 0x41 / 255),
                               (uint8_t)(v * 0x67 / 255));
            }
        }
        hal_lights_show();
    }

    lv_obj_t *hud_ = nullptr;
    lv_obj_t *floor_ = nullptr;
    lv_obj_t *player_ = nullptr;
    lv_obj_t *flame_ = nullptr;
    lv_obj_t *stars_[kStars] = {};
    lv_obj_t *coins_[kCoins] = {};
    lv_obj_t *zaps_[kZaps] = {};
    Coin coin_[kCoins] = {};
    Zap zap_[kZaps] = {};
    float starx_[kStars] = {};
    float stary_[kStars] = {};
    float py_ = 120.0f, vy_ = 0.0f, dist_ = 0.0f, base_speed_ = 190.0f;
    int score_ = 0, coins_n_ = 0, ticks_ = 0, best_ = 0;
    int coin_in_ = 40, zap_in_ = 120, coin_flash_ = 0;
    bool over_ = false, thrust_ = false, thrust_up_ = false;
};

static JetpackApp s_instance;
App *app_jetpack_instance() { return &s_instance; }
