#include "app.h"
#include "hal_lights.h"
#include "app_registry.h"
#include "game_pause.h"
#include "hal_config.h"

#include "esp_random.h"
#include <cmath>
#include <cstdint>
#include <cstring>

static constexpr int kCols = 10;
static constexpr int kRows = 5;
static constexpr int kBrickW = 26;
static constexpr int kBrickH = 11;
static constexpr int kBrickPad = 4;
static constexpr int kBoardLeft = (320 - kCols * (kBrickW + kBrickPad)) / 2;
static constexpr int kBoardTop = 34;
static constexpr int kPaddleW = 52;
static constexpr int kPaddleH = 6;
static constexpr int kPaddleY = 224;
static constexpr int kBallSize = 8;
static constexpr int kScreenH = 240;
static constexpr int kScreenW = 320;

extern "C" const lv_image_dsc_t icon_breakout;

class BreakoutApp : public GameApp {
public:
    const char *name() const override { return "Breakout"; }
    const char *icon() const override { return "BRK"; }
    const char *slug() const override { return "breakout"; }
    const lv_image_dsc_t *icon_image() const override { return &icon_breakout; }
    bool icon_needs_outline() const override { return true; }
    bool wants_wake_lock() const override { return true; }

    void on_enter(lv_obj_t *root) override
    {
        saved_hi_ = hal_config_get_int("breakout", "high_score", 0);
        hi_ = saved_hi_;
        root_ = root;
        lv_obj_set_style_bg_color(root_, lv_color_hex(0x080d18), 0);
        lv_obj_set_style_bg_opa(root_, LV_OPA_COVER, 0);

        score_ = lv_label_create(root);
        lv_obj_set_style_text_font(score_, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(score_, lv_color_hex(BadgeTheme::ACCENT_DETAIL), 0);
        lv_obj_align(score_, LV_ALIGN_TOP_MID, 0, 1);
        game_pause_create_hint(root, LV_ALIGN_TOP_MID, 0, 16);

        paddle_ = lv_obj_create(root);
        lv_obj_remove_style_all(paddle_);
        lv_obj_set_size(paddle_, kPaddleW, kPaddleH);
        lv_obj_set_style_bg_color(paddle_, lv_color_hex(0x36e0c4), 0);
        lv_obj_set_style_bg_opa(paddle_, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(paddle_, 2, 0);

        ball_ = lv_obj_create(root);
        lv_obj_remove_style_all(ball_);
        lv_obj_set_size(ball_, kBallSize, kBallSize);
        lv_obj_set_style_bg_color(ball_, lv_color_hex(0xffffff), 0);
        lv_obj_set_style_bg_opa(ball_, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(ball_, LV_RADIUS_CIRCLE, 0);

        overlay_ = lv_label_create(root);
        lv_obj_set_width(overlay_, 200);
        lv_obj_set_style_text_align(overlay_, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_font(overlay_, &lv_font_montserrat_18, 0);
        lv_obj_set_style_text_color(overlay_, lv_color_hex(0xffd166), 0);
        lv_obj_align(overlay_, LV_ALIGN_CENTER, 0, 0);

        pause_.create(root);

        build_bricks();
        reset_game();
        render();
    }

    void on_exit() override
    {
        persist_high_score();
        lv_obj_set_style_bg_color(root_, lv_color_hex(BadgeTheme::BACKGROUND), 0);
        pause_.on_exit();
        for (int i = 0; i < kCols * kRows; i++) {
            if (bricks_[i]) { lv_obj_delete(bricks_[i]); bricks_[i] = nullptr; }
        }
        paddle_ = ball_ = score_ = overlay_ = root_ = nullptr;
    }

    void on_button(ButtonEvent ev) override
    {
        if (pause_.handle(ev, !dead_)) {
            if (pause_.paused()) checkpoint_progress();
            paddle_dx_ = 0;
            render();
            return;
        }
        if (pause_.paused()) return;
        if (ev.kind == ButtonEventKind::Pressed) {
            if (ev.button == Button::Left)  { paddle_dx_ = -1; }
            if (ev.button == Button::Right) { paddle_dx_ = 1; }
            if (ev.button == Button::A) {
                if (dead_) { restart(); return; }
                if (stuck_) stuck_ = false;
            }
        } else {
            if ((ev.button == Button::Left  && paddle_dx_ < 0) ||
                (ev.button == Button::Right && paddle_dx_ > 0))
                paddle_dx_ = 0;
        }
    }

    void checkpoint_progress() override
    {
        update_high_score();
        persist_high_score();
    }

    void on_tick() override
    {
        if (pause_.paused()) return;
        paddle_x_ += paddle_dx_ * 5;
        if (paddle_x_ < 0) paddle_x_ = 0;
        if (paddle_x_ > kScreenW - kPaddleW) paddle_x_ = kScreenW - kPaddleW;

        if (!dead_ && !stuck_) step_ball();

        render();
    }

private:
    void build_bricks()
    {
        const uint32_t colors[kRows] = { 0xe94f86, 0xe9a83b, 0x29bd97, 0x438bde, 0x9665de };
        for (int r = 0; r < kRows; r++) {
            for (int c = 0; c < kCols; c++) {
                int idx = r * kCols + c;
                lv_obj_t *b = lv_obj_create(root_);
                lv_obj_remove_style_all(b);
                lv_obj_set_size(b, kBrickW, kBrickH);
                lv_obj_set_pos(b,
                    kBoardLeft + c * (kBrickW + kBrickPad),
                    kBoardTop + r * (kBrickH + kBrickPad));
                lv_obj_set_style_bg_color(b, lv_color_hex(colors[r]), 0);
                lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
                lv_obj_set_style_radius(b, 2, 0);
                bricks_[idx] = b;
            }
        }
        generate_layout();
    }

    bool brick_in_layout(int layout, int row, int col) const
    {
        switch (layout) {
            case 0: // classic wall
                return true;
            case 1: // checkerboard
                return ((row + col) & 1) == 0;
            case 2: // narrowing wedge
                return col >= row && col < kCols - row;
            case 3: // staggered bands
                return (row & 1) == 0 || (col % 3) != 1;
            default: // a genuinely different scatter each time this layout appears
                return (esp_random() % 100) < 68;
        }
    }

    void generate_layout()
    {
        constexpr int kLayoutCount = 5;
        int next;
        if (layout_id_ < 0) {
            next = static_cast<int>(esp_random() % kLayoutCount);
        } else {
            next = static_cast<int>(esp_random() % (kLayoutCount - 1));
            if (next >= layout_id_) ++next;
        }
        layout_id_ = next;

        int brick_count = 0;
        for (int row = 0; row < kRows; ++row) {
            for (int col = 0; col < kCols; ++col) {
                int index = row * kCols + col;
                bool present = brick_in_layout(layout_id_, row, col);
                alive_[index] = present ? 1 : 0;
                if (present) ++brick_count;
                if (!bricks_[index]) continue;
                if (present) lv_obj_remove_flag(bricks_[index], LV_OBJ_FLAG_HIDDEN);
                else lv_obj_add_flag(bricks_[index], LV_OBJ_FLAG_HIDDEN);
            }
        }

        // The scatter layout is random per cell; retain a playable board even
        // in the extremely unlikely event that it produces too few bricks.
        if (brick_count < 12) {
            for (int i = 0; i < kCols * kRows; ++i) {
                alive_[i] = 1;
                if (bricks_[i]) lv_obj_remove_flag(bricks_[i], LV_OBJ_FLAG_HIDDEN);
            }
        }
    }

    void restart()
    {
        score_val_ = 0;
        generate_layout();
        reset_game();
    }

    void reset_game()
    {
        paddle_x_ = (kScreenW - kPaddleW) / 2;
        paddle_dx_ = 0;
        ball_x_ = paddle_x_ + kPaddleW / 2 - kBallSize / 2;
        ball_y_ = kPaddleY - kBallSize - 1;
        vx_ = 2.2f;
        vy_ = -2.2f;
        stuck_ = true;
        dead_ = false;
        board_cleared_ = false;
        pause_.reset();
    }

    void step_ball()
    {
        ball_x_ += vx_;
        ball_y_ += vy_;

        if (ball_x_ < 0) { ball_x_ = 0; vx_ = -vx_; }
        if (ball_x_ > kScreenW - kBallSize) { ball_x_ = kScreenW - kBallSize; vx_ = -vx_; }
        if (ball_y_ < kBoardTop) { ball_y_ = kBoardTop; vy_ = -vy_; }

        // Paddle collision.
        if (vy_ > 0 && ball_y_ + kBallSize >= kPaddleY && ball_y_ + kBallSize <= kPaddleY + kPaddleH
            && ball_x_ + kBallSize >= paddle_x_ && ball_x_ <= paddle_x_ + kPaddleW) {
            vy_ = -fabsf(vy_);
            // Angle based on where the ball hit.
            float hit = (ball_x_ + kBallSize / 2.0f) - (paddle_x_ + kPaddleW / 2.0f);
            vx_ = hit * 0.08f;
            if (vx_ > 4) vx_ = 4; 
            if (vx_ < -4) vx_ = -4;
            ball_y_ = kPaddleY - kBallSize - 1;
        }

        // Brick collision (AABB vs AABB).
        for (int i = 0; i < kCols * kRows; i++) {
            if (!alive_[i] || !bricks_[i]) continue;
            int bx = kBoardLeft + (i % kCols) * (kBrickW + kBrickPad);
            int by = kBoardTop + (i / kCols) * (kBrickH + kBrickPad);
            if (ball_x_ + kBallSize > bx && ball_x_ < bx + kBrickW
                && ball_y_ + kBallSize > by && ball_y_ < by + kBrickH) {
                alive_[i] = 0;
                lv_obj_add_flag(bricks_[i], LV_OBJ_FLAG_HIDDEN);
                score_val_ += 10;
                update_high_score();
                // Decide bounce axis by smaller penetration.
                float pen_x = (vx_ > 0) ? (ball_x_ + kBallSize - bx) : (bx + kBrickW - ball_x_);
                float pen_y = (vy_ > 0) ? (ball_y_ + kBallSize - by) : (by + kBrickH - ball_y_);
                if (pen_x < pen_y) vx_ = -vx_; else vy_ = -vy_;
                break;
            }
        }

        if (ball_y_ > kScreenH) {
            dead_ = true;
            update_high_score();
            persist_high_score();
        }

        // Victory check.
        bool any = false;
        for (int i = 0; i < kCols * kRows; i++) if (alive_[i]) { any = true; break; }
        if (!any) {
            persist_high_score();
            hal_lights_flash_success();
            generate_layout();
            reset_game();
            board_cleared_ = true;
        }
    }

    void update_high_score()
    {
        if (score_val_ > hi_) hi_ = score_val_;
    }

    void persist_high_score()
    {
        if (hi_ <= saved_hi_) return;
        hal_config_set_int("breakout", "high_score", hi_);
        saved_hi_ = hi_;
    }

    void render()
    {
        if (paddle_) lv_obj_set_pos(paddle_, (int)paddle_x_, kPaddleY);
        if (ball_) {
            if (stuck_) {
                ball_x_ = paddle_x_ + kPaddleW / 2 - kBallSize / 2;
                ball_y_ = kPaddleY - kBallSize - 1;
            }
            lv_obj_set_pos(ball_, (int)ball_x_, (int)ball_y_);
        }
        if (score_) {
            lv_label_set_text_fmt(score_, "BRICKS  %d     BEST  %d",
                                  score_val_, hi_);
        }
        if (overlay_) {
            if (dead_) lv_label_set_text(overlay_, "Game Over\nA to Retry");
            else if (stuck_) lv_label_set_text(overlay_, board_cleared_
                ? "Board cleared!\nA: next board" : "A to Launch");
            else lv_label_set_text(overlay_, "");
        }
    }

    lv_obj_t *root_ = nullptr;
    lv_obj_t *paddle_ = nullptr;
    lv_obj_t *ball_ = nullptr;
    lv_obj_t *score_ = nullptr;
    lv_obj_t *overlay_ = nullptr;
    lv_obj_t *bricks_[kCols * kRows] = {};
    uint8_t alive_[kCols * kRows] = {};

    float paddle_x_ = 0;
    int paddle_dx_ = 0;
    float ball_x_ = 0, ball_y_ = 0;
    float vx_ = 0, vy_ = 0;
    bool board_cleared_ = false;
    bool stuck_ = true;
    bool dead_ = false;
    int layout_id_ = -1;
    int score_val_ = 0;
    int hi_ = 0;
    int saved_hi_ = 0;
    GamePauseController pause_;
};

static BreakoutApp s_instance;
App *app_breakout_instance() { return &s_instance; }
