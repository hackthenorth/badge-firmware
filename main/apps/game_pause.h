#pragma once

#include "hal_buttons.h"
#include "ui_theme.h"

#include "lvgl.h"

// Shared pause behavior for games that update in real time. Start toggles
// pause; games may also opt into B. The registry handles Home confirmation.
class GamePauseController {
public:
    void create(lv_obj_t *root)
    {
        card_ = lv_obj_create(root);
        lv_obj_remove_style_all(card_);
        lv_obj_set_size(card_, 250, 116);
        lv_obj_align(card_, LV_ALIGN_CENTER, 0, 8);
        lv_obj_set_style_radius(card_, 12, 0);
        lv_obj_set_style_bg_color(card_, lv_color_hex(BadgeTheme::BACKGROUND), 0);
        lv_obj_set_style_bg_opa(card_, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(card_, 2, 0);
        lv_obj_set_style_border_color(card_, lv_color_hex(BadgeTheme::ACCENT_DETAIL), 0);
        lv_obj_clear_flag(card_, LV_OBJ_FLAG_SCROLLABLE);

        text_ = lv_label_create(card_);
        lv_obj_set_width(text_, 226);
        lv_obj_set_style_text_align(text_, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_font(text_, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(text_, lv_color_hex(BadgeTheme::TEXT), 0);
        lv_obj_center(text_);

        reset();
    }

    void on_exit()
    {
        card_ = nullptr;
        text_ = nullptr;
        paused_ = false;
    }

    void reset()
    {
        paused_ = false;
        hide();
    }

    bool paused() const { return paused_; }

    bool handle(ButtonEvent ev, bool can_pause, bool b_toggles_pause = true)
    {
        if (ev.kind != ButtonEventKind::Pressed) return false;

        if (can_pause && (ev.button == Button::Start
                          || (b_toggles_pause && ev.button == Button::B))) {
            paused_ = !paused_;
            if (paused_) {
                show(b_toggles_pause ? "Paused\nStart or B to resume\nHome for Apps"
                                     : "Paused\nStart to resume\nHome for Apps");
            }
            else hide();
            return true;
        }

        return false;
    }

private:
    void show(const char *text)
    {
        if (text_) lv_label_set_text(text_, text);
        if (card_) {
            lv_obj_clear_flag(card_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(card_);
        }
    }

    void hide()
    {
        if (card_) lv_obj_add_flag(card_, LV_OBJ_FLAG_HIDDEN);
    }

    lv_obj_t *card_ = nullptr;
    lv_obj_t *text_ = nullptr;
    bool paused_ = false;
};

inline lv_obj_t *game_pause_create_hint(lv_obj_t *root, lv_align_t align,
                                        int x, int y)
{
    lv_obj_t *hint = lv_label_create(root);
    lv_label_set_text(hint, "Start to pause");
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(hint, lv_color_hex(BadgeTheme::TEXT_MUTED), 0);
    lv_obj_align(hint, align, x, y);
    return hint;
}
