#include "app.h"
#include "hal_config.h"
#include "hal_lights.h"

#include <cstdio>

// Pong vs the badge. Up/Down move your paddle, first to 5 wins.
// The LEDs track the ball: left side violet while the ball approaches you.

extern "C" const lv_image_dsc_t icon_pong;

class PongApp : public GameApp {
public:
    const char *name() const override { return "Pong"; }
    const char *icon() const override { return "PNG"; }
    const lv_image_dsc_t *icon_image() const override { return &icon_pong; }
    const char *slug() const override { return "pong"; }

    bool wants_wake_lock() const override { return true; }

    void on_enter(lv_obj_t *root) override
    {
        wins_ = hal_config_get_int("pong", "wins", 0);

        score_ = lv_label_create(root);
        lv_obj_set_style_text_color(score_, lv_color_hex(BadgeTheme::TEXT), 0);
        lv_obj_set_style_text_font(score_, &lv_font_montserrat_20, 0);
        lv_obj_set_width(score_, FIELD_W);
        lv_obj_set_style_text_align(score_, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_long_mode(score_, LV_LABEL_LONG_WRAP);
        lv_obj_align(score_, LV_ALIGN_TOP_MID, 0, 6);

        field_ = lv_obj_create(root);
        lv_obj_remove_style_all(field_);
        lv_obj_set_size(field_, FIELD_W, FIELD_H);
        lv_obj_set_style_bg_opa(field_, LV_OPA_COVER, 0);
        lv_obj_align(field_, LV_ALIGN_CENTER, 0, 12);
        lv_obj_set_style_bg_color(field_, lv_color_hex(BadgeTheme::PANEL), 0);
        lv_obj_set_style_border_width(field_, 0, 0);
        lv_obj_set_style_radius(field_, 4, 0);
        // The ball intentionally leaves the field when a point is scored;
        // without this the field scrolls to chase it.
        lv_obj_clear_flag(field_, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_scrollbar_mode(field_, LV_SCROLLBAR_MODE_OFF);

        player_ = lv_obj_create(field_);
        lv_obj_set_size(player_, PAD_W, PAD_H);
        lv_obj_set_style_bg_color(player_, lv_color_hex(BadgeTheme::TEXT), 0);
        lv_obj_set_style_border_width(player_, 0, 0);
        lv_obj_set_style_radius(player_, 2, 0);

        cpu_ = lv_obj_create(field_);
        lv_obj_set_size(cpu_, PAD_W, PAD_H);
        lv_obj_set_style_bg_color(cpu_, lv_color_hex(BadgeTheme::ACCENT_DETAIL), 0);
        lv_obj_set_style_border_width(cpu_, 0, 0);
        lv_obj_set_style_radius(cpu_, 2, 0);

        ball_ = lv_obj_create(field_);
        lv_obj_set_size(ball_, BALL, BALL);
        lv_obj_set_style_bg_color(ball_, lv_color_hex(BadgeTheme::TEXT), 0);
        lv_obj_set_style_border_width(ball_, 0, 0);
        lv_obj_set_style_radius(ball_, LV_RADIUS_CIRCLE, 0);

        msg_ = lv_label_create(root);
        lv_obj_set_style_text_color(msg_, lv_color_hex(BadgeTheme::TEXT_MUTED), 0);
        lv_obj_set_width(msg_, 300);
        lv_obj_set_style_text_align(msg_, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_long_mode(msg_, LV_LABEL_LONG_WRAP);
        lv_obj_align(msg_, LV_ALIGN_BOTTOM_MID, 0, -8);

        reset_match();
    }

    void on_exit() override
    {
        hal_lights_clear();
        hal_lights_show();
        score_ = field_ = player_ = cpu_ = ball_ = msg_ = nullptr;
    }

    void on_button(ButtonEvent ev) override
    {
        if (ev.kind == ButtonEventKind::Pressed && ev.button == Button::A && over_) {
            reset_match();
            return;
        }
        if (ev.kind == ButtonEventKind::Pressed) {
            if (ev.button == Button::Up) up_ = true;
            if (ev.button == Button::Down) down_ = true;
        } else {
            if (ev.button == Button::Up) up_ = false;
            if (ev.button == Button::Down) down_ = false;
        }
    }

    void on_tick() override
    {
        if (over_) return;
        if (up_) player_y_ -= 4;
        if (down_) player_y_ += 4;
        if (player_y_ < 0) player_y_ = 0;
        if (player_y_ > FIELD_H - PAD_H) player_y_ = FIELD_H - PAD_H;

        // CPU tracks the ball with a speed cap so it stays beatable.
        int target = ball_y_ - PAD_H / 2;
        if (cpu_y_ + 2 < target) cpu_y_ += 2;
        else if (cpu_y_ - 2 > target) cpu_y_ -= 2;
        if (cpu_y_ < 0) cpu_y_ = 0;
        if (cpu_y_ > FIELD_H - PAD_H) cpu_y_ = FIELD_H - PAD_H;

        ball_x_ += vx_;
        ball_y_ += vy_;
        if (ball_y_ < 0) { ball_y_ = 0; vy_ = -vy_; }
        if (ball_y_ > FIELD_H - BALL) { ball_y_ = FIELD_H - BALL; vy_ = -vy_; }

        if (vx_ < 0 && ball_x_ <= PAD_X + PAD_W && ball_y_ + BALL >= player_y_ &&
            ball_y_ <= player_y_ + PAD_H) {
            ball_x_ = PAD_X + PAD_W;
            vx_ = -vx_;
            vy_ += (vy_ >= 0 ? 1 : -1);
            if (vy_ > 5) vy_ = 5;
            if (vy_ < -5) vy_ = -5;
        }
        if (vx_ > 0 && ball_x_ + BALL >= CPU_X && ball_y_ + BALL >= cpu_y_ &&
            ball_y_ <= cpu_y_ + PAD_H) {
            ball_x_ = CPU_X - BALL;
            vx_ = -vx_;
        }

        if (ball_x_ < -BALL) {
            cpu_score_++;
            serve(1);
        } else if (ball_x_ > FIELD_W) {
            player_score_++;
            serve(-1);
        }

        if (player_score_ >= 5 || cpu_score_ >= 5) {
            over_ = true;
            if (player_score_ >= 5) {
                wins_++;
                hal_config_set_int("pong", "wins", wins_);
                hal_lights_flash_success();
            } else {
                hal_lights_clear();
                hal_lights_show();
            }
        }

        draw();
    }

private:
    static constexpr int FIELD_W = 240;
    static constexpr int FIELD_H = 112;
    static constexpr int PAD_W = 6;
    static constexpr int PAD_H = 34;
    static constexpr int BALL = 8;
    // Gap between each paddle and its field edge. Flush paddles sit half
    // inside the field's rounded border and read as clipped.
    static constexpr int PAD_X = 4;
    static constexpr int CPU_X = FIELD_W - PAD_W - PAD_X;

    void reset_match()
    {
        player_score_ = cpu_score_ = 0;
        over_ = false;
        up_ = down_ = false;
        player_y_ = cpu_y_ = (FIELD_H - PAD_H) / 2;
        serve(1);
    }

    void serve(int dir)
    {
        ball_x_ = FIELD_W / 2;
        ball_y_ = FIELD_H / 2;
        vx_ = 3 * dir;
        vy_ = (dir > 0) ? 2 : -2;
        draw();
    }

    void draw()
    {
        if (!field_) return;
        lv_obj_set_pos(player_, PAD_X, player_y_);
        lv_obj_set_pos(cpu_, CPU_X, cpu_y_);
        lv_obj_set_pos(ball_, ball_x_, ball_y_);
        if (score_) {
            lv_label_set_text_fmt(score_, "You %d : %d CPU   Wins %d", player_score_,
                                  cpu_score_, wins_);
        }
        if (msg_) {
            lv_label_set_text(msg_, over_ ? (player_score_ >= 5 ? "YOU WIN  A: again" : "YOU LOSE  A: again")
                                           : "Up/Down: paddle   First to 5 wins");
        }
        // LEDs follow the ball: violet near your side, dim near the CPU.
        hal_lights_clear();
        int glow = ball_x_ < FIELD_W / 2 ? 1 : 0;
        for (size_t i = 0; i < LED_COUNT; i++) {
            bool left = (i == 0 || i == 5 || i == 4);
            if ((glow && left) || (!glow && !left)) {
                hal_lights_set(i, 0x66, 0x41, 0x67);
            }
        }
        hal_lights_show();
    }

    lv_obj_t *score_ = nullptr;
    lv_obj_t *field_ = nullptr;
    lv_obj_t *player_ = nullptr;
    lv_obj_t *cpu_ = nullptr;
    lv_obj_t *ball_ = nullptr;
    lv_obj_t *msg_ = nullptr;
    int player_y_ = 0;
    int cpu_y_ = 0;
    int ball_x_ = 0;
    int ball_y_ = 0;
    int vx_ = 0;
    int vy_ = 0;
    int player_score_ = 0;
    int cpu_score_ = 0;
    int wins_ = 0;
    bool over_ = false;
    bool up_ = false;
    bool down_ = false;
};

static PongApp s_instance;
App *app_pong_instance() { return &s_instance; }
