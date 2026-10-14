#include "app.h"
#include "app_registry.h"

namespace {

constexpr const char *kScheduleUrl = "https://my.hackthenorth.com/schedule";

extern "C" const lv_image_dsc_t icon_schedule;

class ScheduleApp final : public App {
public:
    const char *name() const override { return "Schedule"; }
    const char *icon() const override { return "SCH"; }
    const lv_image_dsc_t *icon_image() const override { return &icon_schedule; }
    const char *slug() const override { return "schedule"; }
    AppAccess access() const override { return AppAccess::Default; }

    void on_enter(lv_obj_t *root) override
    {
        lv_obj_set_style_bg_color(root, lv_color_hex(BadgeTheme::BACKGROUND), 0);
        lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);

        lv_obj_t *title = lv_label_create(root);
        lv_label_set_text(title, "Schedule");
        lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(title, lv_color_hex(BadgeTheme::TEXT), 0);
        lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 4);

        lv_obj_t *description = lv_label_create(root);
        lv_label_set_text(description,
            "Check out the live event schedule on your hacker dashboard.");
        lv_obj_set_width(description, 296);
        lv_label_set_long_mode(description, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_align(description, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_font(description, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(description, lv_color_hex(BadgeTheme::TEXT_SOFT), 0);
        lv_obj_align(description, LV_ALIGN_TOP_MID, 0, 31);

        lv_obj_t *qr = lv_qrcode_create(root);
        lv_qrcode_set_size(qr, 120);
        lv_qrcode_set_dark_color(qr, lv_color_hex(0x000000));
        lv_qrcode_set_light_color(qr, lv_color_hex(0xffffff));
        lv_qrcode_set_data(qr, kScheduleUrl);
        lv_obj_align(qr, LV_ALIGN_TOP_MID, 0, 69);

        lv_obj_t *url = lv_label_create(root);
        lv_label_set_text(url, kScheduleUrl);
        lv_obj_set_width(url, 304);
        lv_label_set_long_mode(url, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_align(url, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_font(url, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(url, lv_color_hex(BadgeTheme::ACCENT_DETAIL), 0);
        lv_obj_align(url, LV_ALIGN_TOP_MID, 0, 193);

        lv_obj_t *hint = lv_label_create(root);
        lv_label_set_text(hint, "HOME to return");
        lv_obj_set_style_text_font(hint, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(hint, lv_color_hex(BadgeTheme::TEXT_DIM), 0);
        lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, 214);
    }

    // No on_button: HOME falls through to the registry and returns to the
    // launcher. B does nothing here.
};

ScheduleApp s_instance;

} // namespace

App *app_schedule_instance() { return &s_instance; }
