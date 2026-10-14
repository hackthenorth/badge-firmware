#include "app.h"
#include "hal_lights.h"
#include "app_registry.h"
#include "game_pause.h"
#include "hal_config.h"
#include "snake_body.h"

#include "esp_random.h"
#include "lvgl.h"

static constexpr int kCellPx = 12;
static constexpr int kCols = Snake::kCols;   // 24*12 = 288 px
static constexpr int kRows = Snake::kRows;   // 16*12 = 192 px (rest for header)
static constexpr int kBoardX = (320 - kCols * kCellPx) / 2;
static constexpr int kBoardY = 42;
static constexpr int kMaxLen = kCols * kRows;
static constexpr int kTurnQueueCapacity = 2;
static constexpr int kStepIntervalTicks = 8; // 160 ms at the 50 Hz app tick

extern "C" const lv_image_dsc_t icon_snake;

class SnakeApp : public GameApp {
public:
    const char *name() const override { return "Snake"; }
    const char *icon() const override { return "SNK"; }
    const char *slug() const override { return "snake"; }
    const lv_image_dsc_t *icon_image() const override { return &icon_snake; }
    bool wants_wake_lock() const override { return true; }

    AppAccess access() const override { return AppAccess::Default; }

    void on_enter(lv_obj_t *root) override
    {
        hi_ = hal_config_get_int("snake", "high_score", 0);
        root_ = root;

        lv_obj_t *title = lv_label_create(root);
        lv_label_set_text(title, "Snake");
        lv_obj_set_style_text_color(title, lv_color_hex(BadgeTheme::TEXT), 0);
        lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
        lv_obj_align(title, LV_ALIGN_TOP_LEFT, 10, 5);

        score_ = lv_label_create(root);
        lv_obj_set_style_text_font(score_, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(score_, lv_color_hex(BadgeTheme::ACCENT_DETAIL), 0);
        lv_obj_align(score_, LV_ALIGN_TOP_RIGHT, -10, 6);
        lv_obj_t *pause_hint = game_pause_create_hint(root, LV_ALIGN_TOP_LEFT, 10, 24);
        lv_obj_set_style_text_font(pause_hint, &lv_font_montserrat_12, 0);

        board_bg_ = lv_obj_create(root);
        lv_obj_remove_style_all(board_bg_);
        lv_obj_set_size(board_bg_, kCols * kCellPx + 4, kRows * kCellPx + 4);
        lv_obj_set_pos(board_bg_, kBoardX - 2, kBoardY - 2);
        lv_obj_set_style_bg_color(board_bg_, lv_color_hex(BadgeTheme::SURFACE), 0);
        lv_obj_set_style_bg_opa(board_bg_, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(board_bg_, 0, 0);
        lv_obj_set_style_border_width(board_bg_, 1, 0);
        lv_obj_set_style_border_color(board_bg_, lv_color_hex(BadgeTheme::BORDER), 0);
        lv_obj_clear_flag(board_bg_, LV_OBJ_FLAG_SCROLLABLE);

        overlay_ = lv_label_create(root);
        lv_obj_set_width(overlay_, 240);
        lv_obj_set_style_text_align(overlay_, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_font(overlay_, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(overlay_, lv_color_hex(0xff5c8a), 0);
        lv_obj_set_style_bg_color(overlay_, lv_color_hex(BadgeTheme::BACKGROUND), 0);
        lv_obj_set_style_bg_opa(overlay_, LV_OPA_COVER, 0);
        lv_obj_set_style_pad_all(overlay_, 16, 0);
        lv_obj_set_style_radius(overlay_, 8, 0);
        lv_obj_align(overlay_, LV_ALIGN_CENTER, 0, 0);
        lv_label_set_text(overlay_, "");

        pause_.create(root);

        reset_game();
        render_full();
    }

    void on_exit() override
    {
        pause_.on_exit();
        root_ = board_bg_ = score_ = overlay_ = food_obj_ = nullptr;
        for (int i = 0; i < kMaxLen; i++) seg_objs_[i] = nullptr;
    }

    void on_button(ButtonEvent ev) override
    {
        if (pause_.handle(ev, !dead_)) {
            if (pause_.paused()) checkpoint_progress();
            return;
        }
        if (pause_.paused()) return;
        if (ev.kind != ButtonEventKind::Pressed) return;
        if (dead_) {
            if (ev.button == Button::A || ev.button == Button::Start) {
                reset_game();
                render_full();
            }
            return;
        }
        int ndx = dx_, ndy = dy_;
        switch (ev.button) {
            case Button::Up:    ndx = 0; ndy = -1; break;
            case Button::Down:  ndx = 0; ndy = 1; break;
            case Button::Left:  ndx = -1; ndy = 0; break;
            case Button::Right: ndx = 1; ndy = 0; break;
            default: return;
        }
        // Buffer quick corners across movement steps. Validate against the
        // last queued direction so an input sequence can never schedule a
        // future U-turn into the snake's neck.
        const Pt base = turn_count_ > 0
            ? turn_queue_[turn_count_ - 1]
            : Pt { dx_, dy_ };
        if (ndx == base.x && ndy == base.y) return;
        if (ndx == -base.x && ndy == -base.y && body_.length() > 1) return;
        if (turn_count_ >= kTurnQueueCapacity) return;
        turn_queue_[turn_count_++] = { ndx, ndy };
    }

    void checkpoint_progress() override
    {
        const int score = body_.length() - 3;
        if (score <= hi_) return;
        hi_ = score;
        hal_config_set_int("snake", "high_score", hi_);
        refresh_score();
    }

    void on_tick() override
    {
        if (dead_ || pause_.paused()) return;
        if (++tick_counter_ < kStepIntervalTicks) return;
        tick_counter_ = 0;

        if (turn_count_ > 0) {
            dx_ = turn_queue_[0].x;
            dy_ = turn_queue_[0].y;
            for (int i = 1; i < turn_count_; i++)
                turn_queue_[i - 1] = turn_queue_[i];
            turn_count_--;
        }

        const Pt head = body_.head();
        const Pt next = { head.x + dx_, head.y + dy_ };
        const int old_tail = Snake::cell_index(body_.tail());
        const bool ate = next == food_;
        if (!body_.advance(next, ate)) { die(); return; }

        // All body cells keep their pixels. Recycle only the departing tail
        // object as the new head, or allocate one extra object when growing.
        lv_obj_t *new_head;
        if (ate) {
            new_head = make_seg();
        } else {
            new_head = seg_objs_[old_tail];
            seg_objs_[old_tail] = nullptr;
        }
        seg_objs_[Snake::cell_index(next)] = new_head;
        move_to(new_head, next.x, next.y);

        if (ate) {
            if ((body_.length() - 3) % 10 == 0) hal_lights_flash_success();
            if (body_.full()) {
                finish_game("Board cleared!");
                if (food_obj_) { lv_obj_delete(food_obj_); food_obj_ = nullptr; }
            } else {
                place_food();
                move_to(food_obj_, food_.x, food_.y);
            }
            refresh_score();
        }
    }

private:
    using Pt = Snake::Cell;

    void reset_game()
    {
        dead_ = false;
        tick_counter_ = 0;
        body_.reset();
        dx_ = 1; dy_ = 0;
        turn_count_ = 0;
        pause_.reset();
        place_food();
        if (overlay_) lv_obj_add_flag(overlay_, LV_OBJ_FLAG_HIDDEN);
    }

    void refresh_score()
    {
        const int score = body_.length() - 3;
        const int best = score > hi_ ? score : hi_;
        if (score_) lv_label_set_text_fmt(score_, "Score %d  Hi %d", score, best);
    }

    void die() { finish_game("Game over"); }

    void finish_game(const char *message)
    {
        dead_ = true;
        int score = body_.length() - 3;
        if (score > hi_ || body_.full()) hal_lights_flash_success();
        checkpoint_progress();
        if (overlay_) {
            lv_label_set_text_fmt(overlay_, "%s\nA to restart", message);
            lv_obj_clear_flag(overlay_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(overlay_);
        }
        refresh_score();
    }

    void place_food()
    {
        food_ = body_.free_cell(esp_random());
    }

    lv_obj_t *make_seg()
    {
        if (!root_) return nullptr;
        lv_obj_t *o = lv_obj_create(root_);
        lv_obj_remove_style_all(o);
        lv_obj_set_size(o, kCellPx - 1, kCellPx - 1);
        lv_obj_set_style_bg_color(o, lv_color_hex(0x00ffa8), 0);
        lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(o, 2, 0);
        return o;
    }

    void move_to(lv_obj_t *o, int cx, int cy)
    {
        if (!o) return;
        lv_obj_set_pos(o, kBoardX + cx * kCellPx, kBoardY + cy * kCellPx);
    }

    void clear_board_objs()
    {
        for (int i = 0; i < kMaxLen; i++)
            if (seg_objs_[i]) { lv_obj_delete(seg_objs_[i]); seg_objs_[i] = nullptr; }
        if (food_obj_) { lv_obj_delete(food_obj_); food_obj_ = nullptr; }
    }

    void render_full()
    {
        clear_board_objs();
        for (int i = 0; i < body_.length(); i++) {
            const Pt cell = body_.segment(i);
            lv_obj_t *obj = make_seg();
            seg_objs_[Snake::cell_index(cell)] = obj;
            move_to(obj, cell.x, cell.y);
        }
        food_obj_ = lv_obj_create(root_);
        lv_obj_remove_style_all(food_obj_);
        lv_obj_set_size(food_obj_, kCellPx - 1, kCellPx - 1);
        lv_obj_set_style_bg_color(food_obj_, lv_color_hex(0xff5c8a), 0);
        lv_obj_set_style_bg_opa(food_obj_, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(food_obj_, LV_RADIUS_CIRCLE, 0);
        move_to(food_obj_, food_.x, food_.y);
        refresh_score();
    }

    lv_obj_t *root_ = nullptr;
    lv_obj_t *board_bg_ = nullptr;
    lv_obj_t *score_ = nullptr;
    lv_obj_t *overlay_ = nullptr;
    lv_obj_t *food_obj_ = nullptr;
    lv_obj_t *seg_objs_[kMaxLen] = {};

    Snake::Body body_;
    Pt food_ = {};
    int dx_ = 1, dy_ = 0;
    Pt turn_queue_[kTurnQueueCapacity] = {};
    int turn_count_ = 0;
    int tick_counter_ = 0;
    bool dead_ = false;
    int hi_ = 0;
    GamePauseController pause_;
};

static SnakeApp s_instance;
App *app_snake_instance() { return &s_instance; }
