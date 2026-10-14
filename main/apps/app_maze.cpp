#include "app.h"
#include "app_registry.h"
#include "hal_accel.h"
#include "hal_config.h"
#include "hal_lights.h"

#include "esp_random.h"
#include "lvgl.h"
#include <cmath>
#include <cstdint>
#include <cstring>

static constexpr int kMaxCols = 17;
static constexpr int kMaxRows = 11;
static constexpr int kMaxCells = kMaxCols * kMaxRows;
static constexpr float kBallRadius = 5.0f;
static constexpr float kMaxBallSpeed = 4.0f;

struct StageConfig {
    int cols;
    int rows;
    int cell_px;
};

static constexpr StageConfig kStages[] = {
    { 7,  5, 28 },
    { 9,  7, 24 },
    { 11, 9, 20 },
    { 13, 9, 18 },
    { 15, 11, 16 },
    { 17, 11, 15 },
};
static constexpr int kStageCount = sizeof(kStages) / sizeof(kStages[0]);

extern "C" const lv_image_dsc_t icon_maze;

class MazeApp : public GameApp {
public:
    const char *name() const override { return "TiltMaze"; }
    const char *icon() const override { return "MZE"; }
    const char *slug() const override { return "maze"; }
    const lv_image_dsc_t *icon_image() const override { return &icon_maze; }
    bool icon_needs_outline() const override { return true; }

    void on_enter(lv_obj_t *root) override
    {
        root_ = root;
        completed_count_ = hal_config_get_int("maze", "completed", 0);
        if (completed_count_ < 0) completed_count_ = 0;
        stage_ = completed_count_;
        if (stage_ >= kStageCount) stage_ = kStageCount - 1;

        lv_obj_t *title = lv_label_create(root);
        lv_label_set_text(title, "Tilt Maze");
        lv_obj_set_style_text_color(title, lv_color_hex(BadgeTheme::TEXT), 0);
        lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
        lv_obj_align(title, LV_ALIGN_TOP_LEFT, 7, 4);

        stage_label_ = lv_label_create(root);
        lv_obj_set_style_text_font(stage_label_, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(stage_label_, lv_color_hex(BadgeTheme::TEXT_SOFT), 0);
        lv_obj_align(stage_label_, LV_ALIGN_TOP_MID, 0, 5);

        timer_label_ = lv_label_create(root);
        lv_obj_set_style_text_font(timer_label_, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(timer_label_, lv_color_hex(BadgeTheme::ACCENT_DETAIL), 0);
        lv_obj_align(timer_label_, LV_ALIGN_TOP_RIGHT, -7, 4);

        hint_label_ = lv_label_create(root);
        lv_label_set_text(hint_label_, "Up/Down Stage   A New   Start Reset");
        lv_obj_set_style_text_font(hint_label_, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(hint_label_, lv_color_hex(BadgeTheme::TEXT_MUTED), 0);
        lv_obj_align(hint_label_, LV_ALIGN_BOTTOM_MID, 0, -1);

        ball_ = lv_obj_create(root);
        lv_obj_remove_style_all(ball_);
        lv_obj_set_size(ball_, 10, 10);
        lv_obj_set_style_bg_color(ball_, lv_color_hex(0xff5c8a), 0);
        lv_obj_set_style_bg_opa(ball_, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(ball_, LV_RADIUS_CIRCLE, 0);

        overlay_ = lv_label_create(root);
        lv_obj_set_width(overlay_, 260);
        lv_obj_set_style_text_align(overlay_, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_font(overlay_, &lv_font_montserrat_18, 0);
        lv_obj_set_style_text_color(overlay_, lv_color_hex(0xffd166), 0);
        lv_obj_set_style_bg_color(overlay_, lv_color_hex(BadgeTheme::BACKGROUND), 0);
        lv_obj_set_style_bg_opa(overlay_, LV_OPA_90, 0);
        lv_obj_set_style_pad_all(overlay_, 10, 0);
        lv_obj_set_style_radius(overlay_, 8, 0);
        lv_obj_align(overlay_, LV_ALIGN_CENTER, 0, 8);

        regenerate_stage();
    }

    void on_exit() override
    {
        clear_board_objects();
        hal_lights_clear();
        hal_lights_show();
        root_ = ball_ = timer_label_ = stage_label_ = hint_label_ = overlay_ = nullptr;
    }

    void on_button(ButtonEvent ev) override
    {
        if (ev.kind != ButtonEventKind::Pressed) return;
        if (ev.button == Button::Up && stage_ + 1 < kStageCount) {
            ++stage_;
            regenerate_stage();
        } else if (ev.button == Button::Down && stage_ > 0) {
            --stage_;
            regenerate_stage();
        } else if (ev.button == Button::A) {
            if (done_ && stage_ + 1 < kStageCount) ++stage_;
            regenerate_stage();
        } else if (ev.button == Button::Start) {
            reset_game();
        }
    }

    void on_tick() override
    {
        if (!done_) {
            AccelSample s;
            if (hal_accel_read(&s) == ESP_OK) {
                // Scale accel (mg) into acceleration (px/tick²). Tuning constant.
                float ax = -s.x_mg * 0.00012f;
                float ay =  s.y_mg * 0.00012f;
                vx_ = vx_ * 0.985f + ax;
                vy_ = vy_ * 0.985f + ay;
                if (vx_ > kMaxBallSpeed) vx_ = kMaxBallSpeed;
                if (vx_ < -kMaxBallSpeed) vx_ = -kMaxBallSpeed;
                if (vy_ > kMaxBallSpeed) vy_ = kMaxBallSpeed;
                if (vy_ < -kMaxBallSpeed) vy_ = -kMaxBallSpeed;
                step_ball();
            }
            elapsed_ticks_++;
            render();
        }
    }

private:
    struct Cell {
        uint8_t row;
        uint8_t col;
    };

    void configure_stage()
    {
        const StageConfig &config = kStages[stage_];
        cols_ = config.cols;
        rows_ = config.rows;
        cell_px_ = config.cell_px;
        board_x_ = (320 - cols_ * cell_px_) / 2;
        board_y_ = 34 + (184 - rows_ * cell_px_) / 2;
    }

    void generate_maze()
    {
        memset(maze_, 1, sizeof(maze_));

        Cell stack[kMaxCells];
        int stack_top = 0;
        stack[0] = {1, 1};
        maze_[1][1] = 0;

        static constexpr int kDirections[4][2] = {
            {-2, 0}, {2, 0}, {0, -2}, {0, 2},
        };

        while (stack_top >= 0) {
            Cell current = stack[stack_top];
            Cell choices[4];
            int choice_count = 0;
            for (const auto &direction : kDirections) {
                int row = current.row + direction[0];
                int col = current.col + direction[1];
                if (row <= 0 || row >= rows_ - 1
                    || col <= 0 || col >= cols_ - 1) continue;
                if (maze_[row][col] == 1) {
                    choices[choice_count++] = {
                        static_cast<uint8_t>(row), static_cast<uint8_t>(col)};
                }
            }

            if (choice_count == 0) {
                --stack_top;
                continue;
            }

            Cell next = choices[esp_random() % choice_count];
            maze_[(current.row + next.row) / 2]
                 [(current.col + next.col) / 2] = 0;
            maze_[next.row][next.col] = 0;
            stack[++stack_top] = next;
        }

        // Put the goal at the passage farthest from the start. This turns the
        // generated tree into a meaningful route instead of an arbitrary exit.
        int16_t distance[kMaxRows][kMaxCols];
        for (int row = 0; row < kMaxRows; ++row)
            for (int col = 0; col < kMaxCols; ++col) distance[row][col] = -1;

        Cell queue[kMaxCells];
        int head = 0;
        int tail = 0;
        queue[tail++] = {1, 1};
        distance[1][1] = 0;
        Cell farthest = {1, 1};
        static constexpr int kSteps[4][2] = {
            {-1, 0}, {1, 0}, {0, -1}, {0, 1},
        };
        while (head < tail) {
            Cell current = queue[head++];
            if (distance[current.row][current.col]
                > distance[farthest.row][farthest.col]) {
                farthest = current;
            }
            for (const auto &step : kSteps) {
                int row = current.row + step[0];
                int col = current.col + step[1];
                if (row < 0 || row >= rows_ || col < 0 || col >= cols_) continue;
                if (maze_[row][col] == 1 || distance[row][col] >= 0) continue;
                distance[row][col] = distance[current.row][current.col] + 1;
                queue[tail++] = {
                    static_cast<uint8_t>(row), static_cast<uint8_t>(col)};
            }
        }

        goal_r_ = farthest.row;
        goal_c_ = farthest.col;
        maze_[goal_r_][goal_c_] = 2;
    }

    void clear_board_objects()
    {
        for (int i = 0; i < wall_count_; ++i) {
            if (walls_[i]) lv_obj_delete(walls_[i]);
            walls_[i] = nullptr;
        }
        wall_count_ = 0;
        if (goal_) {
            lv_obj_delete(goal_);
            goal_ = nullptr;
        }
    }

    void build_walls()
    {
        clear_board_objects();
        for (int r = 0; r < rows_; r++) {
            for (int c = 0; c < cols_; c++) {
                if (maze_[r][c] != 1) continue;
                lv_obj_t *o = lv_obj_create(root_);
                lv_obj_remove_style_all(o);
                lv_obj_set_size(o, cell_px_, cell_px_);
                lv_obj_set_pos(o, board_x_ + c * cell_px_,
                               board_y_ + r * cell_px_);
                lv_obj_set_style_bg_color(o, lv_color_hex(BadgeTheme::BORDER), 0);
                lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
                lv_obj_set_style_radius(o, 1, 0);
                walls_[wall_count_++] = o;
            }
        }

        goal_ = lv_obj_create(root_);
        lv_obj_remove_style_all(goal_);
        lv_obj_set_size(goal_, cell_px_ - 2, cell_px_ - 2);
        lv_obj_set_pos(goal_, board_x_ + goal_c_ * cell_px_ + 1,
                       board_y_ + goal_r_ * cell_px_ + 1);
        lv_obj_set_style_bg_color(goal_, lv_color_hex(0x00ffa8), 0);
        lv_obj_set_style_bg_opa(goal_, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(goal_, 2, 0);

        if (ball_) lv_obj_move_foreground(ball_);
        if (overlay_) lv_obj_move_foreground(overlay_);
        if (hint_label_) lv_obj_move_foreground(hint_label_);
    }

    void regenerate_stage()
    {
        configure_stage();
        generate_maze();
        build_walls();
        reset_game();
        update_stage_label();
    }

    void update_stage_label()
    {
        if (stage_label_) {
            lv_label_set_text_fmt(stage_label_, "Stage %d/%d  Done %d",
                stage_ + 1, kStageCount, completed_count_);
            lv_obj_align(stage_label_, LV_ALIGN_TOP_MID, 0, 5);
        }
    }

    void reset_game()
    {
        x_ = cell_px_ + cell_px_ / 2.0f;
        y_ = cell_px_ + cell_px_ / 2.0f;
        vx_ = vy_ = 0;
        elapsed_ticks_ = 0;
        done_ = false;
        if (overlay_) lv_obj_add_flag(overlay_, LV_OBJ_FLAG_HIDDEN);
        hal_lights_clear();
        hal_lights_show();
        render();
    }

    bool wall_at(int r, int c) const
    {
        if (r < 0 || r >= rows_ || c < 0 || c >= cols_) return true;
        return maze_[r][c] == 1;
    }

    bool collides(float x, float y) const
    {
        // Stay a fraction inside the ball's visual edge so resting against a
        // wall does not alternate between colliding and non-colliding frames.
        constexpr float kHitRadius = kBallRadius - 0.15f;
        int left = static_cast<int>(floorf((x - kHitRadius) / cell_px_));
        int right = static_cast<int>(floorf((x + kHitRadius) / cell_px_));
        int top = static_cast<int>(floorf((y - kHitRadius) / cell_px_));
        int bottom = static_cast<int>(floorf((y + kHitRadius) / cell_px_));
        for (int row = top; row <= bottom; ++row) {
            for (int col = left; col <= right; ++col) {
                if (wall_at(row, col)) return true;
            }
        }
        return false;
    }

    float move_until_wall(float start, float delta, float fixed,
                          bool moving_x) const
    {
        float target = start + delta;
        if (!(moving_x ? collides(target, fixed) : collides(fixed, target))) {
            return target;
        }

        // Find the last safe point along this small movement. Snapping to the
        // contact boundary prevents the old clip-then-stick behavior while
        // preserving motion along the other axis.
        float safe = 0.0f;
        float blocked = 1.0f;
        for (int i = 0; i < 7; ++i) {
            float fraction = (safe + blocked) * 0.5f;
            float candidate = start + delta * fraction;
            bool hit = moving_x ? collides(candidate, fixed)
                                : collides(fixed, candidate);
            if (hit) blocked = fraction;
            else safe = fraction;
        }
        return start + delta * safe;
    }

    void step_ball()
    {
        float max_motion = fmaxf(fabsf(vx_), fabsf(vy_));
        int substeps = static_cast<int>(ceilf(max_motion / 2.0f));
        if (substeps < 1) substeps = 1;
        float dx = vx_ / substeps;
        float dy = vy_ / substeps;

        for (int i = 0; i < substeps; ++i) {
            float moved_x = move_until_wall(x_, dx, y_, true);
            if (fabsf(moved_x - (x_ + dx)) > 0.001f) vx_ = 0;
            x_ = moved_x;

            float moved_y = move_until_wall(y_, dy, x_, false);
            if (fabsf(moved_y - (y_ + dy)) > 0.001f) vy_ = 0;
            y_ = moved_y;
        }

        // Check goal.
        int cr = static_cast<int>(y_ / cell_px_);
        int cc = static_cast<int>(x_ / cell_px_);
        if (cr == goal_r_ && cc == goal_c_) {
            done_ = true;
            ++completed_count_;
            hal_config_set_int("maze", "completed", completed_count_);
            update_stage_label();
            int tenths = elapsed_ticks_ / 5;
            if (overlay_) {
                lv_label_set_text_fmt(overlay_,
                    "Stage %d Complete\n%d.%ds\nA for Next Maze",
                    stage_ + 1, tenths / 10, tenths % 10);
                lv_obj_clear_flag(overlay_, LV_OBJ_FLAG_HIDDEN);
                lv_obj_move_foreground(overlay_);
            }
            hal_lights_flash_success();
        }
    }

    void render()
    {
        if (ball_) lv_obj_set_pos(ball_, board_x_ + static_cast<int>(x_ - kBallRadius),
                                  board_y_ + static_cast<int>(y_ - kBallRadius));
        int tenths = elapsed_ticks_ / 5;
        if (timer_label_) lv_label_set_text_fmt(timer_label_, "%d.%ds",
            tenths / 10, tenths % 10);
    }

    lv_obj_t *root_ = nullptr;
    lv_obj_t *ball_ = nullptr;
    lv_obj_t *timer_label_ = nullptr;
    lv_obj_t *stage_label_ = nullptr;
    lv_obj_t *hint_label_ = nullptr;
    lv_obj_t *overlay_ = nullptr;
    lv_obj_t *goal_ = nullptr;
    lv_obj_t *walls_[kMaxCells] = {};
    uint8_t maze_[kMaxRows][kMaxCols] = {};
    int wall_count_ = 0;
    int cols_ = 7;
    int rows_ = 5;
    int cell_px_ = 28;
    int board_x_ = 0;
    int board_y_ = 0;
    int stage_ = 0;
    int completed_count_ = 0;
    float x_ = 0, y_ = 0;
    float vx_ = 0, vy_ = 0;
    int goal_r_ = 0, goal_c_ = 0;
    int elapsed_ticks_ = 0;
    bool done_ = false;
};

static MazeApp s_instance;
App *app_maze_instance() { return &s_instance; }
