#include "app.h"
#include "hal_lights.h"
#include "app_registry.h"
#include "hal_gesture.h"

#include "esp_random.h"
#include <cstdio>

static const int kSides[] = { 4, 6, 8, 10, 12, 20, 100 };
static constexpr int kSideCount = sizeof(kSides) / sizeof(kSides[0]);

extern "C" const lv_image_dsc_t icon_dice;

class DiceApp : public GameApp {
public:
    const char *name() const override { return "Dice"; }
    const char *icon() const override { return "D20"; }
    const char *slug() const override { return "dice"; }
    const lv_image_dsc_t *icon_image() const override { return &icon_dice; }

    AppAccess access() const override { return AppAccess::Default; }

    void on_enter(lv_obj_t *root) override
    {
        pick_ = 5; // d20 default
        rolling_ticks_ = 0;
        value_ = 0;

        lv_obj_t *title = lv_label_create(root);
        lv_label_set_text(title, "Dice");
        lv_obj_set_style_text_color(title, lv_color_hex(BadgeTheme::TEXT), 0);
        lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
        lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 6);

        kind_ = lv_label_create(root);
        lv_obj_set_style_text_color(kind_, lv_color_hex(BadgeTheme::ACCENT_DETAIL), 0);
        lv_obj_set_style_text_font(kind_, &lv_font_montserrat_18, 0);
        lv_obj_align(kind_, LV_ALIGN_TOP_MID, 0, 36);

        face_ = lv_label_create(root);
        lv_obj_set_style_text_color(face_, lv_color_hex(BadgeTheme::TEXT), 0);
        lv_obj_set_style_text_font(face_, &lv_font_montserrat_24, 0);
        lv_obj_align(face_, LV_ALIGN_CENTER, 0, 10);

        hint_ = lv_label_create(root);
        lv_obj_set_style_text_color(hint_, lv_color_hex(BadgeTheme::TEXT_MUTED), 0);
        lv_label_set_text(hint_, "L/R: die   A or SHAKE: roll");
        lv_obj_align(hint_, LV_ALIGN_BOTTOM_MID, 0, -10);

        render();
    }

    void on_exit() override
    {
        kind_ = face_ = hint_ = nullptr;
    }

    void on_button(ButtonEvent ev) override
    {
        if (ev.kind != ButtonEventKind::Pressed) return;
        switch (ev.button) {
            case Button::Left:  if (pick_ > 0) pick_--; render(); break;
            case Button::Right: if (pick_ < kSideCount - 1) pick_++; render(); break;
            case Button::A:     start_roll(); break;
            default: break;
        }
    }

    void on_tick() override
    {
        if (gesture_poll_shake()) start_roll();

        if (rolling_ticks_ > 0) {
            value_ = 1 + (int)(esp_random() % kSides[pick_]);
            rolling_ticks_--;
            if (rolling_ticks_ == 0 && value_ == kSides[pick_]) hal_lights_flash_success();
            if (face_) lv_label_set_text_fmt(face_, "%d", value_);
        }
    }

private:
    void start_roll()
    {
        rolling_ticks_ = 10; // ~200 ms of rapid spin
        value_ = 1 + (int)(esp_random() % kSides[pick_]);
        if (face_) lv_label_set_text_fmt(face_, "%d", value_);
    }

    void render()
    {
        if (kind_) lv_label_set_text_fmt(kind_, "d%d", kSides[pick_]);
        if (face_) {
            if (value_) lv_label_set_text_fmt(face_, "%d", value_);
            else lv_label_set_text(face_, "--");
        }
    }

    lv_obj_t *kind_ = nullptr;
    lv_obj_t *face_ = nullptr;
    lv_obj_t *hint_ = nullptr;
    int pick_ = 5;
    int value_ = 0;
    int rolling_ticks_ = 0;
};

static DiceApp s_instance;
App *app_dice_instance() { return &s_instance; }
