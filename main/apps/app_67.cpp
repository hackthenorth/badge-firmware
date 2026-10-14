#include "app.h"
#include "app_registry.h"
#include "hal_accel.h"
#include "hal_config.h"
#include "hal_lights.h"
#include "six_seven_motion.h"

#include <cstdint>

namespace {

constexpr uint32_t kRoundDurationMs = 67000;
constexpr uint32_t kCountdownPulseHalfPeriodMs = 350;
constexpr uint32_t kFinishFlashHalfPeriodMs = 200;
constexpr uint32_t kFinishFlashDurationMs = 2000;
constexpr uint32_t kChartSamplePeriodMs = 10;
constexpr uint32_t kChartPointCount = 48;
constexpr int32_t kChartRangeMg = 1600;

constexpr uint32_t kAxisXColor = 0xff5555;
constexpr uint32_t kAxisYColor = 0x55e080;
constexpr uint32_t kAxisZColor = 0x55aaff;

class SixSevenApp final : public GameApp {
public:
    const char *name() const override { return "67"; }
    const char *icon() const override { return "67"; }
    const char *slug() const override { return "67"; }
    AppAccess access() const override { return AppAccess::Public; }
    bool wants_wake_lock() const override { return true; }
    uint32_t tick_period_ms() const override { return 1; }

    void on_enter(lv_obj_t *root) override
    {
        sensor_fast_mode_ =
            hal_accel_set_output_rate(AccelOutputRate::Hz800) == ESP_OK;
        if (!sensor_fast_mode_) {
            hal_accel_set_output_rate(AccelOutputRate::Hz100);
        }
        detector_.reset();
        high_score_ = hal_config_get_int(slug(), "high_score", 0);
        if (high_score_ < 0) high_score_ = 0;
        new_high_score_ = false;
        state_ = RoundState::Ready;
        score_ = 0;
        shown_seconds_ = UINT32_MAX;
        shown_score_ = -1;
        sensor_read_attempted_ = false;
        sensor_ok_ = false;
        shown_sensor_read_attempted_ = false;
        shown_sensor_ok_ = false;
        last_led_count_ = -1;
        last_led_pulse_phase_ = UINT32_MAX;
        last_flash_phase_ = UINT32_MAX;
        last_chart_sample_ms_ = 0;
        chart_has_sample_ = false;

        hal_lights_clear();
        hal_lights_show();

        lv_obj_t *title = lv_label_create(root);
        lv_label_set_text(title, "6-7 COUNTER");
        lv_obj_set_style_text_color(title, lv_color_hex(BadgeTheme::TEXT), 0);
        lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
        lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 4);

        high_score_label_ = lv_label_create(root);
        lv_obj_set_style_text_color(high_score_label_,
                                    lv_color_hex(BadgeTheme::TEXT_MUTED), 0);
        lv_obj_set_style_text_font(high_score_label_,
                                   &lv_font_montserrat_14, 0);
        lv_obj_align(high_score_label_, LV_ALIGN_TOP_LEFT, 8, 8);

        time_label_ = lv_label_create(root);
        lv_obj_set_style_text_color(time_label_,
                                    lv_color_hex(BadgeTheme::TEXT_SOFT), 0);
        lv_obj_set_style_text_font(time_label_, &lv_font_montserrat_24, 0);
        lv_obj_align(time_label_, LV_ALIGN_TOP_MID, 0, 29);

        count_label_ = lv_label_create(root);
        lv_obj_set_style_text_color(count_label_,
                                    lv_color_hex(BadgeTheme::ACCENT_DETAIL), 0);
        lv_obj_set_style_text_font(count_label_, &lv_font_montserrat_24, 0);
        lv_obj_set_style_transform_scale(count_label_, 430, 0);
        lv_obj_set_style_transform_pivot_x(count_label_, LV_PCT(50), 0);
        lv_obj_set_style_transform_pivot_y(count_label_, LV_PCT(50), 0);

        status_label_ = lv_label_create(root);
        lv_obj_set_width(status_label_, 300);
        lv_obj_set_style_text_align(status_label_, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(status_label_,
                                    lv_color_hex(BadgeTheme::TEXT_MUTED), 0);
        lv_obj_set_style_text_font(status_label_, &lv_font_montserrat_14, 0);
        lv_obj_align(status_label_, LV_ALIGN_TOP_MID, 0, 109);

        chart_ = lv_chart_create(root);
        lv_obj_set_size(chart_, 300, 65);
        lv_obj_set_pos(chart_, 10, 141);
        lv_chart_set_type(chart_, LV_CHART_TYPE_LINE);
        lv_chart_set_point_count(chart_, kChartPointCount);
        lv_chart_set_axis_range(chart_, LV_CHART_AXIS_PRIMARY_Y,
                                -kChartRangeMg, kChartRangeMg);
        lv_chart_set_update_mode(chart_, LV_CHART_UPDATE_MODE_SHIFT);
        lv_chart_set_div_line_count(chart_, 3, 5);
        lv_obj_set_style_bg_color(chart_, lv_color_hex(BadgeTheme::PANEL), 0);
        lv_obj_set_style_bg_opa(chart_, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(chart_, lv_color_hex(BadgeTheme::BORDER), 0);
        lv_obj_set_style_border_width(chart_, 1, 0);
        lv_obj_set_style_radius(chart_, 4, 0);
        lv_obj_set_style_pad_all(chart_, 3, 0);
        lv_obj_set_style_line_color(chart_, lv_color_hex(BadgeTheme::BORDER), 0);
        lv_obj_set_style_line_opa(chart_, LV_OPA_50, 0);
        lv_obj_set_style_line_width(chart_, 2, LV_PART_ITEMS);
        lv_obj_set_style_size(chart_, 0, 0, LV_PART_INDICATOR);

        chart_x_ = lv_chart_add_series(chart_, lv_color_hex(kAxisXColor),
                                       LV_CHART_AXIS_PRIMARY_Y);
        chart_y_ = lv_chart_add_series(chart_, lv_color_hex(kAxisYColor),
                                       LV_CHART_AXIS_PRIMARY_Y);
        chart_z_ = lv_chart_add_series(chart_, lv_color_hex(kAxisZColor),
                                       LV_CHART_AXIS_PRIMARY_Y);

        chart_legend_ = lv_label_create(root);
        lv_label_set_recolor(chart_legend_, true);
        lv_label_set_text(chart_legend_,
                          "#ff5555 X +0#  #55e080 Y +0#  #55aaff Z +0# mg");
        lv_obj_set_style_text_color(chart_legend_,
                                    lv_color_hex(BadgeTheme::TEXT_DIM), 0);
        lv_obj_set_style_text_font(chart_legend_, &lv_font_montserrat_14, 0);
        lv_obj_align(chart_legend_, LV_ALIGN_TOP_MID, 0, 125);

        hint_label_ = lv_label_create(root);
        lv_obj_set_style_text_color(hint_label_,
                                    lv_color_hex(BadgeTheme::TEXT_DIM), 0);
        lv_obj_set_style_text_font(hint_label_, &lv_font_montserrat_14, 0);
        lv_obj_align(hint_label_, LV_ALIGN_BOTTOM_MID, 0, -8);

        render();
    }

    void on_exit() override
    {
        hal_accel_set_output_rate(AccelOutputRate::Hz100);
        hal_lights_clear();
        hal_lights_show();
        time_label_ = count_label_ = status_label_ = hint_label_ = nullptr;
        high_score_label_ = nullptr;
        chart_legend_ = chart_ = nullptr;
        chart_x_ = chart_y_ = chart_z_ = nullptr;
    }

    void on_button(ButtonEvent ev) override
    {
        if (ev.kind != ButtonEventKind::Pressed) return;
        if (ev.button != Button::A && ev.button != Button::Start) return;

        if (state_ == RoundState::Ready || state_ == RoundState::Finished) {
            start_round();
        }
    }

    void on_home_confirmation_cancel(uint32_t paused_ms) override
    {
        round_started_ms_ += paused_ms;
        finish_started_ms_ += paused_ms;
        last_chart_sample_ms_ += paused_ms;
        // Motion while the dialog was open must not complete a half-cycle.
        detector_.begin_cycle(lv_tick_get());
    }

    void checkpoint_progress() override
    {
        if (score_ <= high_score_) return;
        high_score_ = score_;
        new_high_score_ = true;
        shown_score_ = -1;
        hal_config_set_int(slug(), "high_score", high_score_);
    }

    void on_tick() override
    {
        const uint32_t now = lv_tick_get();
        switch (state_) {
        case RoundState::Ready:
            {
                const bool attempted = sensor_read_attempted_;
                const bool ok = sensor_ok_;
                sample_motion();
                if (attempted != sensor_read_attempted_ || ok != sensor_ok_) {
                    render();
                }
            }
            break;
        case RoundState::Running:
            tick_running(now);
            break;
        case RoundState::Finishing:
            sample_motion(false);
            tick_finish_flash(lv_tick_get());
            break;
        case RoundState::Finished:
            sample_motion(false);
            break;
        }
    }

private:
    enum class RoundState : uint8_t { Ready, Running, Finishing, Finished };

    bool sample_motion(bool detect = true)
    {
        AccelSample sample;
        sensor_read_attempted_ = true;
        if (hal_accel_read(&sample) != ESP_OK) {
            sensor_ok_ = false;
            return false;
        }
        sensor_ok_ = true;
        const uint32_t now = lv_tick_get();
        append_chart_sample(sample, now);
        const bool counted =
            detector_.update(sample.x_mg, sample.y_mg, sample.z_mg, now);
        return detect && counted;
    }

    void append_chart_sample(const AccelSample &sample, uint32_t now)
    {
        if (!chart_ || !chart_x_ || !chart_y_ || !chart_z_) return;
        if (chart_has_sample_
            && now - last_chart_sample_ms_ < kChartSamplePeriodMs) return;

        chart_has_sample_ = true;
        last_chart_sample_ms_ = now;
        lv_chart_set_next_value(chart_, chart_x_,
                                static_cast<int32_t>(sample.x_mg));
        lv_chart_set_next_value(chart_, chart_y_,
                                static_cast<int32_t>(sample.y_mg));
        lv_chart_set_next_value(chart_, chart_z_,
                                static_cast<int32_t>(sample.z_mg));
        if (chart_legend_) {
            lv_label_set_text_fmt(chart_legend_,
                "#ff5555 X %+.0f#  #55e080 Y %+.0f#  #55aaff Z %+.0f# mg",
                static_cast<double>(sample.x_mg),
                static_cast<double>(sample.y_mg),
                static_cast<double>(sample.z_mg));
            lv_obj_align(chart_legend_, LV_ALIGN_TOP_MID, 0, 125);
        }
    }

    void start_round()
    {
        state_ = RoundState::Running;
        score_ = 0;
        new_high_score_ = false;
        round_started_ms_ = lv_tick_get();
        detector_.begin_cycle(round_started_ms_);
        shown_seconds_ = UINT32_MAX;
        shown_score_ = -1;
        last_led_count_ = -1;
        last_led_pulse_phase_ = UINT32_MAX;
        update_countdown_leds(kRoundDurationMs, round_started_ms_);
        render();
    }

    uint32_t remaining_ms(uint32_t now) const
    {
        const uint32_t elapsed = now - round_started_ms_;
        return elapsed >= kRoundDurationMs ? 0 : kRoundDurationMs - elapsed;
    }

    static uint32_t seconds_for_display(uint32_t remaining)
    {
        return (remaining + 999u) / 1000u;
    }

    void tick_running(uint32_t now)
    {
        const uint32_t remaining = remaining_ms(now);
        if (remaining == 0) {
            finish_round(now);
            return;
        }

        if (sample_motion()) score_++;
        now = lv_tick_get();
        const uint32_t updated_remaining = remaining_ms(now);
        if (updated_remaining == 0) {
            finish_round(now);
            return;
        }
        update_countdown_leds(updated_remaining, now);

        const uint32_t seconds = seconds_for_display(updated_remaining);
        if (score_ != shown_score_ || seconds != shown_seconds_
            || sensor_read_attempted_ != shown_sensor_read_attempted_
            || sensor_ok_ != shown_sensor_ok_) {
            render();
        }
    }

    void update_countdown_leds(uint32_t remaining, uint32_t now)
    {
        const int lit = static_cast<int>(
            (static_cast<uint64_t>(remaining) * LED_COUNT
             + kRoundDurationMs - 1u) / kRoundDurationMs);
        const uint32_t pulse_phase =
            (now - round_started_ms_) / kCountdownPulseHalfPeriodMs;
        if (lit == last_led_count_ && pulse_phase == last_led_pulse_phase_) return;
        last_led_count_ = lit;
        last_led_pulse_phase_ = pulse_phase;
        const bool bright = (pulse_phase % 2u) == 0u;

        const int extinguished = static_cast<int>(LED_COUNT) - lit;
        for (size_t step = 0; step < LED_COUNT; ++step) {
            const size_t led = BadgeLed::ClockwiseFromUpperRight[step];
            if (static_cast<int>(step) >= extinguished) {
                hal_lights_set(led, 0, bright ? 255 : 24, bright ? 64 : 6);
            } else {
                hal_lights_set(led, 0, 0, 0);
            }
        }
        hal_lights_show();
    }

    void finish_round(uint32_t now)
    {
        update_countdown_leds(0, now);
        checkpoint_progress();
        state_ = RoundState::Finishing;
        finish_started_ms_ = now;
        last_flash_phase_ = UINT32_MAX;
        render();
        tick_finish_flash(now);
    }

    void set_finish_leds(bool on)
    {
        for (size_t i = 0; i < LED_COUNT; ++i) {
            hal_lights_set(i, on ? 255 : 0, 0, 0);
        }
        hal_lights_show();
    }

    void tick_finish_flash(uint32_t now)
    {
        const uint32_t elapsed = now - finish_started_ms_;
        if (elapsed >= kFinishFlashDurationMs) {
            set_finish_leds(false);
            state_ = RoundState::Finished;
            render();
            return;
        }

        const uint32_t phase = elapsed / kFinishFlashHalfPeriodMs;
        if (phase != last_flash_phase_) {
            last_flash_phase_ = phase;
            set_finish_leds((phase % 2u) == 0u);
        }
    }

    void render()
    {
        uint32_t seconds = 67;
        if (state_ == RoundState::Running) {
            seconds = seconds_for_display(remaining_ms(lv_tick_get()));
        } else if (state_ == RoundState::Finishing) {
            seconds = 0;
        }

        if (time_label_) {
            if (state_ == RoundState::Finished) {
                lv_label_set_text(time_label_, "67 SECONDS");
            } else {
                lv_label_set_text_fmt(time_label_, "%u SEC",
                                      static_cast<unsigned>(seconds));
            }
        }
        if (high_score_label_) {
            lv_label_set_text_fmt(high_score_label_, "BEST %d", high_score_);
            lv_obj_align(high_score_label_, LV_ALIGN_TOP_LEFT, 8, 8);
        }
        if (count_label_) {
            lv_label_set_text_fmt(count_label_, "%d", score_);
            lv_obj_set_style_text_color(count_label_,
                lv_color_hex(state_ == RoundState::Finishing
                    ? 0xff4444 : BadgeTheme::ACCENT_DETAIL), 0);
            // Transform scaling does not affect layout bounds, so realign
            // after every width-changing score update.
            lv_obj_align(count_label_, LV_ALIGN_TOP_MID, 0, 69);
        }
        if (status_label_) {
            switch (state_) {
            case RoundState::Ready:
                if (!sensor_read_attempted_) {
                    lv_label_set_text(status_label_, "CHECKING ACCEL...");
                } else if (!sensor_ok_) {
                    lv_label_set_text(status_label_, "ACCEL ERROR");
                } else {
                    lv_label_set_text(status_label_, sensor_fast_mode_
                        ? "ACCEL READY / 800 HZ"
                        : "ACCEL READY / 100 HZ");
                }
                break;
            case RoundState::Running:
                if (!sensor_ok_) {
                    lv_label_set_text(status_label_, "ACCEL ERROR");
                    break;
                }
                lv_label_set_text(status_label_, "SHAKE UP / DOWN");
                break;
            case RoundState::Finishing:
                lv_label_set_text(status_label_, new_high_score_
                    ? "NEW BEST!" : "TIME!");
                break;
            case RoundState::Finished:
                lv_label_set_text(status_label_, new_high_score_
                    ? "NEW BEST!" : "FINAL COUNT");
                break;
            }
        }
        if (hint_label_) {
            lv_label_set_text(hint_label_, state_ == RoundState::Finished
                ? "A AGAIN     HOME APPS" : state_ == RoundState::Ready
                ? "A START     HOME APPS" : "HOME APPS");
            lv_obj_align(hint_label_, LV_ALIGN_BOTTOM_MID, 0, -8);
        }

        shown_seconds_ = seconds;
        shown_score_ = score_;
        shown_sensor_read_attempted_ = sensor_read_attempted_;
        shown_sensor_ok_ = sensor_ok_;
    }

    SixSevenMotionDetector detector_;
    RoundState state_ = RoundState::Ready;
    lv_obj_t *time_label_ = nullptr;
    lv_obj_t *count_label_ = nullptr;
    lv_obj_t *status_label_ = nullptr;
    lv_obj_t *hint_label_ = nullptr;
    lv_obj_t *high_score_label_ = nullptr;
    lv_obj_t *chart_ = nullptr;
    lv_obj_t *chart_legend_ = nullptr;
    lv_chart_series_t *chart_x_ = nullptr;
    lv_chart_series_t *chart_y_ = nullptr;
    lv_chart_series_t *chart_z_ = nullptr;
    uint32_t round_started_ms_ = 0;
    uint32_t finish_started_ms_ = 0;
    uint32_t last_chart_sample_ms_ = 0;
    uint32_t shown_seconds_ = UINT32_MAX;
    uint32_t last_flash_phase_ = UINT32_MAX;
    uint32_t last_led_pulse_phase_ = UINT32_MAX;
    int score_ = 0;
    int high_score_ = 0;
    int shown_score_ = -1;
    int last_led_count_ = -1;
    bool sensor_read_attempted_ = false;
    bool sensor_ok_ = false;
    bool shown_sensor_read_attempted_ = false;
    bool shown_sensor_ok_ = false;
    bool chart_has_sample_ = false;
    bool sensor_fast_mode_ = false;
    bool new_high_score_ = false;
};

SixSevenApp s_instance;

} // namespace

App *app_67_instance() { return &s_instance; }
