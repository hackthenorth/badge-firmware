#include "app.h"
#include "app_registry.h"
#include "hal_accel.h"

#include <cmath>
#include <cstdio>

extern "C" const lv_image_dsc_t icon_level;

namespace {

void format_angle(char *buffer, size_t size, float degrees)
{
    int tenths = static_cast<int>(lroundf(degrees * 10.0f));
    char sign = tenths < 0 ? '-' : '+';
    int magnitude = tenths < 0 ? -tenths : tenths;
    snprintf(buffer, size, "%c%d.%d°", sign, magnitude / 10, magnitude % 10);
}

} // namespace

class LevelApp : public App {
public:
    const char *name() const override { return "Level"; }
    const char *icon() const override { return "LVL"; }
    const char *slug() const override { return "level"; }
    const lv_image_dsc_t *icon_image() const override { return &icon_level; }

    bool icon_needs_outline() const override { return true; }

    void on_enter(lv_obj_t *root) override
    {
        lv_obj_t *title = lv_label_create(root);
        lv_label_set_text(title, "Level");
        lv_obj_set_style_text_color(title, lv_color_hex(BadgeTheme::TEXT), 0);
        lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
        lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 6);

        // Crosshair box at center.
        constexpr int kBoxSize = 160;
        ring_ = lv_obj_create(root);
        lv_obj_remove_style_all(ring_);
        lv_obj_set_size(ring_, kBoxSize, kBoxSize);
        lv_obj_align(ring_, LV_ALIGN_CENTER, 0, 0);
        lv_obj_set_style_radius(ring_, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(ring_, lv_color_hex(BadgeTheme::SURFACE), 0);
        lv_obj_set_style_bg_opa(ring_, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(ring_, 2, 0);
        lv_obj_set_style_border_color(ring_, lv_color_hex(BadgeTheme::BORDER), 0);
        lv_obj_clear_flag(ring_, LV_OBJ_FLAG_SCROLLABLE);

        // Inner "flat" target.
        lv_obj_t *target = lv_obj_create(ring_);
        lv_obj_remove_style_all(target);
        lv_obj_set_size(target, 32, 32);
        lv_obj_align(target, LV_ALIGN_CENTER, 0, 0);
        lv_obj_set_style_radius(target, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(target, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(target, 1, 0);
        lv_obj_set_style_border_color(target, lv_color_hex(BadgeTheme::ACCENT_DETAIL), 0);

        bubble_ = lv_obj_create(ring_);
        lv_obj_remove_style_all(bubble_);
        lv_obj_set_size(bubble_, 24, 24);
        lv_obj_align(bubble_, LV_ALIGN_CENTER, 0, 0);
        lv_obj_set_style_radius(bubble_, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(bubble_, lv_color_hex(BadgeTheme::ACCENT_DETAIL), 0);
        lv_obj_set_style_bg_opa(bubble_, LV_OPA_COVER, 0);

        readout_ = lv_label_create(root);
        lv_obj_set_style_text_color(readout_, lv_color_hex(BadgeTheme::TEXT_MUTED), 0);
        lv_obj_set_style_text_font(readout_, &lv_font_montserrat_16, 0);
        lv_obj_align(readout_, LV_ALIGN_BOTTOM_MID, 0, -10);
        lv_label_set_text(readout_, "Pitch +0.0°  Roll +0.0°");

        box_size_ = kBoxSize;
    }

    void on_exit() override
    {
        ring_ = bubble_ = readout_ = nullptr;
    }

    void on_tick() override
    {
        AccelSample s;
        if (hal_accel_read(&s) != ESP_OK) return;

        // Clamp displayed tilt to ±500 mg → edge of ring.
        float xr = fmaxf(-500.0f, fminf(500.0f, s.x_mg));
        float yr = fmaxf(-500.0f, fminf(500.0f, s.y_mg));
        int maxoff = box_size_ / 2 - 16;
        int dx = (int)(xr / 500.0f * maxoff);
        int dy = (int)(-yr / 500.0f * maxoff); // flip Y so tilting "up" moves bubble up

        if (bubble_) {
            lv_obj_align(bubble_, LV_ALIGN_CENTER, dx, dy);
            bool flat = (fabsf(s.x_mg) < 100.0f) && (fabsf(s.y_mg) < 100.0f);
            lv_obj_set_style_bg_color(bubble_,
                lv_color_hex(flat ? 0x00ffa8 : BadgeTheme::ACCENT_DETAIL), 0);
        }

        if (readout_) {
            float pitch = asinf(fmaxf(-1.0f, fminf(1.0f, s.y_mg / 1000.0f))) * 57.2957795f;
            float roll  = asinf(fmaxf(-1.0f, fminf(1.0f, s.x_mg / 1000.0f))) * 57.2957795f;
            char pitch_text[16];
            char roll_text[16];
            char readout[48];
            format_angle(pitch_text, sizeof(pitch_text), pitch);
            format_angle(roll_text, sizeof(roll_text), roll);
            snprintf(readout, sizeof(readout), "Pitch %s  Roll %s", pitch_text, roll_text);
            lv_label_set_text(readout_, readout);
        }
    }

private:
    lv_obj_t *ring_ = nullptr;
    lv_obj_t *bubble_ = nullptr;
    lv_obj_t *readout_ = nullptr;
    int box_size_ = 0;
};

static LevelApp s_instance;
App *app_level_instance() { return &s_instance; }
