#include "app.h"
#include "app_registry.h"
#include "hal_lights.h"
#include "hal_config.h"

#include "esp_random.h"
#include <cstdint>
#include <cstring>

// 4 "buttons": mapped to the d-pad directions, and each to an LED + color.
// The two middle/bottom positions on the right stay dark so the four choices
// remain visually distinct.
static const size_t kSlotToLed[4] = {
    BadgeLed::UpperLeft, BadgeLed::UpperRight,
    BadgeLed::MiddleLeft, BadgeLed::BottomLeft,
};
static const uint32_t kSlotColor[4] = {
    0xff5c8a, // Up    — pink
    0xffd166, // Left  — yellow
    0x00ffa8, // Right — green
    BadgeTheme::ACCENT_DETAIL, // Down  — violet
};

static inline int button_to_slot(Button b)
{
    switch (b) {
        case Button::Up:    return 0;
        case Button::Left:  return 1;
        case Button::Right: return 2;
        case Button::Down:  return 3;
        default: return -1;
    }
}

extern "C" const lv_image_dsc_t icon_simon;

class SimonApp : public GameApp {
public:
    const char *name() const override { return "Simon"; }
    const char *icon() const override { return "SMN"; }
    const char *slug() const override { return "simon"; }
    const lv_image_dsc_t *icon_image() const override { return &icon_simon; }

    void on_enter(lv_obj_t *root) override
    {
        hi_ = hal_config_get_int("simon", "high_score", 0);

        lv_obj_t *title = lv_label_create(root);
        lv_label_set_text(title, "Simon");
        lv_obj_set_style_text_color(title, lv_color_hex(BadgeTheme::TEXT), 0);
        lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
        lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 6);

        round_label_ = lv_label_create(root);
        lv_obj_set_style_text_font(round_label_, &lv_font_montserrat_18, 0);
        lv_obj_set_style_text_color(round_label_, lv_color_hex(BadgeTheme::ACCENT_DETAIL), 0);
        lv_obj_align(round_label_, LV_ALIGN_CENTER, 0, -30);

        status_ = lv_label_create(root);
        lv_obj_set_style_text_font(status_, &lv_font_montserrat_18, 0);
        lv_obj_set_style_text_color(status_, lv_color_hex(BadgeTheme::TEXT), 0);
        lv_obj_align(status_, LV_ALIGN_CENTER, 0, 4);

        score_label_ = lv_label_create(root);
        lv_obj_set_style_text_color(score_label_, lv_color_hex(BadgeTheme::TEXT_MUTED), 0);
        lv_obj_align(score_label_, LV_ALIGN_CENTER, 0, 40);

        hint_ = lv_label_create(root);
        lv_obj_set_style_text_color(hint_, lv_color_hex(BadgeTheme::TEXT_MUTED), 0);
        lv_label_set_text(hint_, "D-pad: echo   A: start");
        lv_obj_align(hint_, LV_ALIGN_BOTTOM_MID, 0, -10);

        reset_full();
        render();
    }

    void on_exit() override
    {
        hal_lights_clear();
        hal_lights_show();
        round_label_ = status_ = score_label_ = hint_ = nullptr;
    }

    void checkpoint_progress() override
    {
        const int score = length_ - 1;
        if (score <= hi_) return;
        hi_ = score;
        hal_config_set_int("simon", "high_score", hi_);
    }

    void on_button(ButtonEvent ev) override
    {
        if (ev.kind != ButtonEventKind::Pressed) return;

        if (state_ == State::GameOver || state_ == State::Idle) {
            if (ev.button == Button::A) {
                reset_full();
                begin_round();
            }
            return;
        }
        if (state_ != State::PlayerInput) return;

        int slot = button_to_slot(ev.button);
        if (slot < 0) return;

        // Flash + verify.
        flash_slot(slot);
        if (slot != sequence_[input_pos_]) {
            state_ = State::GameOver;
            if (status_) lv_label_set_text(status_, "WRONG");
            checkpoint_progress();
            render();
            return;
        }
        input_pos_++;
        if (input_pos_ >= length_) {
            // Round complete: grow sequence.
            length_++;
            if (length_ > kMaxLen) length_ = kMaxLen;
            begin_round();
        } else {
            render();
        }
    }

    void on_tick() override
    {
        if (state_ == State::ShowSequence) {
            if (flash_ticks_ > 0) { flash_ticks_--; return; }
            if (show_pos_ < length_) {
                flash_slot(sequence_[show_pos_]);
                show_pos_++;
                gap_ticks_ = 12;  // ~240 ms on
                flash_ticks_ = gap_ticks_;
                state_ = State::ShowSequenceOn;
            } else {
                input_pos_ = 0;
                state_ = State::PlayerInput;
                render();
            }
        } else if (state_ == State::ShowSequenceOn) {
            if (flash_ticks_ > 0) { flash_ticks_--; return; }
            // Off period between flashes.
            hal_lights_clear();
            hal_lights_show();
            flash_ticks_ = 6; // ~120 ms off
            state_ = State::ShowSequence;
        }

        if (flash_off_ticks_ > 0 && state_ == State::PlayerInput) {
            if (--flash_off_ticks_ == 0) {
                hal_lights_clear();
                hal_lights_show();
            }
        }
    }

private:
    enum class State { Idle, ShowSequence, ShowSequenceOn, PlayerInput, GameOver };

    static constexpr int kMaxLen = 64;

    void reset_full()
    {
        for (int i = 0; i < kMaxLen; i++) {
            sequence_[i] = (int8_t)(esp_random() % 4);
        }
        length_ = 1;
        show_pos_ = 0;
        input_pos_ = 0;
        state_ = State::Idle;
        flash_ticks_ = 0;
        flash_off_ticks_ = 0;
        hal_lights_clear();
        hal_lights_show();
    }

    void begin_round()
    {
        show_pos_ = 0;
        state_ = State::ShowSequence;
        hal_lights_clear();
        hal_lights_show();
        if (status_) lv_label_set_text(status_, "Watch");
        render();
    }

    void flash_slot(int slot)
    {
        hal_lights_clear();
        uint32_t c = kSlotColor[slot];
        hal_lights_set(kSlotToLed[slot],
            (c >> 16) & 0xff, (c >> 8) & 0xff, c & 0xff);
        hal_lights_show();
        flash_off_ticks_ = 6;
    }

    void render()
    {
        if (round_label_) lv_label_set_text_fmt(round_label_, "Round %d", length_);
        if (status_) {
            switch (state_) {
                case State::Idle:            lv_label_set_text(status_, "Press A to play"); break;
                case State::ShowSequence:
                case State::ShowSequenceOn:  lv_label_set_text(status_, "Watch"); break;
                case State::PlayerInput:     lv_label_set_text_fmt(status_, "Your turn %d/%d",
                                                 input_pos_, length_); break;
                case State::GameOver:        /* message already set */ break;
            }
        }
        if (score_label_) lv_label_set_text_fmt(score_label_, "hi %d", hi_);
    }

    lv_obj_t *round_label_ = nullptr;
    lv_obj_t *status_ = nullptr;
    lv_obj_t *score_label_ = nullptr;
    lv_obj_t *hint_ = nullptr;
    int8_t sequence_[kMaxLen] = {};
    int length_ = 1;
    int show_pos_ = 0;
    int input_pos_ = 0;
    State state_ = State::Idle;
    int flash_ticks_ = 0;
    int flash_off_ticks_ = 0;
    int gap_ticks_ = 0;
    int hi_ = 0;
};

static SimonApp s_instance;
App *app_simon_instance() { return &s_instance; }
