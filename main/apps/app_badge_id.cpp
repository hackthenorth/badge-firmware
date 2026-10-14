#include "app.h"
#include "app_launcher.h"
#include "app_registry.h"
#include "hal_identity.h"
#include "hal_lights.h"
#include "launcher_slugs.h"
#include "pal_picker.h"
#include "ripple_state.h"
#include "solana_wallet.h"

#include <cstdint>
#include <cstdio>

extern "C" const lv_image_dsc_t icon_card;

// The badge ID page: who you are, and the QR someone scans to connect with
// you. This is the boot target, and the first tile in the launcher.
//
// Deliberately an ordinary App rather than a pane bolted onto the launcher.
// The launcher already owns paging, Move mode, and launcher_slugs membership;
// adding a "position -1" to it would mean special-casing this screen out of
// all three. As a normal app it gets the tick and grid slot for free; Home
// explicitly reveals the launcher behind it.

// Where a scanned QR sends someone. The year keeps routes stable across
// events, while the full badge ID selects this attendee's public profile.
static const char *kProfileUrlPrefix = "https://my.hackthenorth.com/qr/2026/";

// Attendee secret. It is recognized only while the QR side of this
// app is visible and reveals exactly one harmless sensory toy.
static constexpr Button kBubbleKonamiCode[] = {
    Button::Up, Button::Up, Button::Down, Button::Down,
    Button::Left, Button::Right, Button::Left, Button::Right,
    Button::B, Button::A,
};
static constexpr int kPalSize = 112;

class BadgeIdApp : public App {
public:
    const char *name() const override { return "My Badge"; }
    const char *icon() const override { return "ME"; }
    const lv_image_dsc_t *icon_image() const override { return &icon_card; }
    const char *slug() const override { return "badgeid"; }
    AppAccess access() const override { return AppAccess::Default; }
    bool handles_home_button() const override { return true; }

    void on_enter(lv_obj_t *root) override
    {
        root_ = root;
        show_details_ = false;
        customizing_ = false;
        konami_pos_ = 0;
        consumed_release_mask_ = 0;
        render();
    }

    void on_exit() override
    {
        root_ = nullptr;
        qr_ = nullptr;
        consumed_release_mask_ = 0;
    }

    void on_tick() override
    {
        // USB profile writes can happen while this screen is already open.
        // Rebuild both name and QR from the current identity on the UI task.
        if (rendered_revision_ != hal_identity_revision()) render();
        // `prov apply` reloads identity without requiring an operator reboot.
        // As soon as valid registration data exists, hand this badge directly
        // into its freshly-reset onboarding flow.
        if (hal_identity_is_provisioned() && badge_onboarding_required()) {
            app_registry_launch(app_onboarding_instance());
            return;
        }
    }

    void on_button(ButtonEvent ev) override
    {
        // Before registration this page is a hard gate, not an app launcher.
        // START remains useful on the line by opening the hardware self-test;
        // the attendee secret stays disabled until an owner exists.
        if (!hal_identity_is_provisioned()) {
            if (ev.kind == ButtonEventKind::Pressed && ev.button == Button::Start) {
                app_registry_launch(app_test_instance());
            }
            return;
        }

        if (ev.kind == ButtonEventKind::Pressed && ev.button == Button::Home) {
            app_registry_return_to_launcher();
            return;
        }
        if (customizing_) {
            if (picker_.on_button(ev)) {
                customizing_ = false;
                render();
            }
            return;
        }
        const uint16_t bit = button_bit(ev.button);
        if (ev.kind == ButtonEventKind::Released) {
            const bool consumed = (consumed_release_mask_ & bit) != 0;
            consumed_release_mask_ &= ~bit;
            if (consumed) return;

            if (ev.button == Button::B) {
                if (show_details_) {
                    show_details_ = false;
                    render();
                }
            } else if (ev.button == Button::Start) {
                toggle_details();
            }
            return;
        }

        // The secret consumes matching presses and their later releases. This
        // keeps the exact sequence on the QR side without B leaving the page
        // or Left/Right changing the equipped pal partway through.
        if (!show_details_ && advance_bubble_konami(ev.button)) {
            consumed_release_mask_ |= bit;
            return;
        }
        if (show_details_) konami_pos_ = 0;

        if (ev.button == Button::Up) {
            show_details_ = false;
            render();
        } else if (ev.button == Button::A && !show_details_) {
            customizing_ = true;
            qr_ = nullptr;
            picker_.show(root_);
        }

        // B and Start are handled on release. This lets a matched
        // secret-key release be consumed without firing its ordinary action.
    }

private:
    uint32_t rendered_revision_ = 0;

    static constexpr uint16_t button_bit(Button button)
    {
        return (uint16_t)(1u << (uint8_t)button);
    }

    bool advance_bubble_konami(Button button)
    {
        const size_t count = sizeof(kBubbleKonamiCode) / sizeof(kBubbleKonamiCode[0]);
        const bool matched = button == kBubbleKonamiCode[konami_pos_];
        if (matched) {
            konami_pos_++;
        } else {
            konami_pos_ = button == kBubbleKonamiCode[0] ? 1 : 0;
        }
        if (konami_pos_ == count) {
            konami_pos_ = 0;
            App *bubble_wrap = app_bubble_wrap_instance();
            launcher_slugs_append(bubble_wrap->slug());
            app_registry_launch(bubble_wrap);
            return true;
        }
        return matched;
    }

    void toggle_details()
    {
        show_details_ = !show_details_;
        render();
    }

    void render()
    {
        if (!root_) return;
        rendered_revision_ = hal_identity_revision();
        lv_obj_clean(root_);
        qr_ = nullptr;

        if (!hal_identity_is_provisioned()) {
            render_unprovisioned();
            return;
        }
        if (customizing_) { picker_.show(root_); return; }
        if (show_details_) render_details();
        else               render_qr();
    }

    // A badge that was never programmed, or whose identity file was lost.
    // Say so plainly and point at the fix rather than showing a blank card.
    // The LEDs shine solid white so staff can spot unprogrammed badges
    // across the room; provisioning reboots into onboarding, which owns
    // the lights from there.
    void render_unprovisioned()
    {
        for (size_t i = 0; i < LED_COUNT; ++i) {
            hal_lights_set(i, 255, 255, 255);
        }
        hal_lights_show();

        lv_obj_t *title = lv_label_create(root_);
        lv_label_set_text(title, "Not registered yet");
        lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
        lv_obj_set_style_text_color(title, lv_color_hex(0xff5a5f), 0);
        lv_obj_align(title, LV_ALIGN_CENTER, 0, -30);

        lv_obj_t *body = lv_label_create(root_);
        lv_label_set_text(body,
            "This badge hasn't been linked\n"
            "to an attendee.\n\n"
            "Bring it to a registration desk.");
        lv_obj_set_style_text_align(body, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(body, lv_color_hex(BadgeTheme::TEXT_SOFT), 0);
        lv_obj_align(body, LV_ALIGN_CENTER, 0, 30);

        hint("Press START for hardware self-test");
    }

    void render_qr()
    {
        const BadgeIdentity *id = hal_identity_get();

        role_band(id);

        char url[sizeof(id->badge_id) + 48];
        snprintf(url, sizeof(url), "%s%s", kProfileUrlPrefix, id->badge_id);

        const uint8_t pal_id = solana_wallet_equipped_pal();
        const lv_image_dsc_t *artwork = pal_image(pal_id);

        qr_ = lv_qrcode_create(root_);
        lv_qrcode_set_size(qr_, artwork ? kPalSize : 128);
        lv_qrcode_set_dark_color(qr_, lv_color_hex(0x000000));
        lv_qrcode_set_light_color(qr_, lv_color_hex(0xffffff));
        lv_qrcode_set_data(qr_, url);
        if (artwork) {
            // Two equal 112 px squares with a 16 px gutter, centered in 320 px.
            lv_obj_set_pos(qr_, 40, 34);
            lv_obj_t *pal = lv_image_create(root_);
            lv_image_set_src(pal, artwork);
            lv_obj_set_pos(pal, 168, 34);
        } else {
            lv_obj_align(qr_, LV_ALIGN_TOP_MID, 0, 32);
        }

        // Use the remaining 80 px as one flex column so the gaps after the QR,
        // name, and badge ID stay visually even regardless of font metrics.
        lv_obj_t *info = lv_obj_create(root_);
        lv_obj_remove_style_all(info);
        lv_obj_set_size(info, LV_PCT(100), 80);
        lv_obj_align(info, LV_ALIGN_BOTTOM_MID, 0, 0);
        lv_obj_set_flex_flow(info, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(info,
            LV_FLEX_ALIGN_SPACE_EVENLY,
            LV_FLEX_ALIGN_CENTER,
            LV_FLEX_ALIGN_CENTER);
        lv_obj_clear_flag(info, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *nm = lv_label_create(info);
        lv_label_set_text(nm, id->display_name);
        lv_obj_set_style_text_font(nm, &lv_font_montserrat_24, 0);
        lv_obj_set_style_text_color(nm, lv_color_hex(BadgeTheme::TEXT), 0);

        lv_obj_t *badge_id = lv_label_create(info);
        lv_label_set_text_fmt(badge_id, "Badge ID: %s", id->badge_id);
        lv_obj_set_width(badge_id, 304);
        lv_label_set_long_mode(badge_id, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_font(badge_id, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_align(badge_id, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(badge_id, lv_color_hex(BadgeTheme::TEXT_MUTED), 0);

        lv_obj_t *qr_hint = make_hint(
            info, "A Customize   Start Details   Home Apps");
        lv_obj_set_width(qr_hint, 304);
        lv_obj_set_style_text_font(qr_hint, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_align(qr_hint, LV_TEXT_ALIGN_CENTER, 0);
        render_ripple_marker();
    }

    void render_ripple_marker()
    {
        const RippleState ripple = ripple_state_load();
        if (!ripple.active) return;

        // A tiny, deliberately unexplained curiosity on the QR page. It sits
        // outside the QR itself, so it cannot reduce scan reliability.
        lv_obj_t *depth = lv_label_create(root_);
        lv_label_set_text_fmt(depth, "%u", static_cast<unsigned>(ripple.depth));
        lv_obj_set_style_text_font(depth, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(depth, lv_color_hex(BadgeTheme::TEXT_DIM), 0);
        lv_obj_align(depth, LV_ALIGN_BOTTOM_RIGHT, -4, -4);
    }

    void render_details()
    {
        const BadgeIdentity *id = hal_identity_get();

        role_band(id);

        lv_obj_t *nm = lv_label_create(root_);
        lv_label_set_text(nm, id->display_name);
        lv_obj_set_style_text_font(nm, &lv_font_montserrat_24, 0);
        lv_obj_set_style_text_color(nm, lv_color_hex(BadgeTheme::TEXT), 0);
        lv_obj_align(nm, LV_ALIGN_TOP_MID, 0, 38);

        auto value_or_dash = [](const char *value) {
            return value && *value ? value : "-";
        };
        char body[512];
        snprintf(body, sizeof(body),
            "Email: %s\n"
            "LinkedIn: %s\n"
            "Discord: %s\n"
            "Instagram: %s\n"
            "Twitter/X: %s\n"
            "Phone number: %s\n"
            "Badge ID:\n%s",
            value_or_dash(id->net_email),
            value_or_dash(id->net_linkedin),
            value_or_dash(id->net_discord),
            value_or_dash(id->net_instagram),
            value_or_dash(id->net_x),
            value_or_dash(id->net_phone),
            id->badge_id);

        lv_obj_t *lb = lv_label_create(root_);
        lv_label_set_text(lb, body);
        lv_obj_set_width(lb, 304);
        lv_label_set_long_mode(lb, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_font(lb, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_align(lb, LV_TEXT_ALIGN_LEFT, 0);
        lv_obj_set_style_text_color(lb, lv_color_hex(BadgeTheme::TEXT_SOFT), 0);
        lv_obj_align(lb, LV_ALIGN_TOP_MID, 0, 68);

        hint("Start/Up/B QR   Home Apps");
    }

    // Role colour matches the lanyard, so a glance at the top of the screen
    // tells an organizer what someone is without reading anything.
    void role_band(const BadgeIdentity *id)
    {
        uint8_t color[3];
        hal_identity_role_default_color(id->role, color);

        lv_obj_t *band = lv_obj_create(root_);
        lv_obj_remove_style_all(band);
        lv_obj_set_size(band, LV_PCT(100), 26);
        lv_obj_set_style_bg_color(band,
            lv_color_make(color[0], color[1], color[2]), 0);
        lv_obj_set_style_bg_opa(band, LV_OPA_COVER, 0);
        lv_obj_clear_flag(band, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_align(band, LV_ALIGN_TOP_MID, 0, 0);

        lv_obj_t *rl = lv_label_create(band);
        lv_label_set_text(rl, hal_identity_role_name(id->role));
        const uint32_t luma = 299u * color[0] + 587u * color[1] + 114u * color[2];
        lv_obj_set_style_text_color(rl,
            lv_color_hex(luma >= 125000u ? BadgeTheme::BACKGROUND : 0xf7f7f2), 0);
        lv_obj_center(rl);
    }

    lv_obj_t *make_hint(lv_obj_t *parent, const char *text)
    {
        lv_obj_t *h = lv_label_create(parent);
        lv_label_set_text(h, text);
        lv_obj_set_style_text_font(h, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(h, lv_color_hex(BadgeTheme::TEXT_DIM), 0);
        return h;
    }

    void hint(const char *text)
    {
        lv_obj_t *h = make_hint(root_, text);
        lv_obj_align(h, LV_ALIGN_BOTTOM_MID, 0, -4);
    }

    lv_obj_t *root_ = nullptr;
    lv_obj_t *qr_ = nullptr;
    bool show_details_ = false;
    bool customizing_ = false;
    PalPicker picker_;
    uint16_t consumed_release_mask_ = 0;
    size_t konami_pos_ = 0;
};

static BadgeIdApp s_instance;
App *app_badge_id_instance() { return &s_instance; }
