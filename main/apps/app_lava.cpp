#include "app.h"
#include "hal_lights.h"

#include <cmath>
#include <cstdint>

// Lava lamp. Six blobs drift on slow sine paths; the LEDs show the
// average blob color so the whole badge glows with the lamp.

extern "C" const lv_image_dsc_t icon_lava;

class LavaApp : public GameApp {
public:
    const char *name() const override { return "Lava"; }
    const char *icon() const override { return "LVA"; }
    const lv_image_dsc_t *icon_image() const override { return &icon_lava; }
    const char *slug() const override { return "lava"; }

    bool wants_wake_lock() const override { return true; }

    void on_enter(lv_obj_t *root) override
    {
        tick_ = 0;
        for (int i = 0; i < kBlobs; i++) {
            blobs_[i] = lv_obj_create(root);
            lv_obj_set_style_border_width(blobs_[i], 0, 0);
            lv_obj_set_style_radius(blobs_[i], LV_RADIUS_CIRCLE, 0);
            // Warm lava palette, one hue per blob.
            lv_obj_set_style_bg_color(blobs_[i], lv_color_hex(kColors[i]), 0);
        }
        hint_ = lv_label_create(root);
        lv_obj_set_style_text_color(hint_, lv_color_hex(BadgeTheme::TEXT_MUTED), 0);
        lv_label_set_text(hint_, "A: stir it up");
        lv_obj_align(hint_, LV_ALIGN_BOTTOM_MID, 0, -8);
    }

    void on_exit() override
    {
        hal_lights_clear();
        hal_lights_show();
        for (int i = 0; i < kBlobs; i++) blobs_[i] = nullptr;
        hint_ = nullptr;
    }

    void on_button(ButtonEvent ev) override
    {
        if (ev.kind == ButtonEventKind::Pressed && ev.button == Button::A) stir_ += 40;
    }

    void on_tick() override
    {
        tick_++;
        if (stir_ > 0) stir_--;
        float energy = 1.0f + (float)stir_ / 10.0f;
        int r_sum = 0, g_sum = 0, b_sum = 0;
        for (int i = 0; i < kBlobs; i++) {
            if (!blobs_[i]) continue;
            float t = (float)tick_ * 0.02f * energy + (float)i * 1.7f;
            int sz = 26 + (int)(10.0f * sinf(t * 0.7f + (float)i));
            int x = 160 + (int)(110.0f * sinf(t * (0.5f + 0.08f * i)));
            int y = 112 + (int)(70.0f * sinf(t * 0.8f + 1.0f));
            lv_obj_set_size(blobs_[i], sz, sz);
            lv_obj_set_pos(blobs_[i], x - sz / 2, y - sz / 2);
            r_sum += (kColors[i] >> 16) & 0xff;
            g_sum += (kColors[i] >> 8) & 0xff;
            b_sum += kColors[i] & 0xff;
        }
        uint8_t r = (uint8_t)(r_sum / kBlobs / 3);
        uint8_t g = (uint8_t)(g_sum / kBlobs / 3);
        uint8_t b = (uint8_t)(b_sum / kBlobs / 3);
        for (size_t i = 0; i < LED_COUNT; i++) hal_lights_set(i, r, g, b);
        hal_lights_show();
    }

private:
    static constexpr int kBlobs = 6;
    static constexpr uint32_t kColors[kBlobs] = {
        0xff4400, 0xff8800, 0xff0044, 0xcc2200, 0xff6600, 0xaa0033,
    };

    lv_obj_t *blobs_[kBlobs] = { nullptr };
    lv_obj_t *hint_ = nullptr;
    int tick_ = 0;
    int stir_ = 0;
};

static LavaApp s_instance;
App *app_lava_instance() { return &s_instance; }
