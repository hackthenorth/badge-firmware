#include "app.h"

extern "C" const lv_image_dsc_t icon_rules;

namespace {

class RulesApp final : public App {
public:
    const char *name() const override { return "Info & Rules"; }
    const char *icon() const override { return "i"; }
    const lv_image_dsc_t *icon_image() const override { return &icon_rules; }
    const char *slug() const override { return "rules"; }
    AppAccess access() const override { return AppAccess::Default; }

    void on_enter(lv_obj_t *root) override
    {
        lv_obj_t *title = lv_label_create(root);
        lv_label_set_text(title, "Info & Rules");
        lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(title, lv_color_hex(BadgeTheme::TEXT), 0);
        lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 5);

        // Leave a white border beyond LVGL's built-in QR margin so phone
        // cameras can reliably distinguish the code from the dark screen.
        lv_obj_t *frame = lv_obj_create(root);
        lv_obj_remove_style_all(frame);
        lv_obj_set_size(frame, 156, 156);
        lv_obj_set_style_bg_color(frame, lv_color_hex(0xffffff), 0);
        lv_obj_set_style_bg_opa(frame, LV_OPA_COVER, 0);
        lv_obj_clear_flag(frame, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_align(frame, LV_ALIGN_TOP_MID, 0, 34);

        lv_obj_t *qr = lv_qrcode_create(frame);
        lv_qrcode_set_size(qr, 140);
        lv_qrcode_set_dark_color(qr, lv_color_hex(0x000000));
        lv_qrcode_set_light_color(qr, lv_color_hex(0xffffff));
        lv_qrcode_set_quiet_zone(qr, true);
        lv_qrcode_set_data(qr, "https://badge.hackthenorth.com/rules");
        lv_obj_center(qr);

        lv_obj_t *hint = lv_label_create(root);
        lv_label_set_text(hint, "Scan with your phone for badge rules.");
        lv_obj_set_width(hint, 296);
        lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_font(hint, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(hint, lv_color_hex(BadgeTheme::TEXT_SOFT), 0);
        lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, 198);

        lv_obj_t *footer = lv_label_create(root);
        lv_label_set_text(footer, "HOME  apps");
        lv_obj_set_style_text_font(footer, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(footer, lv_color_hex(BadgeTheme::TEXT_MUTED), 0);
        lv_obj_align(footer, LV_ALIGN_BOTTOM_MID, 0, -7);
    }
};

RulesApp s_instance;

} // namespace

App *app_rules_instance() { return &s_instance; }
