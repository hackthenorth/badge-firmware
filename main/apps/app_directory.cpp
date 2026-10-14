#include "app.h"
#include "app_registry.h"

extern "C" const lv_image_dsc_t icon_directory;

#include <cstddef>
#include <cstdint>

namespace {

constexpr uint32_t kColorBg = BadgeTheme::BACKGROUND;
constexpr uint32_t kColorPanel = 0x151f2e;
constexpr uint32_t kColorBorder = BadgeTheme::BORDER;
constexpr uint32_t kColorText = BadgeTheme::TEXT;
constexpr uint32_t kColorDim = BadgeTheme::TEXT_MUTED;
constexpr uint32_t kColorAccent = BadgeTheme::ACCENT_DETAIL;

size_t public_app_count()
{
    size_t count = 0;
    for (size_t i = 0; i < app_registry_count(); ++i) {
        App *app = app_registry_get(i);
        if (app && app->access() == AppAccess::Public) ++count;
    }
    return count;
}

App *public_app_at(size_t index)
{
    size_t found = 0;
    for (size_t i = 0; i < app_registry_count(); ++i) {
        App *app = app_registry_get(i);
        if (!app || app->access() != AppAccess::Public) continue;
        if (found++ == index) return app;
    }
    return nullptr;
}

void set_label_style(lv_obj_t *label, uint32_t color, const lv_font_t *font)
{
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_obj_set_style_text_font(label, font, 0);
}

class DirectoryApp final : public App {
public:
    const char *name() const override { return "Directory"; }
    const char *icon() const override { return "DIR"; }
    const lv_image_dsc_t *icon_image() const override { return &icon_directory; }
    const char *slug() const override { return "directory"; }
    AppAccess access() const override { return AppAccess::Internal; }

    void on_enter(lv_obj_t *root) override
    {
        root_ = root;
        page_ = 0;
        render();
    }

    void on_exit() override
    {
        root_ = nullptr;
    }

    void on_button(ButtonEvent ev) override
    {
        if (ev.kind != ButtonEventKind::Pressed || !root_) return;

        const size_t pages = public_app_count() + 1; // format guide + each app
        switch (ev.button) {
        case Button::Up:
        case Button::Left:
            page_ = page_ == 0 ? pages - 1 : page_ - 1;
            render();
            break;
        case Button::Down:
        case Button::Right:
        case Button::A:
            page_ = (page_ + 1) % pages;
            render();
            break;
        case Button::Start:
            // HOME is the registry's way home and never reaches us; Start
            // jumps back to the format guide instead.
            page_ = 0;
            render();
            break;
        default:
            break;
        }
    }

private:
    void render()
    {
        const size_t app_count = public_app_count();
        if (page_ > app_count) page_ = 0;

        lv_obj_clean(root_);
        lv_obj_set_style_bg_color(root_, lv_color_hex(kColorBg), 0);
        lv_obj_set_style_bg_opa(root_, LV_OPA_COVER, 0);

        make_header();
        if (page_ == 0) render_format();
        else render_app(public_app_at(page_ - 1), app_count);
        make_footer();
    }

    void make_header()
    {
        lv_obj_t *title = lv_label_create(root_);
        lv_label_set_text(title, "Directory");
        set_label_style(title, kColorText, &lv_font_montserrat_22);
        lv_obj_set_pos(title, 12, 7);

        lv_obj_t *pill = lv_obj_create(root_);
        lv_obj_remove_style_all(pill);
        lv_obj_set_size(pill, 82, 24);
        lv_obj_set_pos(pill, 226, 8);
        lv_obj_set_style_radius(pill, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(pill, lv_color_hex(kColorBorder), 0);
        lv_obj_set_style_bg_opa(pill, LV_OPA_COVER, 0);
        lv_obj_clear_flag(pill, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *pill_label = lv_label_create(pill);
        lv_label_set_text(pill_label, "INTERNAL");
        set_label_style(pill_label, kColorText, &lv_font_montserrat_14);
        lv_obj_center(pill_label);

        lv_obj_t *line = lv_obj_create(root_);
        lv_obj_remove_style_all(line);
        lv_obj_set_size(line, 296, 1);
        lv_obj_set_pos(line, 12, 40);
        lv_obj_set_style_bg_color(line, lv_color_hex(kColorBorder), 0);
        lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);
    }

    void render_format()
    {
        lv_obj_t *eyebrow = lv_label_create(root_);
        lv_label_set_text(eyebrow, "UNLOCK CARD FORMAT");
        set_label_style(eyebrow, kColorAccent, &lv_font_montserrat_14);
        lv_obj_set_pos(eyebrow, 14, 50);

        lv_obj_t *instructions = lv_label_create(root_);
        lv_label_set_text(instructions, "Type: Text");
        set_label_style(instructions, kColorText, &lv_font_montserrat_20);
        lv_obj_set_pos(instructions, 14, 76);

        lv_obj_t *example = lv_obj_create(root_);
        lv_obj_remove_style_all(example);
        lv_obj_set_size(example, 292, 43);
        lv_obj_set_pos(example, 14, 116);
        lv_obj_set_style_radius(example, 7, 0);
        lv_obj_set_style_bg_color(example, lv_color_hex(kColorPanel), 0);
        lv_obj_set_style_bg_opa(example, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(example, 1, 0);
        lv_obj_set_style_border_color(example, lv_color_hex(kColorAccent), 0);
        lv_obj_clear_flag(example, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *example_label = lv_label_create(example);
        lv_label_set_text(example_label, "Example text:  snake");
        set_label_style(example_label, kColorText, &lv_font_montserrat_18);
        lv_obj_center(example_label);
    }

    void render_app(App *app, size_t app_count)
    {
        if (!app) {
            lv_obj_t *empty = lv_label_create(root_);
            lv_label_set_text(empty, "No public apps registered");
            set_label_style(empty, kColorDim, &lv_font_montserrat_18);
            lv_obj_center(empty);
            return;
        }

        lv_obj_t *position = lv_label_create(root_);
        lv_label_set_text_fmt(position, "PUBLIC APP  %u / %u",
                              static_cast<unsigned>(page_),
                              static_cast<unsigned>(app_count));
        set_label_style(position, kColorAccent, &lv_font_montserrat_14);
        lv_obj_set_pos(position, 14, 50);

        lv_obj_t *app_name = lv_label_create(root_);
        lv_label_set_text(app_name, app->name());
        lv_obj_set_width(app_name, 292);
        set_label_style(app_name, kColorText, &lv_font_montserrat_22);
        lv_obj_set_pos(app_name, 14, 72);

        lv_obj_t *payload_title = lv_label_create(root_);
        lv_label_set_text(payload_title, "TEXT TO WRITE");
        set_label_style(payload_title, kColorDim, &lv_font_montserrat_14);
        lv_obj_set_pos(payload_title, 14, 107);

        lv_obj_t *payload = lv_obj_create(root_);
        lv_obj_remove_style_all(payload);
        lv_obj_set_size(payload, 292, 52);
        lv_obj_set_pos(payload, 14, 128);
        lv_obj_set_style_radius(payload, 7, 0);
        lv_obj_set_style_bg_color(payload, lv_color_hex(kColorPanel), 0);
        lv_obj_set_style_bg_opa(payload, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(payload, 2, 0);
        lv_obj_set_style_border_color(payload, lv_color_hex(kColorAccent), 0);
        lv_obj_clear_flag(payload, LV_OBJ_FLAG_SCROLLABLE);

        // Scanner matches this exact registry slug. Directory therefore stays
        // correct when a native or Lua public app is added or renamed.
        lv_obj_t *payload_label = lv_label_create(payload);
        lv_label_set_text(payload_label, app->slug());
        lv_obj_set_width(payload_label, 270);
        lv_label_set_long_mode(payload_label, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_align(payload_label, LV_TEXT_ALIGN_CENTER, 0);
        set_label_style(payload_label, kColorText, &lv_font_montserrat_18);
        lv_obj_center(payload_label);

        lv_obj_t *note = lv_label_create(root_);
        lv_label_set_text(note, "Write exactly this text");
        set_label_style(note, kColorDim, &lv_font_montserrat_14);
        lv_obj_align(note, LV_ALIGN_TOP_MID, 0, 188);
    }

    void make_footer()
    {
        lv_obj_t *footer = lv_label_create(root_);
        lv_label_set_text(footer, "UP/DOWN: browse   START: guide   HOME: home");
        set_label_style(footer, kColorDim, &lv_font_montserrat_14);
        lv_obj_align(footer, LV_ALIGN_BOTTOM_MID, 0, -5);
    }

    lv_obj_t *root_ = nullptr;
    size_t page_ = 0;
};

DirectoryApp s_instance;

} // namespace

App *app_directory_instance() { return &s_instance; }
