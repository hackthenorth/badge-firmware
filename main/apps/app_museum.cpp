#include "app.h"

namespace {

constexpr const char *kMuseumUrl = "https://museum.hackthenorth.com";

extern "C" const lv_image_dsc_t icon_museum;

class MuseumApp final : public App {
public:
    const char *name() const override { return "Museum"; }
    const char *icon() const override { return "MUS"; }
    const lv_image_dsc_t *icon_image() const override { return &icon_museum; }
    const char *slug() const override { return "museum"; }
    AppAccess access() const override { return AppAccess::Default; }

    void on_enter(lv_obj_t *root) override
    {
        lv_obj_set_style_bg_color(root, lv_color_hex(BadgeTheme::BACKGROUND), 0);
        lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);

        lv_obj_t *title = lv_label_create(root);
        lv_label_set_text(title, "Museum");
        lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(title, lv_color_hex(BadgeTheme::TEXT), 0);
        lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);

        lv_obj_t *description = lv_label_create(root);
        lv_label_set_text(description,
            "Explore the Museum of Hack the North\n"
            "and projects from years past.");
        lv_obj_set_width(description, 300);
        lv_label_set_long_mode(description, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_align(description, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_font(description, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(description, lv_color_hex(BadgeTheme::TEXT_SOFT), 0);
        lv_obj_align(description, LV_ALIGN_TOP_MID, 0, 36);

        lv_obj_t *qr = lv_qrcode_create(root);
        lv_qrcode_set_size(qr, 112);
        lv_qrcode_set_dark_color(qr, lv_color_hex(0x000000));
        lv_qrcode_set_light_color(qr, lv_color_hex(0xffffff));
        lv_qrcode_set_data(qr, kMuseumUrl);
        lv_obj_align(qr, LV_ALIGN_TOP_MID, 0, 80);

        lv_obj_t *url = lv_label_create(root);
        lv_label_set_text(url, kMuseumUrl);
        lv_obj_set_width(url, 304);
        lv_label_set_long_mode(url, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_align(url, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_font(url, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(url, lv_color_hex(BadgeTheme::ACCENT_DETAIL), 0);
        lv_obj_align(url, LV_ALIGN_TOP_MID, 0, 200);

        lv_obj_t *hint = lv_label_create(root);
        lv_label_set_text(hint, "Scan to explore  |  HOME to return");
        lv_obj_set_style_text_font(hint, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(hint, lv_color_hex(BadgeTheme::TEXT_DIM), 0);
        lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -6);
    }
};

MuseumApp s_instance;

} // namespace

App *app_museum_instance() { return &s_instance; }
