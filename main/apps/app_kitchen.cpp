#include "app.h"
#include "app_registry.h"

#include <cmath>

// Showcase of a handful of LVGL widgets, auto-animated.
extern "C" const lv_image_dsc_t icon_kitchen;

class KitchenApp : public App {
public:
    const char *name() const override { return "Sink"; }
    const char *icon() const override { return "LV"; }
    const char *slug() const override { return "kitchen"; }
    const lv_image_dsc_t *icon_image() const override { return &icon_kitchen; }

    void on_enter(lv_obj_t *root) override
    {
        lv_obj_t *title = lv_label_create(root);
        lv_label_set_text(title, "LVGL Showcase");
        lv_obj_set_style_text_color(title, lv_color_hex(BadgeTheme::TEXT), 0);
        lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
        lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 4);

        // --- Arc (top-left) ---
        arc_ = lv_arc_create(root);
        lv_obj_set_size(arc_, 70, 70);
        lv_obj_set_pos(arc_, 10, 34);
        lv_arc_set_range(arc_, 0, 100);
        lv_arc_set_bg_angles(arc_, 135, 45);
        lv_obj_remove_flag(arc_, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_arc_color(arc_, lv_color_hex(BadgeTheme::SURFACE), LV_PART_MAIN);
        lv_obj_set_style_arc_color(arc_, lv_color_hex(BadgeTheme::ACCENT_DETAIL), LV_PART_INDICATOR);
        lv_obj_set_style_bg_opa(arc_, LV_OPA_TRANSP, LV_PART_KNOB);

        // --- Bar (below arc) ---
        bar_ = lv_bar_create(root);
        lv_obj_set_size(bar_, 120, 14);
        lv_obj_set_pos(bar_, 90, 50);
        lv_bar_set_range(bar_, 0, 100);
        lv_obj_set_style_bg_color(bar_, lv_color_hex(BadgeTheme::SURFACE), LV_PART_MAIN);
        lv_obj_set_style_bg_color(bar_, lv_color_hex(0x8a63ff), LV_PART_INDICATOR);
        lv_obj_set_style_bg_opa(bar_, LV_OPA_COVER, LV_PART_INDICATOR);

        // --- Slider (below bar) ---
        slider_ = lv_slider_create(root);
        lv_obj_set_size(slider_, 120, 10);
        lv_obj_set_pos(slider_, 90, 80);
        lv_slider_set_range(slider_, 0, 100);
        lv_obj_remove_flag(slider_, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_color(slider_, lv_color_hex(BadgeTheme::SURFACE), LV_PART_MAIN);
        lv_obj_set_style_bg_color(slider_, lv_color_hex(0x00ffa8), LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(slider_, lv_color_hex(BadgeTheme::TEXT), LV_PART_KNOB);

        // --- Switch + checkbox + button (right column) ---
        sw_ = lv_switch_create(root);
        lv_obj_set_size(sw_, 42, 22);
        lv_obj_set_pos(sw_, 228, 40);
        lv_obj_remove_flag(sw_, LV_OBJ_FLAG_CLICKABLE);

        cb_ = lv_checkbox_create(root);
        lv_checkbox_set_text(cb_, "Ready");
        lv_obj_set_pos(cb_, 228, 70);
        lv_obj_set_style_text_color(cb_, lv_color_hex(BadgeTheme::TEXT), 0);
        lv_obj_remove_flag(cb_, LV_OBJ_FLAG_CLICKABLE);

        btn_ = lv_button_create(root);
        lv_obj_set_size(btn_, 70, 26);
        lv_obj_set_pos(btn_, 228, 100);
        lv_obj_remove_flag(btn_, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_color(btn_, lv_color_hex(BadgeTheme::BORDER), 0);
        lv_obj_t *btn_lbl = lv_label_create(btn_);
        lv_label_set_text(btn_lbl, "PRESS");
        lv_obj_center(btn_lbl);

        // --- Spinner (bottom-left) ---
        spinner_ = lv_spinner_create(root);
        lv_obj_set_size(spinner_, 50, 50);
        lv_obj_set_pos(spinner_, 10, 160);
        lv_spinner_set_anim_params(spinner_, 1000, 200);
        lv_obj_set_style_arc_color(spinner_, lv_color_hex(BadgeTheme::SURFACE), LV_PART_MAIN);
        lv_obj_set_style_arc_color(spinner_, lv_color_hex(0xff5c8a), LV_PART_INDICATOR);

        // --- Rolling readout (bottom, wide) ---
        readout_ = lv_label_create(root);
        lv_obj_set_style_text_color(readout_, lv_color_hex(BadgeTheme::ACCENT_DETAIL), 0);
        lv_obj_set_style_text_font(readout_, &lv_font_montserrat_14, 0);
        lv_obj_set_pos(readout_, 75, 175);

        phase_ = 0;
    }

    void on_exit() override
    {
        arc_ = bar_ = slider_ = sw_ = cb_ = btn_ = spinner_ = readout_ = nullptr;
    }

    void on_tick() override
    {
        phase_ += 0.04f;

        int a = (int)((sinf(phase_) + 1.0f) * 50);          // 0..100
        int b = (int)((sinf(phase_ * 1.3f + 1.0f) + 1.0f) * 50);
        int c = (int)((sinf(phase_ * 0.7f + 2.0f) + 1.0f) * 50);

        if (arc_)    lv_arc_set_value(arc_, a);
        if (bar_)    lv_bar_set_value(bar_, b, LV_ANIM_OFF);
        if (slider_) lv_slider_set_value(slider_, c, LV_ANIM_OFF);

        // Toggle switch/checkbox/button colors on a slow timer.
        int tick = (int)(phase_ * 10);
        bool on = (tick / 6) & 1;
        if (sw_ && on != sw_on_) {
            if (on) lv_obj_add_state(sw_, LV_STATE_CHECKED);
            else    lv_obj_remove_state(sw_, LV_STATE_CHECKED);
            sw_on_ = on;
        }
        bool cb_on = ((tick / 9) & 1);
        if (cb_ && cb_on != cb_on_) {
            if (cb_on) lv_obj_add_state(cb_, LV_STATE_CHECKED);
            else       lv_obj_remove_state(cb_, LV_STATE_CHECKED);
            cb_on_ = cb_on;
        }
        bool btn_on = ((tick / 4) & 1);
        if (btn_ && btn_on != btn_on_) {
            lv_obj_set_style_bg_color(btn_,
                lv_color_hex(btn_on ? BadgeTheme::ACCENT_DETAIL : BadgeTheme::BORDER), 0);
            btn_on_ = btn_on;
        }

        if (readout_) lv_label_set_text_fmt(readout_,
            "arc=%3d  bar=%3d  sld=%3d", a, b, c);
    }

private:
    lv_obj_t *arc_ = nullptr;
    lv_obj_t *bar_ = nullptr;
    lv_obj_t *slider_ = nullptr;
    lv_obj_t *sw_ = nullptr;
    lv_obj_t *cb_ = nullptr;
    lv_obj_t *btn_ = nullptr;
    lv_obj_t *spinner_ = nullptr;
    lv_obj_t *readout_ = nullptr;
    bool sw_on_ = false;
    bool cb_on_ = false;
    bool btn_on_ = false;
    float phase_ = 0;
};

static KitchenApp s_instance;
App *app_kitchen_instance() { return &s_instance; }
