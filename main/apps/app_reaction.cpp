#include "app.h"
#include "hal_config.h"
#include "hal_lights.h"

#include "esp_random.h"
#include "esp_timer.h"
#include <cstdint>
#include <cstdio>

// LED reaction test. LEDs glow red while armed, flip green on GO —
// press A as fast as you can. False starts are called out.

extern "C" const lv_image_dsc_t icon_reaction;

class ReactionApp : public GameApp {
public:
    const char *name() const override { return "React"; }
    const char *icon() const override { return "RCT"; }
    const lv_image_dsc_t *icon_image() const override { return &icon_reaction; }
    const char *slug() const override { return "reaction"; }

    void on_enter(lv_obj_t *root) override
    {
        best_ = hal_config_get_int("reaction", "best_ms", 0);
        state_ = State::Idle;

        title_ = lv_label_create(root);
        lv_label_set_text(title_, "React");
        lv_obj_set_style_text_color(title_, lv_color_hex(BadgeTheme::TEXT), 0);
        lv_obj_set_style_text_font(title_, &lv_font_montserrat_20, 0);
        lv_obj_align(title_, LV_ALIGN_TOP_MID, 0, 6);

        big_ = lv_label_create(root);
        lv_obj_set_style_text_color(big_, lv_color_hex(BadgeTheme::TEXT), 0);
        lv_obj_set_style_text_font(big_, &lv_font_montserrat_24, 0);
        lv_obj_align(big_, LV_ALIGN_CENTER, 0, -10);

        sub_ = lv_label_create(root);
        lv_obj_set_style_text_color(sub_, lv_color_hex(BadgeTheme::TEXT_MUTED), 0);
        lv_obj_align(sub_, LV_ALIGN_CENTER, 0, 30);

        hint_ = lv_label_create(root);
        lv_obj_set_style_text_color(hint_, lv_color_hex(BadgeTheme::TEXT_MUTED), 0);
        lv_label_set_text(hint_, "A: start / react");
        lv_obj_align(hint_, LV_ALIGN_BOTTOM_MID, 0, -10);

        set_idle();
        render_best();
    }

    void on_exit() override
    {
        hal_lights_clear();
        hal_lights_show();
        title_ = big_ = sub_ = hint_ = nullptr;
    }

    void on_button(ButtonEvent ev) override
    {
        if (ev.kind != ButtonEventKind::Pressed || ev.button != Button::A) return;
        switch (state_) {
            case State::Idle:
            case State::Done:
                arm();
                break;
            case State::Armed:
                false_start();
                break;
            case State::Go:
                react();
                break;
        }
    }

    void on_home_confirmation_cancel(uint32_t paused_ms) override
    {
        go_us_ += static_cast<int64_t>(paused_ms) * 1000;
    }

    void on_tick() override
    {
        if (state_ != State::Armed) return;
        if (--wait_ticks_ <= 0) {
            state_ = State::Go;
            go_us_ = esp_timer_get_time();
            for (size_t i = 0; i < LED_COUNT; i++) hal_lights_set(i, 0, 255, 0);
            hal_lights_show();
            if (big_) {
                lv_label_set_text(big_, "GO!");
                lv_obj_set_style_text_color(big_, lv_color_hex(0x00ff00), 0);
            }
            if (sub_) lv_label_set_text(sub_, "Press A!");
        }
    }

private:
    enum class State { Idle, Armed, Go, Done };

    void set_leds(uint8_t r, uint8_t g, uint8_t b)
    {
        for (size_t i = 0; i < LED_COUNT; i++) hal_lights_set(i, r, g, b);
        hal_lights_show();
    }

    void set_idle()
    {
        state_ = State::Idle;
        set_leds(0, 0, 0);
        if (big_) {
            lv_label_set_text(big_, "Ready?");
            lv_obj_set_style_text_color(big_, lv_color_hex(BadgeTheme::TEXT), 0);
        }
        if (sub_) lv_label_set_text(sub_, "Green = go");
    }

    void render_best()
    {
        if (!hint_) return;
        if (best_ > 0) lv_label_set_text_fmt(hint_, "A: start   best %dms", best_);
        else lv_label_set_text(hint_, "A: start / react");
    }

    void arm()
    {
        state_ = State::Armed;
        // 1.0 - 3.5 s at 20 ms ticks.
        wait_ticks_ = 50 + (int)(esp_random() % 126);
        set_leds(255, 0, 0);
        if (big_) {
            lv_label_set_text(big_, "Wait...");
            lv_obj_set_style_text_color(big_, lv_color_hex(0xff0000), 0);
        }
        if (sub_) lv_label_set_text(sub_, "Wait for green");
    }

    void false_start()
    {
        state_ = State::Done;
        set_leds(255, 0, 0);
        if (big_) lv_label_set_text(big_, "TOO SOON");
        if (sub_) lv_label_set_text(sub_, "A to retry");
    }

    void react()
    {
        int ms = (int)((esp_timer_get_time() - go_us_) / 1000);
        state_ = State::Done;
        if (best_ == 0 || ms < best_) {
            best_ = ms;
            hal_config_set_int("reaction", "best_ms", best_);
            if (sub_) lv_label_set_text(sub_, "NEW BEST!");
            hal_lights_flash_success();
        } else {
            if (sub_) lv_label_set_text_fmt(sub_, "best %dms  A: retry", best_);
        }
        if (big_) {
            lv_label_set_text_fmt(big_, "%dms", ms);
            lv_obj_set_style_text_color(big_, lv_color_hex(BadgeTheme::TEXT), 0);
        }
        render_best();
    }

    lv_obj_t *title_ = nullptr;
    lv_obj_t *big_ = nullptr;
    lv_obj_t *sub_ = nullptr;
    lv_obj_t *hint_ = nullptr;
    State state_ = State::Idle;
    int wait_ticks_ = 0;
    int64_t go_us_ = 0;
    int best_ = 0;
};

static ReactionApp s_instance;
App *app_reaction_instance() { return &s_instance; }
