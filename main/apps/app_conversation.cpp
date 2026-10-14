#include "app.h"

#include "esp_random.h"

#include <cstddef>

namespace {

constexpr const char *kPrompts[] = {
    "What is your most controversial snack opinion?",
    "Which animal would be the worst project manager?",
    "What tiny problem would you solve with unlimited funding?",
    "Pitch your project using only sound effects.",
    "What is the weirdest useful thing you have built?",
    "Which app deserves a dramatic movie adaptation?",
    "What would your personal warning label say?",
    "Invent a new holiday. How do we celebrate it?",
    "What is your most unnecessary technical skill?",
    "Which fictional world has the best Wi-Fi?",
    "What would be in your dream vending machine?",
    "Name a terrible superpower with one perfect use.",
    "What should absolutely have a dark mode?",
    "Which bug would make the funniest feature?",
    "What is the best name you have given a device?",
    "If your project had a mascot, what would it be?",
    "What everyday object needs an API?",
    "What is your favourite harmless conspiracy theory?",
    "Which three words describe your weekend so far?",
    "What would you automate if setup took five minutes?",
    "What is the funniest thing you believed as a kid?",
    "Invent a startup that should never receive funding.",
    "What food would make the best programming language?",
    "What is the boldest shortcut you have taken today?",
    "Which household chore should be an esport?",
    "What is the strangest item in your backpack?",
    "What would your hacker walk-up song be?",
    "If your laptop could talk, what would it complain about?",
    "What is the best accidental discovery you have made?",
    "Which two technologies should never be combined?",
    "What would a capybara build at a hackathon?",
    "Explain your project like it is a bedtime story.",
    "What is a hill you would defend with a foam sword?",
    "What would you name a pet robot?",
    "Which keyboard key has the most personality?",
    "What is your ideal 2 a.m. snack?",
    "If debugging were a sport, what are the rules?",
    "What mundane task makes you feel extremely powerful?",
    "Which fictional character would be a great teammate?",
    "What is the most delightful website you know?",
    "Design a useless button. What does it do?",
    "What topic could you give a surprise talk about?",
    "Which invention would confuse a medieval knight most?",
    "What is your favourite oddly specific fact?",
    "What is a project idea you hope someone steals?",
    "What would make this room 10 percent more whimsical?",
    "Which error message best describes your current mood?",
    "What would you put in a museum of the internet?",
    "What is the kindest thing a stranger has done for you?",
    "Which smell should be available as a phone notification?",
    "What is a completely impractical dream project?",
    "What would your team name be in a cooking competition?",
    "Which piece of tech feels most like magic to you?",
    "What is one thing you learned this weekend?",
    "Turn the nearest object into a startup pitch.",
    "What skill would you download instantly?",
    "Which cartoon gadget do you still want?",
    "What is your favourite way to procrastinate productively?",
    "What would the final boss of this hackathon be?",
    "Describe your vibe as a pull request title.",
};

constexpr size_t kPromptCount = sizeof(kPrompts) / sizeof(kPrompts[0]);

extern "C" const lv_image_dsc_t icon_conversation;

class ConversationApp final : public App {
public:
    const char *name() const override { return "Conversation Starters"; }
    const char *icon() const override { return "TLK"; }
    const lv_image_dsc_t *icon_image() const override { return &icon_conversation; }
    const char *slug() const override { return "conversation"; }

    void on_enter(lv_obj_t *root) override
    {
        root_ = root;
        lv_obj_set_style_bg_color(root_, lv_color_hex(BadgeTheme::BACKGROUND), 0);
        lv_obj_set_style_bg_opa(root_, LV_OPA_COVER, 0);

        lv_obj_t *title = lv_label_create(root_);
        lv_label_set_text(title, "Conversation Starters");
        lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(title, lv_color_hex(BadgeTheme::TEXT), 0);
        lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 4);

        lv_obj_t *credit = lv_label_create(root_);
        lv_label_set_text(credit, "Inspired by calvin.sh's Mingletron 2000");
        lv_obj_set_style_text_font(credit, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(credit, lv_color_hex(BadgeTheme::TEXT_DIM), 0);
        lv_obj_align(credit, LV_ALIGN_TOP_MID, 0, 26);

        for (size_t i = 0; i < 3; ++i) {
            cards_[i] = lv_obj_create(root_);
            lv_obj_set_size(cards_[i], 302, 53);
            lv_obj_set_pos(cards_[i], 9, 42 + static_cast<int>(i) * 56);
            lv_obj_set_style_bg_color(cards_[i],
                lv_color_hex(0x132f35), 0);
            lv_obj_set_style_border_width(cards_[i], 1, 0);
            lv_obj_set_style_border_color(cards_[i],
                lv_color_hex(BadgeTheme::ACCENT_DETAIL), 0);
            lv_obj_set_style_radius(cards_[i], 9, 0);
            lv_obj_set_style_pad_all(cards_[i], 6, 0);
            lv_obj_clear_flag(cards_[i], LV_OBJ_FLAG_SCROLLABLE);

            labels_[i] = lv_label_create(cards_[i]);
            lv_obj_set_width(labels_[i], 286);
            lv_label_set_long_mode(labels_[i], LV_LABEL_LONG_WRAP);
            lv_obj_set_style_text_align(labels_[i], LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_set_style_text_font(labels_[i], &lv_font_montserrat_14, 0);
            lv_obj_set_style_text_color(labels_[i], lv_color_hex(BadgeTheme::TEXT), 0);
            lv_obj_center(labels_[i]);
        }

        lv_obj_t *hint = lv_label_create(root_);
        lv_label_set_text(hint, "A / START  new trio     HOME  apps");
        lv_obj_set_style_text_font(hint, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(hint, lv_color_hex(BadgeTheme::TEXT_DIM), 0);
        lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -7);

        shuffle();
    }

    void on_exit() override
    {
        root_ = nullptr;
        for (lv_obj_t *&label : labels_) label = nullptr;
        for (lv_obj_t *&card : cards_) card = nullptr;
    }

    void on_button(ButtonEvent ev) override
    {
        if (ev.kind != ButtonEventKind::Pressed) return;
        if (ev.button == Button::A || ev.button == Button::Start) shuffle();
    }

private:
    void shuffle()
    {
        size_t chosen[3] = {};
        for (size_t i = 0; i < 3; ++i) {
            bool unique = false;
            while (!unique) {
                chosen[i] = esp_random() % kPromptCount;
                unique = true;
                for (size_t j = 0; j < i; ++j) {
                    if (chosen[j] == chosen[i]) unique = false;
                }
            }
            lv_label_set_text(labels_[i], kPrompts[chosen[i]]);
        }
    }

    lv_obj_t *root_ = nullptr;
    lv_obj_t *cards_[3] = {};
    lv_obj_t *labels_[3] = {};
};

ConversationApp s_instance;

} // namespace

App *app_conversation_instance() { return &s_instance; }
