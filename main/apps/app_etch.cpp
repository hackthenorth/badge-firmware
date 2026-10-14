#include "app.h"
#include "hal_gesture.h"

#include <cstdio>
#include <memory>
#include <new>

// Etch-a-sketch. D-pad drags the stylus, shake (or B) clears the slate.
// The trail is a ring of small dots so memory stays bounded.

extern "C" const lv_image_dsc_t icon_etch;

class EtchApp : public GameApp {
public:
    const char *name() const override { return "Etch"; }
    const char *icon() const override { return "ETC"; }
    const lv_image_dsc_t *icon_image() const override { return &icon_etch; }
    const char *slug() const override { return "etch"; }

    void on_enter(lv_obj_t *root) override
    {
        root_ = root;
        dots_.reset(new (std::nothrow) lv_obj_t *[kMaxDots]{});
        if (!dots_) {
            lv_obj_t *oops = lv_label_create(root);
            lv_label_set_text(oops, "Not enough memory\nHOME for Apps");
            lv_obj_set_style_text_color(oops, lv_color_hex(BadgeTheme::TEXT), 0);
            lv_obj_center(oops);
            return;
        }
        x_ = 160;
        y_ = 110;
        pen_down_ = true;
        held_up_ = held_down_ = held_left_ = held_right_ = false;
        head_ = 0;
        filled_ = 0;

        pad_ = lv_obj_create(root);
        lv_obj_set_style_pad_all(pad_, 0, 0);
        lv_obj_clear_flag(pad_, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(pad_, PAD_W, PAD_H);
        lv_obj_align(pad_, LV_ALIGN_CENTER, 0, 8);
        lv_obj_set_style_bg_color(pad_, lv_color_hex(BadgeTheme::PANEL), 0);
        lv_obj_set_style_border_width(pad_, 0, 0);
        lv_obj_set_style_radius(pad_, 4, 0);

        cursor_ = lv_obj_create(pad_);
        lv_obj_set_size(cursor_, DOT + 2, DOT + 2);
        lv_obj_set_style_bg_color(cursor_, lv_color_hex(0xff0000), 0);
        lv_obj_set_style_border_width(cursor_, 0, 0);
        lv_obj_set_style_radius(cursor_, LV_RADIUS_CIRCLE, 0);

        hint_ = lv_label_create(root);
        lv_obj_set_style_text_color(hint_, lv_color_hex(BadgeTheme::TEXT_MUTED), 0);
        lv_obj_set_style_text_font(hint_, &lv_font_montserrat_12, 0);
        update_hint();
        lv_obj_align(hint_, LV_ALIGN_BOTTOM_MID, 0, -8);

        stamp();
        draw_cursor();
    }

    void on_exit() override
    {
        // Dots are children of pad_; deleting the pad frees them all.
        root_ = pad_ = cursor_ = hint_ = nullptr;
        dots_.reset();
    }

    void on_button(ButtonEvent ev) override
    {
        if (!dots_) return;
        if (ev.kind == ButtonEventKind::Pressed && ev.button == Button::A) {
            pen_down_ = !pen_down_;
            if (pen_down_) stamp();
            update_hint();
            return;
        }
        if (ev.kind == ButtonEventKind::Pressed && ev.button == Button::B) {
            clear();
            return;
        }
        if (ev.kind == ButtonEventKind::Pressed) {
            if (ev.button == Button::Up) held_up_ = true;
            if (ev.button == Button::Down) held_down_ = true;
            if (ev.button == Button::Left) held_left_ = true;
            if (ev.button == Button::Right) held_right_ = true;
        } else {
            if (ev.button == Button::Up) held_up_ = false;
            if (ev.button == Button::Down) held_down_ = false;
            if (ev.button == Button::Left) held_left_ = false;
            if (ev.button == Button::Right) held_right_ = false;
        }
    }

    void on_tick() override
    {
        if (!dots_) return;
        if (gesture_poll_shake()) clear();
        bool moved = false;
        if (held_up_ && y_ > 0) { y_ -= STEP; moved = true; }
        if (held_down_ && y_ < PAD_H - DOT) { y_ += STEP; moved = true; }
        if (held_left_ && x_ > 0) { x_ -= STEP; moved = true; }
        if (held_right_ && x_ < PAD_W - DOT) { x_ += STEP; moved = true; }
        if (moved) {
            stamp();
            draw_cursor();
        }
    }

private:
    static constexpr int PAD_W = 292;
    static constexpr int PAD_H = 168;
    static constexpr int DOT = 4;
    static constexpr int STEP = 3;
    static constexpr int kMaxDots = 900;

    void stamp()
    {
        if (!pad_ || !pen_down_) return;
        if (filled_ == kMaxDots) {
            // Ring full: recycle the oldest dot.
            lv_obj_set_pos(dots_[head_], x_, y_);
            head_ = (head_ + 1) % kMaxDots;
            return;
        }
        lv_obj_t *d = lv_obj_create(pad_);
        lv_obj_set_size(d, DOT, DOT);
        lv_obj_set_pos(d, x_, y_);
        lv_obj_set_style_bg_color(d, lv_color_hex(BadgeTheme::TEXT), 0);
        lv_obj_set_style_border_width(d, 0, 0);
        lv_obj_set_style_radius(d, 1, 0);
        dots_[filled_++] = d;
    }

    void update_hint()
    {
        lv_label_set_text(hint_, pen_down_
            ? "Pen DOWN  A: lift  D-pad: move  B: clear"
            : "Pen UP  A: draw  D-pad: move  B: clear");
    }

    void draw_cursor()
    {
        if (cursor_) lv_obj_set_pos(cursor_, x_ - 1, y_ - 1);
    }

    void clear()
    {
        if (!pad_) return;
        for (int i = 0; i < filled_; i++) {
            if (dots_[i]) lv_obj_delete(dots_[i]);
            dots_[i] = nullptr;
        }
        filled_ = 0;
        head_ = 0;
        // Cursor was created before any dot, keep it above the trail.
        if (cursor_) lv_obj_move_foreground(cursor_);
    }

    lv_obj_t *root_ = nullptr;
    lv_obj_t *pad_ = nullptr;
    lv_obj_t *cursor_ = nullptr;
    lv_obj_t *hint_ = nullptr;
    std::unique_ptr<lv_obj_t *[]> dots_;
    int head_ = 0;
    int filled_ = 0;
    int x_ = 0;
    int y_ = 0;
    bool pen_down_ = true;
    bool held_up_ = false;
    bool held_down_ = false;
    bool held_left_ = false;
    bool held_right_ = false;
};

static EtchApp s_instance;
App *app_etch_instance() { return &s_instance; }
