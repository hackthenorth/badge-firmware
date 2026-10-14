#include "app.h"
#include "app_registry.h"
#include "hal_lights.h"

#include <cstdio>

struct Color { const char *name; uint8_t r, g, b; };

static const Color kPalette[] = {
    {"off",   0,   0,   0 },
    {"red",   80,  0,   0 },
    {"green", 0,   80,  0 },
    {"blue",  0,   0,   80},
    {"white", 60,  60,  60},
};
static constexpr int kPaletteCount = sizeof(kPalette) / sizeof(kPalette[0]);

extern "C" const lv_image_dsc_t icon_lights;

class LightsApp : public App {
public:
    const char *name() const override { return "Lights"; }
    const char *icon() const override { return "LED"; }
    const char *slug() const override { return "lights"; }
    const lv_image_dsc_t *icon_image() const override { return &icon_lights; }
    bool icon_needs_outline() const override { return true; }

    void on_enter(lv_obj_t *root) override
    {
        lv_obj_t *title = lv_label_create(root);
        lv_label_set_text(title, "Lights");
        lv_obj_set_style_text_color(title, lv_color_white(), 0);
        lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
        lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);

        for (int i = 0; i < (int)LED_COUNT; i++) {
            thumbs_[i] = lv_obj_create(root);
            lv_obj_remove_style_all(thumbs_[i]);
            lv_obj_set_size(thumbs_[i], 32, 32);
            lv_obj_set_pos(thumbs_[i], 24 + i * 45, 80);
            lv_obj_set_style_radius(thumbs_[i], 16, 0);
            lv_obj_set_style_bg_color(thumbs_[i], lv_color_hex(0x202020), 0);
            lv_obj_set_style_bg_opa(thumbs_[i], LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(thumbs_[i], 2, 0);
            lv_obj_set_style_border_color(thumbs_[i], lv_color_hex(0x404040), 0);
        }

        idx_label_ = lv_label_create(root);
        lv_obj_set_style_text_color(idx_label_, lv_color_white(), 0);
        lv_obj_set_pos(idx_label_, 24, 140);

        color_label_ = lv_label_create(root);
        lv_obj_set_style_text_color(color_label_, lv_color_white(), 0);
        lv_obj_set_pos(color_label_, 24, 170);

        hint_ = lv_label_create(root);
        lv_obj_set_style_text_color(hint_, lv_color_hex(0x808080), 0);
        lv_obj_set_pos(hint_, 24, 200);
        lv_label_set_text(hint_, "L/R: pixel   U/D: color   A: apply");

        update_ui();
    }

    void on_exit() override
    {
        for (auto &t : thumbs_) t = nullptr;
        idx_label_ = color_label_ = hint_ = nullptr;
    }

    void on_button(ButtonEvent ev) override
    {
        if (ev.kind != ButtonEventKind::Pressed) return;
        switch (ev.button) {
            case Button::Left:  if (pixel_ > 0) pixel_--; break;
            case Button::Right: if (pixel_ < (int)LED_COUNT - 1) pixel_++; break;
            case Button::Up:    color_ = (color_ + 1) % kPaletteCount; break;
            case Button::Down:  color_ = (color_ - 1 + kPaletteCount) % kPaletteCount; break;
            case Button::A: {
                const Color &c = kPalette[color_];
                hal_lights_set(pixel_, c.r, c.g, c.b);
                hal_lights_show();
                break;
            }
            default: return;
        }
        update_ui();
    }

private:
    void update_ui()
    {
        for (int i = 0; i < (int)LED_COUNT; i++) {
            if (!thumbs_[i]) continue;
            lv_obj_set_style_border_color(thumbs_[i],
                i == pixel_ ? lv_color_hex(0xFFD000) : lv_color_hex(0x404040), 0);
            lv_obj_set_style_border_width(thumbs_[i], i == pixel_ ? 3 : 2, 0);
        }
        if (idx_label_) {
            char buf[32];
            snprintf(buf, sizeof(buf), "Pixel: %d", pixel_);
            lv_label_set_text(idx_label_, buf);
        }
        if (color_label_) {
            char buf[32];
            snprintf(buf, sizeof(buf), "Color: %s", kPalette[color_].name);
            lv_label_set_text(color_label_, buf);
        }
    }

    lv_obj_t *thumbs_[LED_COUNT] = {};
    lv_obj_t *idx_label_ = nullptr;
    lv_obj_t *color_label_ = nullptr;
    lv_obj_t *hint_ = nullptr;
    int pixel_ = 0;
    int color_ = 0;
};

static LightsApp s_instance;
App *app_lights_instance() { return &s_instance; }
