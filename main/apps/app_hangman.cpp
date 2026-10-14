#include "app.h"
#include "hal_config.h"
#include "hal_lights.h"

#include "esp_random.h"
#include <cstdio>
#include <cstring>

// Hangman, hacker edition. Up/Down picks a letter, A guesses it.
// Six misses and the deploy fails.

extern "C" const lv_image_dsc_t icon_hangman;

class HangmanApp : public GameApp {
public:
    const char *name() const override { return "Hangman"; }
    const char *icon() const override { return "HNG"; }
    const lv_image_dsc_t *icon_image() const override { return &icon_hangman; }
    const char *slug() const override { return "hangman"; }

    void on_enter(lv_obj_t *root) override
    {
        wins_ = hal_config_get_int("hangman", "wins", 0);

        title_ = lv_label_create(root);
        lv_label_set_text(title_, "Hangman");
        lv_obj_set_style_text_color(title_, lv_color_hex(BadgeTheme::TEXT), 0);
        lv_obj_set_style_text_font(title_, &lv_font_montserrat_20, 0);
        lv_obj_align(title_, LV_ALIGN_TOP_MID, 0, 6);

        art_ = lv_label_create(root);
        lv_obj_set_style_text_color(art_, lv_color_hex(BadgeTheme::ACCENT_DETAIL), 0);
        lv_obj_set_style_text_font(art_, &lv_font_montserrat_16, 0);
        lv_obj_align(art_, LV_ALIGN_TOP_MID, 0, 34);

        word_ = lv_label_create(root);
        lv_obj_set_style_text_color(word_, lv_color_hex(BadgeTheme::TEXT), 0);
        lv_obj_set_style_text_font(word_, &lv_font_montserrat_22, 0);
        lv_obj_align(word_, LV_ALIGN_CENTER, 0, 10);

        pick_ = lv_label_create(root);
        lv_obj_set_style_text_color(pick_, lv_color_hex(BadgeTheme::TEXT), 0);
        lv_obj_set_style_text_font(pick_, &lv_font_montserrat_20, 0);
        lv_obj_align(pick_, LV_ALIGN_CENTER, 0, 48);

        hint_ = lv_label_create(root);
        lv_obj_set_style_text_color(hint_, lv_color_hex(BadgeTheme::TEXT_MUTED), 0);
        lv_obj_align(hint_, LV_ALIGN_BOTTOM_MID, 0, -10);

        new_word();
    }

    void on_exit() override
    {
        hal_lights_clear();
        hal_lights_show();
        title_ = art_ = word_ = pick_ = hint_ = nullptr;
    }

    void on_button(ButtonEvent ev) override
    {
        if (ev.kind != ButtonEventKind::Pressed) return;
        if (over_) {
            if (ev.button == Button::A) new_word();
            return;
        }
        switch (ev.button) {
            case Button::Up:
                letter_ = (letter_ == 'Z') ? 'A' : (char)(letter_ + 1);
                skip_guessed(1);
                break;
            case Button::Down:
                letter_ = (letter_ == 'A') ? 'Z' : (char)(letter_ - 1);
                skip_guessed(-1);
                break;
            case Button::A: guess(); break;
            default: break;
        }
        render();
    }

private:
    static constexpr const char *kWords[] = {
        "DEPLOY",   "BADGE",    "HACKER",  "STANDUP", "FIRMWARE",
        "SOLDER",   "DEMO",     "LAPTOP",  "KERNEL",  "PIXEL",
        "ROUTER",   "GITHUB",   "MODEM",  "PRINTER", "TABLE",
        "WIFI",
    };
    static constexpr int kWordCount = sizeof(kWords) / sizeof(kWords[0]);
    // Gallows stages by miss count 0..6, drawn with plain ASCII.
    static constexpr const char *kArt[] = {
        "+---+\n    |\n    |\n    |",
        "+---+\nO   |\n    |\n    |",
        "+---+\nO   |\n|   |\n    |",
        "+---+\nO   |\n/|  |\n    |",
        "+---+\nO   |\n/|\\ |\n    |",
        "+---+\nO   |\n/|\\ |\n/   |",
        "+---+\nO   |\n/|\\ |\n/ \\ |",
    };

    void new_word()
    {
        const char *w = kWords[esp_random() % kWordCount];
        // Words table is fixed width-unfriendly; copy and trim spaces.
        int n = 0;
        for (const char *p = w; *p && n < 11; p++) {
            if (*p != ' ') word_buf_[n++] = *p;
        }
        word_buf_[n] = '\0';
        word_len_ = n;
        memset(guessed_, 0, sizeof(guessed_));
        misses_ = 0;
        over_ = false;
        won_ = false;
        letter_ = 'A';
        hal_lights_clear();
        hal_lights_show();
        render();
    }

    void skip_guessed(int dir)
    {
        for (int i = 0; i < 26; i++) {
            if (!guessed_[letter_ - 'A']) return;
            letter_ = (char)(letter_ + dir);
            if (letter_ < 'A') letter_ = 'Z';
            if (letter_ > 'Z') letter_ = 'A';
        }
    }

    void guess()
    {
        if (guessed_[letter_ - 'A']) return;
        guessed_[letter_ - 'A'] = true;
        bool hit = false;
        for (int i = 0; i < word_len_; i++) {
            if (word_buf_[i] == letter_) hit = true;
        }
        if (!hit) {
            misses_++;
            // Misses light the ring red, one LED per miss.
            for (int i = 0; i < misses_ && i < (int)LED_COUNT; i++) {
                hal_lights_set((size_t)i, 255, 0, 0);
            }
            hal_lights_show();
        }
        if (misses_ >= 6) {
            over_ = true;
            won_ = false;
        } else {
            won_ = true;
            for (int i = 0; i < word_len_; i++) {
                if (!guessed_[word_buf_[i] - 'A']) won_ = false;
            }
            if (won_) {
                over_ = true;
                wins_++;
                hal_config_set_int("hangman", "wins", wins_);
                hal_lights_flash_success();
            } else {
                skip_guessed(1);
            }
        }
    }

    void render()
    {
        if (art_) lv_label_set_text(art_, kArt[misses_]);
        if (word_) {
            char buf[32];
            int n = 0;
            for (int i = 0; i < word_len_ && n < (int)sizeof(buf) - 3; i++) {
                buf[n++] = guessed_[word_buf_[i] - 'A'] ? word_buf_[i] : '_';
                buf[n++] = ' ';
            }
            buf[n] = '\0';
            lv_label_set_text(word_, buf);
        }
        if (pick_) {
            if (over_) {
                lv_label_set_text(pick_, won_ ? "SHIPPED!" : word_buf_);
            } else {
                lv_label_set_text_fmt(pick_, "< %c >", letter_);
            }
        }
        if (hint_) {
            if (over_) lv_label_set_text(hint_, "A: new word");
            else lv_label_set_text_fmt(hint_, "U/D: letter  A: guess  wins %d", wins_);
        }
    }

    lv_obj_t *title_ = nullptr;
    lv_obj_t *art_ = nullptr;
    lv_obj_t *word_ = nullptr;
    lv_obj_t *pick_ = nullptr;
    lv_obj_t *hint_ = nullptr;
    char word_buf_[12] = { 0 };
    int word_len_ = 0;
    bool guessed_[26] = { false };
    char letter_ = 'A';
    int misses_ = 0;
    int wins_ = 0;
    bool over_ = false;
    bool won_ = false;
};

static HangmanApp s_instance;
App *app_hangman_instance() { return &s_instance; }
