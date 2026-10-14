#include "app.h"
#include "app_registry.h"
#include "hal_lights.h"
#include "hal_config.h"

#include "lvgl.h"
#include <cmath>
#include <cstdio>

extern "C" const lv_image_dsc_t icon_pomodoro;

class PomodoroApp : public App {
public:
    const char *name() const override { return "Pomodoro"; }
    const char *icon() const override { return "TMT"; }
    const char *slug() const override { return "pomodoro"; }
    const lv_image_dsc_t *icon_image() const override { return &icon_pomodoro; }

    void on_enter(lv_obj_t *root) override
    {
        work_min_  = hal_config_get_int("pomodoro", "work_min", 25);
        short_min_ = hal_config_get_int("pomodoro", "short_min", 5);
        long_min_  = hal_config_get_int("pomodoro", "long_min", 15);
        phase_ = Phase::Idle;
        cycle_ = 0;
        paused_ = true;
        elapsed_ms_ = 0;
        last_tick_ = lv_tick_get();

        lv_obj_t *title = lv_label_create(root);
        lv_label_set_text(title, "Pomodoro");
        lv_obj_set_style_text_color(title, lv_color_hex(BadgeTheme::TEXT), 0);
        lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
        lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 6);

        phase_label_ = lv_label_create(root);
        lv_obj_set_style_text_font(phase_label_, &lv_font_montserrat_18, 0);
        lv_obj_align(phase_label_, LV_ALIGN_TOP_MID, 0, 36);

        big_ = lv_label_create(root);
        lv_obj_set_style_text_font(big_, &lv_font_montserrat_24, 0);
        lv_obj_set_style_text_color(big_, lv_color_hex(BadgeTheme::TEXT), 0);
        lv_obj_align(big_, LV_ALIGN_CENTER, 0, -5);

        bar_ = lv_bar_create(root);
        lv_obj_set_size(bar_, 260, 10);
        lv_obj_align(bar_, LV_ALIGN_CENTER, 0, 30);
        lv_bar_set_range(bar_, 0, 1000);
        lv_obj_set_style_bg_color(bar_, lv_color_hex(BadgeTheme::SURFACE), LV_PART_MAIN);
        lv_obj_set_style_bg_color(bar_, lv_color_hex(BadgeTheme::ACCENT_DETAIL), LV_PART_INDICATOR);

        cycle_label_ = lv_label_create(root);
        lv_obj_set_style_text_font(cycle_label_, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(cycle_label_, lv_color_hex(BadgeTheme::TEXT_MUTED), 0);
        lv_obj_align(cycle_label_, LV_ALIGN_CENTER, 0, 52);

        hint_ = lv_label_create(root);
        lv_obj_set_style_text_color(hint_, lv_color_hex(BadgeTheme::TEXT_MUTED), 0);
        lv_label_set_text(hint_, "A: start/pause   U/D: len   START: reset");
        lv_obj_align(hint_, LV_ALIGN_BOTTOM_MID, 0, -6);

        render();
    }

    void on_exit() override
    {
        hal_lights_clear();
        hal_lights_show();
        phase_label_ = big_ = bar_ = cycle_label_ = hint_ = nullptr;
    }

    void on_button(ButtonEvent ev) override
    {
        if (ev.kind != ButtonEventKind::Pressed) return;
        switch (ev.button) {
            case Button::A:
                if (phase_ == Phase::Idle) {
                    phase_ = Phase::Work;
                    elapsed_ms_ = 0;
                    last_tick_ = lv_tick_get();
                    paused_ = false;
                } else {
                    paused_ = !paused_;
                    last_tick_ = lv_tick_get();
                }
                break;
            case Button::Up:
                if (phase_ == Phase::Idle) {
                    if (work_min_ < 90) work_min_++;
                    hal_config_set_int("pomodoro", "work_min", work_min_);
                }
                break;
            case Button::Down:
                if (phase_ == Phase::Idle) {
                    if (work_min_ > 5) work_min_--;
                    hal_config_set_int("pomodoro", "work_min", work_min_);
                }
                break;
            case Button::Start:
                phase_ = Phase::Idle;
                elapsed_ms_ = 0;
                cycle_ = 0;
                paused_ = true;
                hal_lights_clear();
                hal_lights_show();
                break;
            default: break;
        }
        render();
    }

    void on_tick() override
    {
        uint32_t now = lv_tick_get();
        uint32_t delta = now - last_tick_;
        last_tick_ = now;

        if (phase_ != Phase::Idle && !paused_) {
            elapsed_ms_ += delta;
            uint32_t phase_total_ms = current_phase_ms();
            if (elapsed_ms_ >= phase_total_ms) {
                advance_phase();
                return;
            }
            update_leds(phase_total_ms);
        }
        render();
    }

private:
    enum class Phase { Idle, Work, ShortBreak, LongBreak };

    uint32_t current_phase_ms() const
    {
        switch (phase_) {
            case Phase::Work:       return (uint32_t)work_min_ * 60u * 1000u;
            case Phase::ShortBreak: return (uint32_t)short_min_ * 60u * 1000u;
            case Phase::LongBreak:  return (uint32_t)long_min_ * 60u * 1000u;
            default: return 1;
        }
    }

    void advance_phase()
    {
        if (phase_ == Phase::Work) {
            cycle_++;
            phase_ = (cycle_ % 4 == 0) ? Phase::LongBreak : Phase::ShortBreak;
        } else {
            phase_ = Phase::Work;
        }
        elapsed_ms_ = 0;
        // Signal phase change with a brief full flash.
        uint8_t c = 80;
        uint32_t col = (phase_ == Phase::Work) ? 0xff3030 : 0x30ff30;
        for (int i = 0; i < (int)LED_COUNT; i++) {
            hal_lights_set(i, (col >> 16) & 0xff, (col >> 8) & 0xff, col & 0xff);
        }
        (void)c;
        hal_lights_show();
    }

    void update_leds(uint32_t total_ms)
    {
        // Pulse LEDs only in the last 60 s of a phase.
        uint32_t remaining = total_ms - elapsed_ms_;
        if (remaining > 60000) {
            hal_lights_clear();
            hal_lights_show();
            return;
        }
        float t = (float)elapsed_ms_ / 1000.0f;
        float pulse = (sinf(t * 3.0f) + 1.0f) * 0.5f;
        uint8_t v = (uint8_t)(pulse * 60.0f);
        bool work = (phase_ == Phase::Work);
        for (int i = 0; i < (int)LED_COUNT; i++)
            hal_lights_set(i, work ? v : 0, work ? 0 : v, 0);
        hal_lights_show();
    }

    void render()
    {
        const char *names[] = { "Idle", "Work", "Short Break", "Long Break" };
        if (phase_label_) {
            lv_label_set_text(phase_label_, names[(int)phase_]);
            uint32_t col = phase_ == Phase::Work ? 0xff5c8a
                          : phase_ == Phase::Idle ? BadgeTheme::TEXT_MUTED : 0x00ffa8;
            lv_obj_set_style_text_color(phase_label_, lv_color_hex(col), 0);
        }
        if (big_) {
            uint32_t total_ms = current_phase_ms();
            uint32_t shown = (phase_ == Phase::Idle) ? (uint32_t)work_min_ * 60u * 1000u
                                                     : (total_ms - elapsed_ms_);
            uint32_t s = shown / 1000;
            lv_label_set_text_fmt(big_, "%02u:%02u", (unsigned)(s / 60), (unsigned)(s % 60));
        }
        if (bar_) {
            uint32_t total_ms = current_phase_ms();
            int v = phase_ == Phase::Idle ? 0 : (int)((uint64_t)elapsed_ms_ * 1000 / total_ms);
            lv_bar_set_value(bar_, v, LV_ANIM_OFF);
        }
        if (cycle_label_) {
            const char *st = paused_ ? "paused" : (phase_ == Phase::Idle ? "ready" : "running");
            lv_label_set_text_fmt(cycle_label_, "cycle %d   %s", cycle_, st);
        }
    }

    lv_obj_t *phase_label_ = nullptr;
    lv_obj_t *big_ = nullptr;
    lv_obj_t *bar_ = nullptr;
    lv_obj_t *cycle_label_ = nullptr;
    lv_obj_t *hint_ = nullptr;

    int work_min_ = 25;
    int short_min_ = 5;
    int long_min_ = 15;
    Phase phase_ = Phase::Idle;
    int cycle_ = 0;
    uint32_t elapsed_ms_ = 0;
    uint32_t last_tick_ = 0;
    bool paused_ = true;
};

static PomodoroApp s_instance;
App *app_pomodoro_instance() { return &s_instance; }
