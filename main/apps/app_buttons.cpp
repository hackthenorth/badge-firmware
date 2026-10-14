#include "app.h"
#include "app_registry.h"

#include <cstring>

// Visualizes every button. B is an ordinary app button; only HOME never
// arrives here (the registry intercepts it on the way home).
extern "C" const lv_image_dsc_t icon_buttons;

class ButtonsApp : public App {
public:
    const char *name() const override { return "Buttons"; }
    const char *icon() const override { return "BTN"; }
    const char *slug() const override { return "buttons"; }
    const lv_image_dsc_t *icon_image() const override { return &icon_buttons; }

    void on_enter(lv_obj_t *root) override
    {
        memset(pressed_, 0, sizeof(pressed_));
        for (auto &p : pads_) p = nullptr;

        lv_obj_t *title = lv_label_create(root);
        lv_label_set_text(title, "Buttons");
        lv_obj_set_style_text_color(title, lv_color_hex(BadgeTheme::TEXT), 0);
        lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
        lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 6);

        // D-pad cluster (left side): Up top, Down bottom, Left left, Right right.
        constexpr int dpad_cx = 80, dpad_cy = 140, arm = 40;
        make_pad(root, Button::Up,    dpad_cx,         dpad_cy - arm, "UP");
        make_pad(root, Button::Down,  dpad_cx,         dpad_cy + arm, "DN");
        make_pad(root, Button::Left,  dpad_cx - arm,   dpad_cy,       "LF");
        make_pad(root, Button::Right, dpad_cx + arm,   dpad_cy,       "RT");

        // Face buttons (right side): A / B / Start / Home / Aux1.
        make_pad(root, Button::A,     240, 170, "A");
        make_pad(root, Button::B,     230, 135, "B");
        make_pad(root, Button::Start, 260, 100, "STRT");
        make_pad(root, Button::Home,  200, 100, "HOME");
        make_pad(root, Button::Aux1,  200, 170, "AUX1");

        refresh_all();
    }

    void on_exit() override
    {
        for (auto &p : pads_) p = nullptr;
    }

    void on_button(ButtonEvent ev) override
    {
        pressed_[(int)ev.button] = (ev.kind == ButtonEventKind::Pressed);
        update_pad(ev.button);
    }

private:
    void make_pad(lv_obj_t *parent, Button b, int cx, int cy, const char *label)
    {
        int w = 56, h = 36;
        lv_obj_t *pad = lv_obj_create(parent);
        lv_obj_remove_style_all(pad);
        lv_obj_set_size(pad, w, h);
        lv_obj_set_pos(pad, cx - w / 2, cy - h / 2);
        lv_obj_set_style_radius(pad, 0, 0);
        lv_obj_set_style_bg_opa(pad, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(pad, 2, 0);
        lv_obj_clear_flag(pad, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *lbl = lv_label_create(pad);
        lv_label_set_text(lbl, label);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_16, 0);
        lv_obj_center(lbl);

        pads_[(int)b] = pad;
    }

    void refresh_all()
    {
        for (int i = 0; i < (int)Button::COUNT; i++) update_pad((Button)i);
    }

    void update_pad(Button b)
    {
        lv_obj_t *pad = pads_[(int)b];
        if (!pad) return;
        bool p = pressed_[(int)b];
        uint32_t bg     = p ? BadgeTheme::ACCENT_DETAIL : BadgeTheme::SURFACE;
        uint32_t border = p ? 0xffffff : BadgeTheme::BORDER;
        uint32_t text   = p ? BadgeTheme::BACKGROUND : BadgeTheme::TEXT;
        lv_obj_set_style_bg_color(pad, lv_color_hex(bg), 0);
        lv_obj_set_style_border_color(pad, lv_color_hex(border), 0);

        // Label is the only child.
        lv_obj_t *lbl = lv_obj_get_child(pad, 0);
        if (lbl) lv_obj_set_style_text_color(lbl, lv_color_hex(text), 0);
    }

    lv_obj_t *pads_[(int)Button::COUNT] = {};
    bool pressed_[(int)Button::COUNT] = {};
};

static ButtonsApp s_instance;
App *app_buttons_instance() { return &s_instance; }
