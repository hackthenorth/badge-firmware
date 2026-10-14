#include "app.h"
#include "hal_identity.h"

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

enum class SocialKind : uint8_t {
    Email,
    Phone,
    LinkedIn,
    Discord,
    Instagram,
    X,
};

struct SocialItem {
    SocialKind kind;
    const char *name;
    const char *value;
    uint32_t color;
};

constexpr size_t kMaxSocials = 6;

const char *without_at(const char *value)
{
    return value && value[0] == '@' ? value + 1 : value;
}

bool is_web_url(const char *value)
{
    return value && (strncmp(value, "https://", 8) == 0
        || strncmp(value, "http://", 7) == 0);
}

bool is_numeric(const char *value)
{
    if (!value || !*value) return false;
    for (const char *p = value; *p; ++p) {
        if (!std::isdigit(static_cast<unsigned char>(*p))) return false;
    }
    return true;
}

extern "C" const lv_image_dsc_t icon_socials;

class SocialsApp final : public App {
public:
    const char *name() const override { return "Socials"; }
    const char *icon() const override { return "WEB"; }
    const lv_image_dsc_t *icon_image() const override { return &icon_socials; }
    bool icon_is_mask() const override { return true; }
    const char *slug() const override { return "socials"; }
    AppAccess access() const override { return AppAccess::Default; }

    void on_enter(lv_obj_t *root) override
    {
        root_ = root;
        selected_ = 0;
        showing_qr_ = false;
        load_socials();
        render();
    }

    void on_exit() override
    {
        root_ = nullptr;
        count_ = 0;
        showing_qr_ = false;
    }

    void on_button(ButtonEvent ev) override
    {
        if (ev.kind != ButtonEventKind::Pressed) return;

        if (showing_qr_) {
            if (ev.button == Button::B) {
                showing_qr_ = false;
                render();
            } else if ((ev.button == Button::Left || ev.button == Button::Up)
                       && count_ > 0) {
                selected_ = (selected_ + count_ - 1) % count_;
                render();
            } else if ((ev.button == Button::Right || ev.button == Button::Down)
                       && count_ > 0) {
                selected_ = (selected_ + 1) % count_;
                render();
            }
            return;
        }

        if (count_ == 0) return;
        if (ev.button == Button::Up || ev.button == Button::Left) {
            selected_ = (selected_ + count_ - 1) % count_;
            render();
        } else if (ev.button == Button::Down || ev.button == Button::Right) {
            selected_ = (selected_ + 1) % count_;
            render();
        } else if (ev.button == Button::A || ev.button == Button::Start) {
            showing_qr_ = true;
            render();
        }
    }

private:
    void add(SocialKind kind, const char *name, const char *value, uint32_t color)
    {
        if (!value || !*value || count_ >= kMaxSocials) return;
        items_[count_++] = {kind, name, value, color};
    }

    void load_socials()
    {
        count_ = 0;
        const BadgeIdentity *id = hal_identity_get();
        add(SocialKind::Instagram, "Instagram", id->net_instagram, 0xe95991);
        add(SocialKind::LinkedIn, "LinkedIn", id->net_linkedin, 0x4aa3df);
        add(SocialKind::Discord, "Discord", id->net_discord, 0x8b9dff);
        add(SocialKind::X, "X", id->net_x, 0xe5e7eb);
        add(SocialKind::Email, "Email", id->net_email, 0x51d7a2);
        add(SocialKind::Phone, "Phone", id->net_phone, 0xffc857);
    }

    void style_root()
    {
        lv_obj_clean(root_);
        lv_obj_set_style_bg_color(root_, lv_color_hex(BadgeTheme::BACKGROUND), 0);
        lv_obj_set_style_bg_opa(root_, LV_OPA_COVER, 0);
    }

    void render()
    {
        style_root();
        if (showing_qr_ && count_ > 0) render_qr();
        else render_list();
    }

    void render_list()
    {
        lv_obj_t *title = lv_label_create(root_);
        lv_label_set_text(title, "My Socials");
        lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(title, lv_color_hex(BadgeTheme::TEXT), 0);
        lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 5);

        lv_obj_t *blurb = lv_label_create(root_);
        lv_label_set_text(blurb, "Let someone scan a QR to connect with you.");
        lv_obj_set_width(blurb, 296);
        lv_obj_set_style_text_align(blurb, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_font(blurb, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(blurb, lv_color_hex(BadgeTheme::TEXT_SOFT), 0);
        lv_obj_align(blurb, LV_ALIGN_TOP_MID, 0, 31);

        if (count_ == 0) {
            lv_obj_t *empty = lv_label_create(root_);
            lv_label_set_text(empty,
                "No social profiles are loaded yet.\n\nAdd them when your badge is registered.");
            lv_obj_set_width(empty, 280);
            lv_label_set_long_mode(empty, LV_LABEL_LONG_WRAP);
            lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_set_style_text_font(empty, &lv_font_montserrat_14, 0);
            lv_obj_set_style_text_color(empty, lv_color_hex(BadgeTheme::TEXT_SOFT), 0);
            lv_obj_align(empty, LV_ALIGN_CENTER, 0, -4);
        } else {
            for (size_t i = 0; i < count_; ++i) {
                const bool active = i == selected_;
                lv_obj_t *row = lv_obj_create(root_);
                lv_obj_set_size(row, 296, 25);
                lv_obj_set_pos(row, 12, 54 + static_cast<int>(i) * 27);
                lv_obj_set_style_radius(row, 7, 0);
                lv_obj_set_style_pad_all(row, 0, 0);
                lv_obj_set_style_bg_color(row,
                    lv_color_hex(active ? 0x1e2b3d : 0x101820), 0);
                lv_obj_set_style_border_width(row, active ? 2 : 1, 0);
                lv_obj_set_style_border_color(row,
                    lv_color_hex(active ? items_[i].color : 0x263342), 0);
                lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

                lv_obj_t *name = lv_label_create(row);
                lv_label_set_text(name, items_[i].name);
                lv_obj_set_style_text_font(name, &lv_font_montserrat_12, 0);
                lv_obj_set_style_text_color(name, lv_color_hex(items_[i].color), 0);
                lv_obj_align(name, LV_ALIGN_LEFT_MID, 8, 0);

                lv_obj_t *value = lv_label_create(row);
                lv_label_set_text(value, items_[i].value);
                lv_obj_set_width(value, 184);
                lv_label_set_long_mode(value, LV_LABEL_LONG_DOT);
                lv_obj_set_style_text_align(value, LV_TEXT_ALIGN_RIGHT, 0);
                lv_obj_set_style_text_font(value, &lv_font_montserrat_12, 0);
                lv_obj_set_style_text_color(value, lv_color_hex(BadgeTheme::TEXT), 0);
                lv_obj_align(value, LV_ALIGN_RIGHT_MID, -8, 0);
            }
        }

        lv_obj_t *hint = lv_label_create(root_);
        lv_label_set_text(hint, count_ == 0
            ? "HOME  apps"
            : "UP / DOWN  choose     A  show QR");
        lv_obj_set_style_text_font(hint, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(hint, lv_color_hex(BadgeTheme::TEXT_DIM), 0);
        lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -7);
    }

    void qr_payload(const SocialItem &item, char *out, size_t out_size)
    {
        const char *value = item.value;
        switch (item.kind) {
        case SocialKind::Email:
            snprintf(out, out_size, "mailto:%s", value);
            break;
        case SocialKind::Phone:
            snprintf(out, out_size, "tel:%s", value);
            break;
        case SocialKind::LinkedIn:
            if (is_web_url(value)) snprintf(out, out_size, "%s", value);
            else snprintf(out, out_size, "https://www.linkedin.com/in/%s", value);
            break;
        case SocialKind::Instagram:
            if (is_web_url(value)) snprintf(out, out_size, "%s", value);
            else snprintf(out, out_size, "https://www.instagram.com/%s", without_at(value));
            break;
        case SocialKind::X:
            if (is_web_url(value)) snprintf(out, out_size, "%s", value);
            else snprintf(out, out_size, "https://x.com/%s", without_at(value));
            break;
        case SocialKind::Discord:
            if (is_web_url(value)) snprintf(out, out_size, "%s", value);
            else if (is_numeric(value)) snprintf(out, out_size, "https://discord.com/users/%s", value);
            else snprintf(out, out_size, "Discord: %s", without_at(value));
            break;
        }
    }

    void render_qr()
    {
        const SocialItem &item = items_[selected_];
        char payload[128];
        qr_payload(item, payload, sizeof(payload));

        lv_obj_t *title = lv_label_create(root_);
        lv_label_set_text_fmt(title, "%s  %u/%u", item.name,
            static_cast<unsigned>(selected_ + 1), static_cast<unsigned>(count_));
        lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(title, lv_color_hex(item.color), 0);
        lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 5);

        lv_obj_t *qr = lv_qrcode_create(root_);
        lv_qrcode_set_size(qr, 148);
        lv_qrcode_set_dark_color(qr, lv_color_hex(0x000000));
        lv_qrcode_set_light_color(qr, lv_color_hex(0xffffff));
        lv_qrcode_set_data(qr, payload);
        lv_obj_align(qr, LV_ALIGN_TOP_MID, 0, 34);

        lv_obj_t *value = lv_label_create(root_);
        lv_label_set_text(value, item.value);
        lv_obj_set_width(value, 304);
        lv_label_set_long_mode(value, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_align(value, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_font(value, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(value, lv_color_hex(BadgeTheme::TEXT), 0);
        lv_obj_align(value, LV_ALIGN_TOP_MID, 0, 187);

        lv_obj_t *hint = lv_label_create(root_);
        lv_label_set_text(hint, "LEFT / RIGHT  switch     B  list");
        lv_obj_set_style_text_font(hint, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(hint, lv_color_hex(BadgeTheme::TEXT_DIM), 0);
        lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -7);
    }

    lv_obj_t *root_ = nullptr;
    SocialItem items_[kMaxSocials] = {};
    size_t count_ = 0;
    size_t selected_ = 0;
    bool showing_qr_ = false;
};

SocialsApp s_instance;

} // namespace

App *app_socials_instance() { return &s_instance; }
