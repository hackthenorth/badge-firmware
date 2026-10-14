#include "app.h"
#include "hal_lights.h"

#include <cstddef>
#include <cstdint>

namespace {

struct Mood {
    const char *name;
    const char *subtitle;
    uint32_t ui_color;
    uint8_t red;
    uint8_t green;
    uint8_t blue;
};

constexpr Mood kMoods[] = {
    {"Looking for a team", "Come build with me", 0x22e6b2, 0x10, 0xff, 0xa8},
    {"I'm locked in", "Deep focus mode", 0xff4f5e, 0xff, 0x12, 0x20},
    {"Come talk to me", "Say hello!", 0x4da3ff, 0x18, 0x62, 0xff},
};

constexpr size_t kMoodCount = sizeof(kMoods) / sizeof(kMoods[0]);
constexpr int kPulseOnTicks = 100;   // 2 seconds at the 50 Hz app tick
constexpr int kPulseOffTicks = 100;  // 50% duty cycle
constexpr int kPulseRiseTicks = kPulseOnTicks / 2;
constexpr int kPulseCycleTicks = kPulseOnTicks + kPulseOffTicks;
constexpr uint8_t kMoodPeakLogical = 180; // below the shared full-brightness cap

extern "C" const lv_image_dsc_t icon_mood;

class MoodApp final : public App {
public:
    const char *name() const override { return "Mood LEDs"; }
    const char *icon() const override { return "RGB"; }
    const lv_image_dsc_t *icon_image() const override { return &icon_mood; }
    const char *slug() const override { return "mood"; }
    AppAccess access() const override { return AppAccess::Default; }
    bool wants_wake_lock() const override { return true; }

    void on_enter(lv_obj_t *root) override
    {
        root_ = root;
        selected_ = 0;
        pulse_tick_ = 0;
        pulse_level_ = 0;
        lv_obj_set_style_bg_color(root_, lv_color_hex(BadgeTheme::BACKGROUND), 0);
        lv_obj_set_style_bg_opa(root_, LV_OPA_COVER, 0);

        lv_obj_t *title = lv_label_create(root_);
        lv_label_set_text(title, "Mood LEDs");
        lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(title, lv_color_hex(BadgeTheme::TEXT), 0);
        lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 5);

        for (size_t i = 0; i < kMoodCount; ++i) {
            cards_[i] = lv_obj_create(root_);
            lv_obj_set_size(cards_[i], 296, 53);
            lv_obj_set_pos(cards_[i], 12, 34 + static_cast<int>(i) * 57);
            lv_obj_set_style_radius(cards_[i], 10, 0);
            lv_obj_set_style_bg_color(cards_[i], lv_color_hex(0x111923), 0);
            lv_obj_set_style_border_width(cards_[i], 2, 0);
            lv_obj_set_style_pad_all(cards_[i], 0, 0);
            lv_obj_clear_flag(cards_[i], LV_OBJ_FLAG_SCROLLABLE);

            dots_[i] = lv_obj_create(cards_[i]);
            lv_obj_remove_style_all(dots_[i]);
            lv_obj_set_size(dots_[i], 26, 26);
            lv_obj_set_style_radius(dots_[i], LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_bg_opa(dots_[i], LV_OPA_COVER, 0);
            lv_obj_set_style_bg_color(dots_[i], lv_color_hex(kMoods[i].ui_color), 0);
            lv_obj_align(dots_[i], LV_ALIGN_LEFT_MID, 12, 0);

            names_[i] = lv_label_create(cards_[i]);
            lv_label_set_text(names_[i], kMoods[i].name);
            lv_obj_set_style_text_font(names_[i], &lv_font_montserrat_14, 0);
            lv_obj_set_style_text_color(names_[i], lv_color_hex(BadgeTheme::TEXT), 0);
            lv_obj_align(names_[i], LV_ALIGN_TOP_LEFT, 48, 8);

            lv_obj_t *subtitle = lv_label_create(cards_[i]);
            lv_label_set_text(subtitle, kMoods[i].subtitle);
            lv_obj_set_style_text_font(subtitle, &lv_font_montserrat_12, 0);
            lv_obj_set_style_text_color(subtitle, lv_color_hex(BadgeTheme::TEXT_DIM), 0);
            lv_obj_align(subtitle, LV_ALIGN_TOP_LEFT, 48, 28);
        }

        hint_ = lv_label_create(root_);
        lv_obj_set_style_text_font(hint_, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(hint_, lv_color_hex(BadgeTheme::TEXT_DIM), 0);
        lv_obj_align(hint_, LV_ALIGN_BOTTOM_MID, 0, -7);

        apply_selected();
        show_intro();
    }

    void on_exit() override
    {
        hal_lights_clear();
        hal_lights_show();
        root_ = hint_ = nullptr;
        intro_ = nullptr;
        for (lv_obj_t *&card : cards_) card = nullptr;
        for (lv_obj_t *&dot : dots_) dot = nullptr;
        for (lv_obj_t *&name : names_) name = nullptr;
    }

    void on_tick() override
    {
        pulse_tick_ = (pulse_tick_ + 1) % kPulseCycleTicks;
        render_pulse(false);
    }

    void on_button(ButtonEvent ev) override
    {
        if (ev.kind != ButtonEventKind::Pressed) return;

        if (intro_) {
            if (ev.button == Button::A) {
                lv_obj_delete(intro_);
                intro_ = nullptr;
                apply_selected();
            }
            return;
        }
        if (ev.button == Button::Start) {
            show_intro();
            return;
        }

        size_t next = selected_;
        if (ev.button == Button::Up || ev.button == Button::Left) {
            next = (selected_ + kMoodCount - 1) % kMoodCount;
        } else if (ev.button == Button::Down || ev.button == Button::Right
                   || ev.button == Button::A) {
            next = (selected_ + 1) % kMoodCount;
        } else {
            return;
        }

        selected_ = next;
        apply_selected();
    }

private:
    void show_intro()
    {
        intro_ = lv_obj_create(root_);
        lv_obj_remove_style_all(intro_);
        lv_obj_set_pos(intro_, 8, 34);
        lv_obj_set_size(intro_, 304, 174);
        lv_obj_set_style_bg_color(intro_, lv_color_hex(BadgeTheme::BACKGROUND), 0);
        lv_obj_set_style_bg_opa(intro_, LV_OPA_COVER, 0);
        lv_obj_clear_flag(intro_, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *copy = lv_label_create(intro_);
        lv_label_set_text(copy,
            "Choose a mood, then leave this app\n"
            "open and wear your badge around\n"
            "the event to show how you feel.");
        lv_obj_set_width(copy, 304);
        lv_obj_set_style_text_font(copy, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_align(copy, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(copy, lv_color_hex(BadgeTheme::TEXT_SOFT), 0);
        lv_obj_set_pos(copy, 0, 8);

        lv_obj_t *team = lv_label_create(intro_);
        lv_label_set_text(team,
            "Green means: looking for a team!\n"
            "See green LEDs? Go say hi!");
        lv_obj_set_width(team, 304);
        lv_obj_set_style_text_font(team, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_align(team, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(team, lv_color_hex(kMoods[0].ui_color), 0);
        lv_obj_set_pos(team, 0, 84);

        lv_obj_t *cta = lv_label_create(intro_);
        lv_label_set_text(cta, "Press A to choose your mood");
        lv_obj_set_style_text_font(cta, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(cta, lv_color_hex(BadgeTheme::TEXT), 0);
        lv_obj_align(cta, LV_ALIGN_BOTTOM_MID, 0, -8);
        lv_label_set_text(hint_, "HOME: apps");
    }

    void apply_selected()
    {
        render_pulse(true);

        for (size_t i = 0; i < kMoodCount; ++i) {
            const bool active = i == selected_;
            lv_obj_set_style_border_color(cards_[i],
                lv_color_hex(active ? kMoods[i].ui_color : 0x283345), 0);
            lv_obj_set_style_bg_color(cards_[i],
                lv_color_hex(active ? 0x18252b : 0x111923), 0);
            lv_obj_set_style_opa(dots_[i], active ? LV_OPA_COVER : LV_OPA_60, 0);
        }

        lv_label_set_text(hint_, "UP / DOWN: mood   START: info");
    }

    void render_pulse(bool force)
    {
        uint8_t level = 0;
        if (pulse_tick_ < kPulseOnTicks) {
            const int ramp = pulse_tick_ < kPulseRiseTicks
                ? pulse_tick_ + 1
                : kPulseOnTicks - pulse_tick_;
            level = static_cast<uint8_t>(
                (ramp * kMoodPeakLogical) / kPulseRiseTicks);
        }
        if (!force && level == pulse_level_) return;
        pulse_level_ = level;

        const Mood &mood = kMoods[selected_];
        const auto scale = [level](uint8_t channel) {
            return static_cast<uint8_t>(
                (static_cast<unsigned>(channel) * level + 127u) / 255u);
        };
        for (size_t i = 0; i < LED_COUNT; ++i) {
            hal_lights_set(i, scale(mood.red), scale(mood.green),
                           scale(mood.blue));
        }
        hal_lights_show();
    }

    lv_obj_t *root_ = nullptr;
    lv_obj_t *hint_ = nullptr;
    lv_obj_t *intro_ = nullptr;
    lv_obj_t *cards_[kMoodCount] = {};
    lv_obj_t *dots_[kMoodCount] = {};
    lv_obj_t *names_[kMoodCount] = {};
    size_t selected_ = 0;
    int pulse_tick_ = 0;
    uint8_t pulse_level_ = 0;
};

MoodApp s_instance;

} // namespace

App *app_mood_instance() { return &s_instance; }
