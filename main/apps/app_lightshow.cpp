#include "app.h"
#include "app_registry.h"
#include "hal_lights.h"

#include <cstdio>
#include <cmath>

static void hsv_to_rgb(float h, float s, float v,
                       uint8_t *r, uint8_t *g, uint8_t *b)
{
    float c = v * s;
    float hh = fmodf(h, 360.0f) / 60.0f;
    float x = c * (1.0f - fabsf(fmodf(hh, 2.0f) - 1.0f));
    float rf = 0, gf = 0, bf = 0;
    if (hh < 1)      { rf = c; gf = x; }
    else if (hh < 2) { rf = x; gf = c; }
    else if (hh < 3) { gf = c; bf = x; }
    else if (hh < 4) { gf = x; bf = c; }
    else if (hh < 5) { rf = x; bf = c; }
    else             { rf = c; bf = x; }
    float m = v - c;
    *r = (uint8_t)((rf + m) * 255);
    *g = (uint8_t)((gf + m) * 255);
    *b = (uint8_t)((bf + m) * 255);
}

extern "C" const lv_image_dsc_t icon_lightshow;

class LightShowApp : public App {
public:
    const char *name() const override { return "LightShow"; }
    const char *icon() const override { return "FX"; }
    const char *slug() const override { return "lightshow"; }
    const lv_image_dsc_t *icon_image() const override { return &icon_lightshow; }
    bool icon_needs_outline() const override { return true; }

    void on_enter(lv_obj_t *root) override
    {
        lv_obj_t *title = lv_label_create(root);
        lv_label_set_text(title, "Light Show");
        lv_obj_set_style_text_color(title, lv_color_hex(BadgeTheme::TEXT), 0);
        lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
        lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);

        mode_label_ = lv_label_create(root);
        lv_obj_set_style_text_color(mode_label_, lv_color_hex(BadgeTheme::ACCENT_DETAIL), 0);
        lv_obj_set_style_text_font(mode_label_, &lv_font_montserrat_18, 0);
        lv_obj_align(mode_label_, LV_ALIGN_CENTER, 0, -10);

        hint_ = lv_label_create(root);
        lv_obj_set_style_text_color(hint_, lv_color_hex(BadgeTheme::TEXT_MUTED), 0);
        lv_label_set_text(hint_, "A: next mode   Up/Down: speed");
        lv_obj_align(hint_, LV_ALIGN_BOTTOM_MID, 0, -14);

        phase_ = 0;
        update_label();
    }

    void on_exit() override
    {
        hal_lights_clear();
        hal_lights_show();
        mode_label_ = hint_ = nullptr;
    }

    void on_button(ButtonEvent ev) override
    {
        if (ev.kind != ButtonEventKind::Pressed) return;
        switch (ev.button) {
            case Button::A:
                mode_ = (mode_ + 1) % kModeCount;
                update_label();
                break;
            case Button::Up:
                if (speed_ < 16) speed_++;
                update_label();
                break;
            case Button::Down:
                if (speed_ > 1) speed_--;
                update_label();
                break;
            default: break;
        }
    }

    void on_tick() override
    {
        phase_ += speed_;
        switch (mode_) {
            case 0: rainbow(); break;
            case 1: chase();   break;
            case 2: breathe(); break;
            case 3: sparkle(); break;
        }
        hal_lights_show();
    }

private:
    static constexpr int kModeCount = 4;

    void rainbow()
    {
        for (int i = 0; i < (int)LED_COUNT; i++) {
            float h = fmodf(phase_ + i * 60.0f, 360.0f);
            uint8_t r, g, b;
            hsv_to_rgb(h, 1.0f, 0.35f, &r, &g, &b);
            hal_lights_set(i, r, g, b);
        }
    }

    void chase()
    {
        int head = ((int)(phase_ / 6)) % (int)LED_COUNT;
        for (int i = 0; i < (int)LED_COUNT; i++) {
            int d = (i - head + (int)LED_COUNT) % (int)LED_COUNT;
            uint8_t v = (d == 0) ? 120 : (d == 1) ? 40 : (d == 2) ? 10 : 0;
            hal_lights_set(i, 0, v, v); // cyan trail
        }
    }

    void breathe()
    {
        float a = (sinf(phase_ * 0.02f) + 1.0f) * 0.5f; // 0..1
        uint8_t r = (uint8_t)(a * 50);
        uint8_t g = (uint8_t)(a * 0);
        uint8_t b = (uint8_t)(a * 120);
        for (int i = 0; i < (int)LED_COUNT; i++) hal_lights_set(i, r, g, b);
    }

    void sparkle()
    {
        for (int i = 0; i < (int)LED_COUNT; i++) {
            uint32_t seed = (uint32_t)phase_ * 2654435761u + i * 0x9E3779B9u;
            uint8_t v = (seed >> 24) < 32 ? 140 : 0;
            hal_lights_set(i, v, v, v);
        }
    }

    void update_label()
    {
        if (!mode_label_) return;
        const char *names[] = {"Rainbow", "Chase", "Breathe", "Sparkle"};
        char buf[48];
        snprintf(buf, sizeof(buf), "%s  (speed %d)", names[mode_], speed_);
        lv_label_set_text(mode_label_, buf);
    }

    lv_obj_t *mode_label_ = nullptr;
    lv_obj_t *hint_ = nullptr;
    int mode_ = 0;
    int speed_ = 6;
    float phase_ = 0;
};

static LightShowApp s_instance;
App *app_lightshow_instance() { return &s_instance; }
