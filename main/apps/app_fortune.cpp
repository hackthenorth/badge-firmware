#include "app.h"
#include "hal_gesture.h"
#include "hal_lights.h"

#include "esp_random.h"
#include <cstdio>

// Fortune cookie. A (or a good shake) cracks the cookie: one fortune
// plus three lucky numbers, with a gold LED burst.

extern "C" const lv_image_dsc_t icon_fortune;

class FortuneApp : public GameApp {
public:
    const char *name() const override { return "Fortune Cookie"; }
    const char *icon() const override { return "FRT"; }
    const lv_image_dsc_t *icon_image() const override { return &icon_fortune; }
    const char *slug() const override { return "fortune"; }

    void on_enter(lv_obj_t *root) override
    {
        count_ = 0;

        title_ = lv_label_create(root);
        lv_label_set_text(title_, "Fortune Cookie");
        lv_obj_set_style_text_color(title_, lv_color_hex(BadgeTheme::TEXT), 0);
        lv_obj_set_style_text_font(title_, &lv_font_montserrat_20, 0);
        lv_obj_align(title_, LV_ALIGN_TOP_MID, 0, 6);

        cookie_ = lv_label_create(root);
        lv_obj_set_style_text_color(cookie_, lv_color_hex(BadgeTheme::ACCENT_DETAIL), 0);
        lv_obj_set_style_text_font(cookie_, &lv_font_montserrat_24, 0);
        lv_obj_align(cookie_, LV_ALIGN_CENTER, 0, -40);

        text_ = lv_label_create(root);
        lv_obj_set_style_text_color(text_, lv_color_hex(BadgeTheme::TEXT), 0);
        lv_obj_set_style_text_font(text_, &lv_font_montserrat_18, 0);
        lv_label_set_long_mode(text_, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(text_, 280);
        lv_obj_set_style_text_align(text_, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(text_, LV_ALIGN_CENTER, 0, 20);

        lucky_ = lv_label_create(root);
        lv_obj_set_style_text_color(lucky_, lv_color_hex(BadgeTheme::TEXT_MUTED), 0);
        lv_obj_align(lucky_, LV_ALIGN_CENTER, 0, 62);

        hint_ = lv_label_create(root);
        lv_obj_set_style_text_color(hint_, lv_color_hex(BadgeTheme::TEXT_MUTED), 0);
        lv_label_set_text(hint_, "A or SHAKE: crack one");
        lv_obj_align(hint_, LV_ALIGN_BOTTOM_MID, 0, -10);

        if (cookie_) lv_label_set_text(cookie_, "(Cookie)");
        if (text_) lv_label_set_text(text_, "The crumbs know all.");
        hal_lights_clear();
        hal_lights_show();
    }

    void on_exit() override
    {
        hal_lights_clear();
        hal_lights_show();
        title_ = cookie_ = text_ = lucky_ = hint_ = nullptr;
    }

    void on_button(ButtonEvent ev) override
    {
        if (ev.kind != ButtonEventKind::Pressed || ev.button != Button::A) return;
        crack();
    }

    void on_tick() override
    {
        if (gesture_poll_shake()) crack();
        if (glow_ticks_ > 0 && --glow_ticks_ == 0) {
            hal_lights_clear();
            hal_lights_show();
        }
    }

private:
    static constexpr const char *kFortunes[] = {
        "You will ship before the deadline.",
        "A stranger's demo will change your weekend.",
        "Beware the merge conflict on Sunday.",
        "Your side project is the main project now.",
        "Ask the person next to you what they built.",
        "The workshop you almost skipped is the one.",
        "Coffee first. Flash firmware later.",
        "Your LED color brings good luck today.",
        "A 3am idea deserves a prototype.",
        "Help someone debug; keep the karma.",
        "The docs are wrong. Trust your readings.",
        "Sleep is a valid power-saving mode.",
    };
    static constexpr int kFortuneCount = sizeof(kFortunes) / sizeof(kFortunes[0]);

    void crack()
    {
        count_++;
        if (cookie_) lv_label_set_text(cookie_, "(Cracked!)");
        if (text_) lv_label_set_text(text_, kFortunes[esp_random() % kFortuneCount]);
        if (lucky_) {
            lv_label_set_text_fmt(lucky_, "Lucky %d %d %d  #%d", (int)(esp_random() % 99) + 1,
                                  (int)(esp_random() % 99) + 1, (int)(esp_random() % 99) + 1,
                                  count_);
        }
        for (size_t i = 0; i < LED_COUNT; i++) hal_lights_set(i, 255, 200, 40);
        hal_lights_show();
        glow_ticks_ = 40; // ~0.8 s of gold
    }

    lv_obj_t *title_ = nullptr;
    lv_obj_t *cookie_ = nullptr;
    lv_obj_t *text_ = nullptr;
    lv_obj_t *lucky_ = nullptr;
    lv_obj_t *hint_ = nullptr;
    int count_ = 0;
    int glow_ticks_ = 0;
};

static FortuneApp s_instance;
App *app_fortune_instance() { return &s_instance; }
