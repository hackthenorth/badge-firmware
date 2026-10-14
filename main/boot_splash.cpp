#include "boot_splash.h"

#include "hal_display.h"
#include "ui_theme.h"

#include <esp_app_desc.h>
#include "esp_timer.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"

extern "C" const lv_image_dsc_t boot_logo_htn;
extern "C" const lv_image_dsc_t boot_logo_solana;

namespace {

constexpr uint32_t kSplashDurationMs = 1000;
constexpr int64_t kSplashDurationUs = (int64_t)kSplashDurationMs * 1000;
constexpr uint32_t kBackground = BadgeTheme::BACKGROUND;
constexpr int32_t kHtnLogoY = 45;
constexpr int32_t kHtnLogoHeight = 52;
constexpr int32_t kSolanaLogoY = 151;
constexpr int32_t kCrossSize = 16;
constexpr int32_t kCrossY = ((kHtnLogoY + kHtnLogoHeight + kSolanaLogoY) / 2)
                          - (kCrossSize / 2);

lv_obj_t *s_splash = nullptr;
int64_t s_shown_at_us = 0;

constexpr lv_point_precise_t kCrossDown[] = { { 0, 0 }, { kCrossSize, kCrossSize } };
constexpr lv_point_precise_t kCrossUp[] = { { 0, kCrossSize }, { kCrossSize, 0 } };

void style_cross_stroke(lv_obj_t *stroke)
{
    lv_obj_set_style_line_width(stroke, 2, LV_PART_MAIN);
    lv_obj_set_style_line_color(stroke, lv_color_hex(0xffffff), LV_PART_MAIN);
    lv_obj_set_style_line_rounded(stroke, true, LV_PART_MAIN);
    lv_obj_align(stroke, LV_ALIGN_TOP_MID, 0, kCrossY);
}

void dismiss_splash(lv_timer_t *)
{
    if (s_splash) {
        lv_obj_delete(s_splash);
        s_splash = nullptr;
    }
}

} // namespace

void boot_splash_show()
{
    if (!lvgl_port_lock(0)) return;

    lv_obj_t *top = lv_layer_top();
    s_splash = lv_obj_create(top);
    lv_obj_remove_style_all(s_splash);
    lv_obj_set_size(s_splash, 320, 240);
    lv_obj_set_pos(s_splash, 0, 0);
    lv_obj_set_style_bg_color(s_splash, lv_color_hex(kBackground), 0);
    lv_obj_set_style_bg_opa(s_splash, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_splash, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *htn = lv_image_create(s_splash);
    lv_image_set_src(htn, &boot_logo_htn);
    lv_obj_align(htn, LV_ALIGN_TOP_MID, 0, kHtnLogoY);

    // A geometric multiplication mark rather than a font glyph: two equal,
    // perpendicular diagonal strokes make this read as a sideways cross.
    lv_obj_t *cross_down = lv_line_create(s_splash);
    lv_line_set_points(cross_down, kCrossDown, 2);
    style_cross_stroke(cross_down);

    lv_obj_t *cross_up = lv_line_create(s_splash);
    lv_line_set_points(cross_up, kCrossUp, 2);
    style_cross_stroke(cross_up);

    lv_obj_t *solana = lv_image_create(s_splash);
    lv_image_set_src(solana, &boot_logo_solana);
    lv_obj_align(solana, LV_ALIGN_TOP_MID, 0, kSolanaLogoY);

    // ESP-IDF stores this Git-derived project version in the image descriptor,
    // so the same identifier is available both on-screen and to host tooling.
    lv_obj_t *version = lv_label_create(s_splash);
    lv_obj_set_width(version, 310);
    lv_label_set_long_mode(version, LV_LABEL_LONG_DOT);
    lv_label_set_text_fmt(version, "FW %s", esp_app_get_description()->version);
    lv_obj_set_style_text_font(version, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(version, lv_color_hex(BadgeTheme::TEXT_DIM), 0);
    lv_obj_set_style_text_align(version, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(version, LV_ALIGN_BOTTOM_MID, 0, -5);

    lvgl_port_unlock();

    // The display HAL refreshes while the LCD is still off and sends DISPON
    // only after every splash stripe reaches the panel.
    hal_display_show();
    s_shown_at_us = esp_timer_get_time();
}

void boot_splash_finish()
{
    if (!lvgl_port_lock(0)) return;

    if (!s_splash) {
        lvgl_port_unlock();
        return;
    }

    const int64_t elapsed_us = esp_timer_get_time() - s_shown_at_us;
    const int64_t remaining_us = kSplashDurationUs - elapsed_us;
    if (remaining_us <= 0) {
        dismiss_splash(nullptr);
        lvgl_port_unlock();
        return;
    }

    const uint32_t remaining_ms = (uint32_t)((remaining_us + 999) / 1000);
    lv_timer_t *timer = lv_timer_create(dismiss_splash, remaining_ms, nullptr);
    if (timer) {
        lv_timer_set_repeat_count(timer, 1);
    } else {
        // Never strand a permanent top-layer card if the timer allocation
        // fails; revealing the boot destination immediately is safer.
        dismiss_splash(nullptr);
    }

    lvgl_port_unlock();
}
