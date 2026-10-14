#include "app.h"
#include "hal_gesture.h"
#include "hal_lights.h"

#include <cmath>
#include <cstdint>
#include <cstdio>

// Kaleidoscope. Tilt steers 8 mirrored arms; the pattern hue cycles
// and the LEDs mirror it. Shake reseeds the arm lengths.

extern "C" const lv_image_dsc_t icon_kaleido;

class KaleidoApp : public GameApp {
public:
    const char *name() const override { return "Kaleido"; }
    const char *icon() const override { return "KLD"; }
    const lv_image_dsc_t *icon_image() const override { return &icon_kaleido; }
    const char *slug() const override { return "kaleido"; }

    bool wants_wake_lock() const override { return true; }

    void on_enter(lv_obj_t *root) override
    {
        for (int a = 0; a < kArms; a++) {
            arms_[a] = lv_line_create(root);
            lv_obj_set_style_line_width(arms_[a], 2, 0);
            lv_obj_set_style_line_rounded(arms_[a], true, 0);
        }
        seed_ = 1;
        hue_ = 0;
        hint_ = lv_label_create(root);
        lv_obj_set_style_text_color(hint_, lv_color_hex(BadgeTheme::TEXT_MUTED), 0);
        lv_label_set_text(hint_, "tilt to steer  SHAKE: reseed");
        lv_obj_align(hint_, LV_ALIGN_BOTTOM_MID, 0, -8);
    }

    void on_exit() override
    {
        hal_lights_clear();
        hal_lights_show();
        for (int a = 0; a < kArms; a++) arms_[a] = nullptr;
        hint_ = nullptr;
    }

    void on_tick() override
    {
        if (gesture_poll_shake()) seed_++;
        AccelSample s = gesture_last_sample();
        float tilt = atan2f(s.y_mg, s.x_mg + 1.0f);
        hue_ = (hue_ + 1) % 360;

        uint8_t r, g, b;
        hue_to_rgb(hue_, r, g, b);
        for (int a = 0; a < kArms; a++) {
            if (!arms_[a]) continue;
            float base = tilt + (float)a * (2.0f * kPi / kArms);
            for (int i = 0; i < kPts; i++) {
                float t = (float)i / (kPts - 1);
                float wob = sinf(t * (3.0f + (seed_ % 5)) + hue_ * 0.05f);
                float rad = 12.0f + t * 90.0f + wob * 14.0f;
                // Mirror alternate arms for the kaleidoscope fold.
                float ang = (a % 2) ? base + wob * 0.6f : base - wob * 0.6f;
                pts_[a][i].x = (int16_t)(CX + rad * cosf(ang));
                pts_[a][i].y = (int16_t)(CY + rad * sinf(ang));
            }
            lv_line_set_points(arms_[a], pts_[a], kPts);
            lv_obj_set_style_line_color(arms_[a], lv_color_make(r, g, b), 0);
        }
        for (size_t i = 0; i < LED_COUNT; i++) hal_lights_set(i, r, g, b);
        hal_lights_show();
    }

private:
    static constexpr int kArms = 8;
    static constexpr int kPts = 20;
    static constexpr float kPi = 3.14159265f;
    static constexpr int CX = 160;
    static constexpr int CY = 112;

    static void hue_to_rgb(int h, uint8_t &r, uint8_t &g, uint8_t &b)
    {
        int s = h % 360;
        int x = (s % 60) * 255 / 60;
        if (s < 60) { r = 255; g = (uint8_t)x; b = 0; } else if (s < 120) { r = (uint8_t)(255 - x); g = 255; b = 0; } else if (s < 180) { r = 0; g = 255; b = (uint8_t)x; } else if (s < 240) { r = 0; g = (uint8_t)(255 - x); b = 255; } else if (s < 300) { r = (uint8_t)x; g = 0; b = 255; } else { r = 255; g = 0; b = (uint8_t)(255 - x); }
    }

    lv_obj_t *arms_[kArms] = { nullptr };
    lv_point_precise_t pts_[kArms][kPts];
    lv_obj_t *hint_ = nullptr;
    int seed_ = 1;
    int hue_ = 0;
};

static KaleidoApp s_instance;
App *app_kaleido_instance() { return &s_instance; }
