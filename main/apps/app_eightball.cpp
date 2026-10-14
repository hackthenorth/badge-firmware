#include "app.h"
#include "app_registry.h"
#include "hal_gesture.h"
#include "hal_lights.h"

#include "esp_random.h"

static const char *kAnswers[] = {
    "It is certain",
    "It is decidedly so",
    "Without a doubt",
    "Yes definitely",
    "You may rely on it",
    "As I see it, yes",
    "Most likely",
    "Outlook good",
    "Yes",
    "Signs point to yes",
    "Reply hazy, try again",
    "Ask again later",
    "Better not tell you now",
    "Cannot predict now",
    "Concentrate and ask again",
    "Don't count on it",
    "My reply is no",
    "My sources say no",
    "Outlook not so good",
    "Very doubtful",
};
static constexpr int kAnswerCount = sizeof(kAnswers) / sizeof(kAnswers[0]);

extern "C" const lv_image_dsc_t icon_eightball;

class EightBallApp : public GameApp {
public:
    const char *name() const override { return "EightBall"; }
    const char *icon() const override { return "8"; }
    const char *slug() const override { return "eightball"; }
    const lv_image_dsc_t *icon_image() const override { return &icon_eightball; }
    bool icon_needs_outline() const override { return true; }

    void on_enter(lv_obj_t *root) override
    {
        think_ticks_ = 0;
        answer_idx_ = -1;

        lv_obj_t *title = lv_label_create(root);
        lv_label_set_text(title, "Magic 8-Ball");
        lv_obj_set_style_text_color(title, lv_color_hex(BadgeTheme::TEXT), 0);
        lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
        lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 6);

        panel_ = lv_obj_create(root);
        lv_obj_remove_style_all(panel_);
        lv_obj_set_size(panel_, 280, 110);
        lv_obj_align(panel_, LV_ALIGN_CENTER, 0, -4);
        lv_obj_set_style_bg_color(panel_, lv_color_hex(BadgeTheme::SURFACE), 0);
        lv_obj_set_style_bg_opa(panel_, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(panel_, 0, 0);
        lv_obj_set_style_border_width(panel_, 2, 0);
        lv_obj_set_style_border_color(panel_, lv_color_hex(BadgeTheme::BORDER), 0);
        lv_obj_clear_flag(panel_, LV_OBJ_FLAG_SCROLLABLE);

        answer_ = lv_label_create(panel_);
        lv_obj_set_style_text_font(answer_, &lv_font_montserrat_18, 0);
        lv_obj_set_style_text_color(answer_, lv_color_hex(BadgeTheme::TEXT), 0);
        lv_obj_set_width(answer_, 260);
        lv_label_set_long_mode(answer_, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_align(answer_, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_center(answer_);
        lv_label_set_text(answer_, "Shake for wisdom");

        hint_ = lv_label_create(root);
        lv_obj_set_style_text_color(hint_, lv_color_hex(BadgeTheme::TEXT_MUTED), 0);
        lv_label_set_text(hint_, "SHAKE or A: ask");
        lv_obj_align(hint_, LV_ALIGN_BOTTOM_MID, 0, -10);
    }

    void on_exit() override
    {
        panel_ = answer_ = hint_ = nullptr;
    }

    void on_button(ButtonEvent ev) override
    {
        if (ev.kind != ButtonEventKind::Pressed) return;
        if (ev.button == Button::A) start_thinking();
    }

    void on_tick() override
    {
        if (gesture_poll_shake()) start_thinking();

        if (think_ticks_ > 0) {
            think_ticks_--;
            // Dim cycling text during the think window.
            if (answer_ && think_ticks_ % 3 == 0) {
                static const char *wait[] = {"...", ".  ", ".. ", "..."};
                lv_label_set_text(answer_, wait[(think_ticks_ / 3) % 4]);
            }
            if (think_ticks_ == 0) reveal();
        }
    }

private:
    void start_thinking()
    {
        think_ticks_ = 25; // ~500 ms
        answer_idx_ = (int)(esp_random() % kAnswerCount);
        if (answer_)
            lv_obj_set_style_border_color(lv_obj_get_parent(answer_),
                lv_color_hex(BadgeTheme::BORDER), 0);
    }

    void reveal()
    {
        if (!answer_ || answer_idx_ < 0) return;
        hal_lights_flash_success();
        lv_label_set_text(answer_, kAnswers[answer_idx_]);
        lv_obj_set_style_border_color(lv_obj_get_parent(answer_),
            lv_color_hex(BadgeTheme::ACCENT_DETAIL), 0);
    }

    lv_obj_t *panel_ = nullptr;
    lv_obj_t *answer_ = nullptr;
    lv_obj_t *hint_ = nullptr;
    int think_ticks_ = 0;
    int answer_idx_ = -1;
};

static EightBallApp s_instance;
App *app_eightball_instance() { return &s_instance; }
