#include "app.h"
#include "app_registry.h"
#include "hal_config.h"
#include "hal_lights.h"

namespace {

struct Rgb {
    uint8_t r;
    uint8_t g;
    uint8_t b;
};

// Deliberately broad endpoints: the middle is true neutral white, while the
// extremes are visibly blue and orange instead of subtle white-balance tints.
constexpr Rgb kCool = {80, 170, 255};
constexpr Rgb kNeutral = {255, 255, 255};
constexpr Rgb kWarm = {255, 96, 24};
constexpr int kNeutralTemp = 50;
constexpr int kMaxTemp = 100;
constexpr int kTempStep = 5;
constexpr int kBrightnessStep = 32;
constexpr int kMaxBrightness = 255;
constexpr int kDefaultBrightness = (kMaxBrightness + 1) / 2;

Rgb interpolate(Rgb from, Rgb to, int value, int range)
{
    const auto channel = [value, range](uint8_t start, uint8_t end) {
        const int delta = static_cast<int>(end) - static_cast<int>(start);
        return static_cast<uint8_t>(static_cast<int>(start)
            + (delta * value + (delta >= 0 ? range / 2 : -range / 2)) / range);
    };
    return {channel(from.r, to.r), channel(from.g, to.g), channel(from.b, to.b)};
}

Rgb temperature_color(int temperature)
{
    if (temperature <= kNeutralTemp) {
        return interpolate(kCool, kNeutral, temperature, kNeutralTemp);
    }
    return interpolate(kNeutral, kWarm, temperature - kNeutralTemp,
                       kMaxTemp - kNeutralTemp);
}

const char *temperature_name(int temperature)
{
    if (temperature < 20) return "VERY COOL";
    if (temperature < 45) return "COOL";
    if (temperature <= 55) return "NEUTRAL";
    if (temperature <= 80) return "WARM";
    return "VERY WARM";
}

lv_obj_t *make_label(lv_obj_t *root, const char *text, uint32_t color,
                     const lv_font_t *font, int x, int y)
{
    lv_obj_t *label = lv_label_create(root);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_pos(label, x, y);
    return label;
}

void style_slider(lv_obj_t *slider, uint32_t track, uint32_t indicator)
{
    lv_obj_remove_style_all(slider);
    lv_obj_set_style_radius(slider, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(slider, lv_color_hex(track), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(slider, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider, lv_color_hex(indicator), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(slider, LV_RADIUS_CIRCLE, LV_PART_KNOB);
    lv_obj_set_style_bg_color(slider, lv_color_hex(BadgeTheme::TEXT), LV_PART_KNOB);
    lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_border_width(slider, 2, LV_PART_KNOB);
    lv_obj_set_style_border_color(slider, lv_color_hex(BadgeTheme::BACKGROUND),
                                  LV_PART_KNOB);
    lv_obj_set_style_pad_all(slider, 5, LV_PART_KNOB);
    lv_obj_remove_flag(slider, LV_OBJ_FLAG_CLICKABLE);
}

void make_temperature_segment(lv_obj_t *root, int x, uint32_t from, uint32_t to)
{
    lv_obj_t *segment = lv_obj_create(root);
    lv_obj_remove_style_all(segment);
    lv_obj_set_size(segment, 132, 8);
    lv_obj_set_pos(segment, x, 139);
    lv_obj_set_style_radius(segment, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(segment, lv_color_hex(from), 0);
    lv_obj_set_style_bg_grad_color(segment, lv_color_hex(to), 0);
    lv_obj_set_style_bg_grad_dir(segment, LV_GRAD_DIR_HOR, 0);
    lv_obj_set_style_bg_opa(segment, LV_OPA_COVER, 0);
    lv_obj_clear_flag(segment, LV_OBJ_FLAG_SCROLLABLE);
}

extern "C" const lv_image_dsc_t icon_flashlight;

class FlashlightApp final : public App {
public:
    const char *name() const override { return "Flashlight"; }
    const char *icon() const override { return "FL"; }
    const char *slug() const override { return "flashlight"; }
    AppAccess access() const override { return AppAccess::Internal; }
    const lv_image_dsc_t *icon_image() const override { return &icon_flashlight; }
    bool icon_needs_outline() const override { return true; }

    void on_enter(lv_obj_t *root) override
    {
        brightness_ = hal_config_get_int("flashlight", "brightness",
                                         kDefaultBrightness);
        if (brightness_ < 0) brightness_ = 0;
        if (brightness_ > kMaxBrightness) brightness_ = kMaxBrightness;

        temp_ = hal_config_get_int("flashlight", "temperature", -1);
        if (temp_ < 0) {
            // Migrate the previous three-position cool/neutral/warm setting.
            const int old_temp = hal_config_get_int("flashlight", "temp", 1);
            temp_ = old_temp <= 0 ? 0 : old_temp == 1 ? kNeutralTemp : kMaxTemp;
        }
        if (temp_ > kMaxTemp) temp_ = kMaxTemp;
        enabled_ = true;

        lv_obj_t *title = lv_label_create(root);
        lv_label_set_text(title, "FLASHLIGHT");
        lv_obj_set_style_text_color(title, lv_color_hex(BadgeTheme::TEXT), 0);
        lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
        lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 5);

        power_label_ = lv_label_create(root);
        lv_obj_set_style_text_font(power_label_, &lv_font_montserrat_16, 0);
        lv_obj_align(power_label_, LV_ALIGN_TOP_MID, 0, 32);

        brightness_label_ = make_label(root, "", BadgeTheme::TEXT_SOFT,
                                       &lv_font_montserrat_14, 30, 61);
        brightness_slider_ = lv_slider_create(root);
        style_slider(brightness_slider_, BadgeTheme::SURFACE, BadgeTheme::TEXT);
        lv_obj_set_size(brightness_slider_, 260, 8);
        lv_obj_set_pos(brightness_slider_, 30, 88);
        lv_slider_set_range(brightness_slider_, 0, kMaxBrightness);

        temp_label_ = make_label(root, "", BadgeTheme::TEXT_SOFT,
                                 &lv_font_montserrat_14, 30, 112);
        make_temperature_segment(root, 30, 0x50aaff, 0xffffff);
        make_temperature_segment(root, 158, 0xffffff, 0xff6018);

        temp_slider_ = lv_slider_create(root);
        style_slider(temp_slider_, BadgeTheme::SURFACE, BadgeTheme::TEXT);
        lv_obj_set_size(temp_slider_, 260, 8);
        lv_obj_set_pos(temp_slider_, 30, 139);
        lv_slider_set_range(temp_slider_, 0, kMaxTemp);
        lv_obj_set_style_bg_opa(temp_slider_, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(temp_slider_, LV_OPA_TRANSP, LV_PART_INDICATOR);

        make_label(root, "COOL", BadgeTheme::TEXT_MUTED,
                   &lv_font_montserrat_14, 30, 155);
        lv_obj_t *neutral = make_label(root, "NEUTRAL", BadgeTheme::TEXT_MUTED,
                                       &lv_font_montserrat_14, 0, 155);
        lv_obj_align(neutral, LV_ALIGN_TOP_MID, 0, 155);
        lv_obj_t *warm = make_label(root, "WARM", BadgeTheme::TEXT_MUTED,
                                    &lv_font_montserrat_14, 0, 155);
        lv_obj_align(warm, LV_ALIGN_TOP_RIGHT, -30, 155);

        lv_obj_t *controls = lv_label_create(root);
        lv_label_set_text(controls, "U/D BRIGHTNESS   LEFT/RIGHT TEMP");
        lv_obj_set_style_text_color(controls, lv_color_hex(BadgeTheme::TEXT_MUTED), 0);
        lv_obj_set_style_text_font(controls, &lv_font_montserrat_14, 0);
        lv_obj_align(controls, LV_ALIGN_BOTTOM_MID, 0, -25);

        lv_obj_t *power_hint = lv_label_create(root);
        lv_label_set_text(power_hint, "A TURN ON/OFF");
        lv_obj_set_style_text_color(power_hint, lv_color_hex(BadgeTheme::TEXT_DIM), 0);
        lv_obj_set_style_text_font(power_hint, &lv_font_montserrat_14, 0);
        lv_obj_align(power_hint, LV_ALIGN_BOTTOM_MID, 0, -6);

        apply();
        render();
    }

    void on_exit() override
    {
        hal_lights_clear();
        hal_lights_show();
        hal_config_set_int("flashlight", "brightness", brightness_);
        hal_config_set_int("flashlight", "temperature", temp_);
        power_label_ = brightness_label_ = temp_label_ = nullptr;
        brightness_slider_ = temp_slider_ = nullptr;
    }

    void on_button(ButtonEvent ev) override
    {
        if (ev.kind != ButtonEventKind::Pressed) return;
        const int previous_brightness = brightness_;
        const int previous_temp = temp_;
        switch (ev.button) {
        case Button::Up:
            brightness_ = brightness_ + kBrightnessStep > kMaxBrightness
                ? kMaxBrightness : brightness_ + kBrightnessStep;
            break;
        case Button::Down:
            brightness_ = brightness_ < kBrightnessStep
                ? 0 : brightness_ - kBrightnessStep;
            break;
        case Button::Left:
            temp_ = temp_ < kTempStep ? 0 : temp_ - kTempStep;
            break;
        case Button::Right:
            temp_ = temp_ + kTempStep > kMaxTemp
                ? kMaxTemp : temp_ + kTempStep;
            break;
        case Button::A:
            enabled_ = !enabled_;
            break;
        default:
            return;
        }
        if (brightness_ != previous_brightness) {
            hal_config_set_int("flashlight", "brightness", brightness_);
        }
        if (temp_ != previous_temp) {
            hal_config_set_int("flashlight", "temperature", temp_);
        }
        apply();
        render();
    }

private:
    void apply()
    {
        if (!enabled_) {
            hal_lights_clear();
            hal_lights_show();
            return;
        }

        const Rgb color = temperature_color(temp_);
        const uint8_t r = static_cast<uint8_t>(
            static_cast<uint16_t>(color.r) * brightness_ / kMaxBrightness);
        const uint8_t g = static_cast<uint8_t>(
            static_cast<uint16_t>(color.g) * brightness_ / kMaxBrightness);
        const uint8_t b = static_cast<uint8_t>(
            static_cast<uint16_t>(color.b) * brightness_ / kMaxBrightness);
        for (size_t i = 0; i < LED_COUNT; ++i) hal_lights_set(i, r, g, b);
        hal_lights_show();
    }

    void render()
    {
        if (power_label_) {
            lv_label_set_text(power_label_, enabled_ ? "ON" : "OFF");
            lv_obj_set_style_text_color(power_label_,
                lv_color_hex(enabled_ ? 0x76e6a9 : BadgeTheme::TEXT_DIM), 0);
        }
        if (brightness_label_) {
            lv_label_set_text_fmt(brightness_label_, "BRIGHTNESS  %d%%",
                (brightness_ * 100 + kMaxBrightness / 2) / kMaxBrightness);
        }
        if (brightness_slider_) {
            lv_slider_set_value(brightness_slider_, brightness_, LV_ANIM_OFF);
        }
        if (temp_label_) {
            lv_label_set_text_fmt(temp_label_, "TEMPERATURE  %s",
                                  temperature_name(temp_));
        }
        if (temp_slider_) {
            lv_slider_set_value(temp_slider_, temp_, LV_ANIM_OFF);
            const Rgb sample = temperature_color(temp_);
            lv_obj_set_style_bg_color(temp_slider_,
                lv_color_make(sample.r, sample.g, sample.b), LV_PART_KNOB);
        }
    }

    lv_obj_t *power_label_ = nullptr;
    lv_obj_t *brightness_label_ = nullptr;
    lv_obj_t *brightness_slider_ = nullptr;
    lv_obj_t *temp_label_ = nullptr;
    lv_obj_t *temp_slider_ = nullptr;
    int brightness_ = kDefaultBrightness;
    int temp_ = kNeutralTemp;
    bool enabled_ = true;
};

FlashlightApp s_instance;

} // namespace

App *app_flashlight_instance() { return &s_instance; }
