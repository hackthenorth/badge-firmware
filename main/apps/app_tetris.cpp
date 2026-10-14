#include "app.h"
#include "hal_lights.h"
#include "app_registry.h"
#include "game_pause.h"
#include "hal_config.h"

#include "esp_random.h"
#include "lvgl.h"
#include <cstdint>
#include <cstring>

static constexpr int kCols = 10;
static constexpr int kRows = 18;
static constexpr int kCellPx = 12;
static constexpr int kBoardX = (320 - kCols * kCellPx) / 2 - 40;
static constexpr int kBoardY = 18;
static constexpr int kSidebarX = kBoardX + kCols * kCellPx + 10;

// Each piece: 4 rotations × 4 cells × {dx,dy}.
// Stored as offsets around a pivot.
struct Piece {
    int8_t cells[4][4][2]; // rot, cell, {x,y}
    uint32_t color;
};

static const Piece kPieces[7] = {
    // I
    { { {{-1,0},{0,0},{1,0},{2,0}}, {{0,-1},{0,0},{0,1},{0,2}}, {{-1,0},{0,0},{1,0},{2,0}}, {{0,-1},{0,0},{0,1},{0,2}} }, BadgeTheme::ACCENT_DETAIL },
    // O
    { { {{0,0},{1,0},{0,1},{1,1}}, {{0,0},{1,0},{0,1},{1,1}}, {{0,0},{1,0},{0,1},{1,1}}, {{0,0},{1,0},{0,1},{1,1}} }, 0xffd166 },
    // T
    { { {{-1,0},{0,0},{1,0},{0,1}}, {{0,-1},{0,0},{0,1},{-1,0}}, {{-1,0},{0,0},{1,0},{0,-1}}, {{0,-1},{0,0},{0,1},{1,0}} }, 0x8a63ff },
    // S
    { { {{0,0},{1,0},{-1,1},{0,1}}, {{0,-1},{0,0},{1,0},{1,1}}, {{0,0},{1,0},{-1,1},{0,1}}, {{0,-1},{0,0},{1,0},{1,1}} }, 0x00ffa8 },
    // Z
    { { {{-1,0},{0,0},{0,1},{1,1}}, {{1,-1},{0,0},{1,0},{0,1}}, {{-1,0},{0,0},{0,1},{1,1}}, {{1,-1},{0,0},{1,0},{0,1}} }, 0xff5c8a },
    // J
    { { {{-1,-1},{-1,0},{0,0},{1,0}}, {{0,-1},{1,-1},{0,0},{0,1}}, {{-1,0},{0,0},{1,0},{1,1}}, {{0,-1},{0,0},{-1,1},{0,1}} }, 0x5c8aff },
    // L
    { { {{-1,0},{0,0},{1,0},{1,-1}}, {{0,-1},{0,0},{0,1},{1,1}}, {{-1,0},{0,0},{1,0},{-1,1}}, {{-1,-1},{0,-1},{0,0},{0,1}} }, 0xffa155 },
};

extern "C" const lv_image_dsc_t icon_tetris;

class TetrisApp : public GameApp {
public:
    const char *name() const override { return "Tetris"; }
    const char *icon() const override { return "TTR"; }
    const char *slug() const override { return "tetris"; }
    const lv_image_dsc_t *icon_image() const override { return &icon_tetris; }
    bool wants_wake_lock() const override { return true; }

    void on_enter(lv_obj_t *root) override
    {
        root_ = root;
        saved_hi_ = hal_config_get_int("tetris", "high_score", 0);
        hi_ = saved_hi_;

        lv_obj_t *title = lv_label_create(root);
        lv_label_set_text(title, "Tetris");
        lv_obj_set_style_text_color(title, lv_color_hex(BadgeTheme::TEXT), 0);
        lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
        lv_obj_align(title, LV_ALIGN_TOP_LEFT, 10, 2);
        game_pause_create_hint(root, LV_ALIGN_TOP_RIGHT, -8, 4);

        // Draw the entire board through one object. The old implementation
        // created and restyled 180 LVGL children whenever a piece moved.
        board_canvas_ = lv_obj_create(root);
        lv_obj_remove_style_all(board_canvas_);
        lv_obj_set_size(board_canvas_, kCols * kCellPx + 2,
                        kRows * kCellPx + 2);
        lv_obj_set_pos(board_canvas_, kBoardX - 1, kBoardY - 1);
        lv_obj_set_style_bg_color(board_canvas_, lv_color_hex(BadgeTheme::SURFACE), 0);
        lv_obj_set_style_bg_opa(board_canvas_, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(board_canvas_, 1, 0);
        lv_obj_set_style_border_color(board_canvas_, lv_color_hex(BadgeTheme::BORDER), 0);
        lv_obj_clear_flag(board_canvas_, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(board_canvas_, board_draw_event,
                            LV_EVENT_DRAW_MAIN, this);

        // Sidebar readouts.
        score_label_ = lv_label_create(root);
        lv_obj_set_width(score_label_, 124);
        lv_obj_set_style_text_font(score_label_, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(score_label_, lv_color_hex(BadgeTheme::ACCENT_DETAIL), 0);
        lv_obj_set_pos(score_label_, kSidebarX, kBoardY + 10);

        lines_label_ = lv_label_create(root);
        lv_obj_set_style_text_font(lines_label_, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(lines_label_, lv_color_hex(BadgeTheme::TEXT), 0);
        lv_obj_set_pos(lines_label_, kSidebarX, kBoardY + 58);

        hi_label_ = lv_label_create(root);
        lv_obj_set_style_text_font(hi_label_, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(hi_label_, lv_color_hex(BadgeTheme::TEXT_MUTED), 0);
        lv_obj_set_width(hi_label_, 124);
        lv_obj_set_pos(hi_label_, kSidebarX, kBoardY + 82);

        lv_obj_t *hold_label = lv_label_create(root);
        lv_label_set_text(hold_label, "Hold");
        lv_obj_set_style_text_font(hold_label, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(hold_label, lv_color_hex(BadgeTheme::TEXT), 0);
        lv_obj_set_pos(hold_label, kSidebarX, kBoardY + 126);

        lv_obj_t *next_label = lv_label_create(root);
        lv_label_set_text(next_label, "Next");
        lv_obj_set_style_text_font(next_label, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(next_label, lv_color_hex(BadgeTheme::TEXT), 0);
        lv_obj_set_pos(next_label, kSidebarX + 64, kBoardY + 126);

        preview_canvas_ = lv_obj_create(root);
        lv_obj_remove_style_all(preview_canvas_);
        lv_obj_set_size(preview_canvas_, 124, 38);
        lv_obj_set_pos(preview_canvas_, kSidebarX, kBoardY + 145);
        lv_obj_clear_flag(preview_canvas_, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(preview_canvas_, preview_draw_event,
                            LV_EVENT_DRAW_MAIN, this);

        lv_obj_t *controls = lv_label_create(root);
        lv_label_set_text(controls, "B: Hold");
        lv_obj_set_style_text_font(controls, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(controls, lv_color_hex(BadgeTheme::TEXT_MUTED), 0);
        lv_obj_set_pos(controls, kSidebarX, kBoardY + 184);

        game_over_card_ = lv_obj_create(root);
        lv_obj_remove_style_all(game_over_card_);
        lv_obj_set_size(game_over_card_, 250, 132);
        lv_obj_align(game_over_card_, LV_ALIGN_CENTER, 0, 8);
        lv_obj_set_style_radius(game_over_card_, 12, 0);
        lv_obj_set_style_bg_color(game_over_card_,
                                  lv_color_hex(BadgeTheme::BACKGROUND), 0);
        lv_obj_set_style_bg_opa(game_over_card_, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(game_over_card_, 2, 0);
        lv_obj_set_style_border_color(game_over_card_,
                                      lv_color_hex(0xffd166), 0);
        lv_obj_clear_flag(game_over_card_, LV_OBJ_FLAG_SCROLLABLE);

        game_over_text_ = lv_label_create(game_over_card_);
        lv_obj_set_width(game_over_text_, 226);
        lv_obj_set_style_text_align(game_over_text_, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_font(game_over_text_, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(game_over_text_,
                                    lv_color_hex(BadgeTheme::TEXT), 0);
        lv_obj_center(game_over_text_);
        lv_obj_add_flag(game_over_card_, LV_OBJ_FLAG_HIDDEN);

        pause_.create(root);

        reset_game();
        render();
    }

    void on_exit() override
    {
        persist_high_score();
        pause_.on_exit();
        root_ = board_canvas_ = preview_canvas_ = score_label_ = lines_label_
            = hi_label_ = game_over_card_ = game_over_text_ = nullptr;
    }

    void on_button(ButtonEvent ev) override
    {
        if (pause_.handle(ev, !dead_, false)) {
            if (pause_.paused()) checkpoint_progress();
            render();
            return;
        }
        if (pause_.paused()) return;
        if (ev.kind != ButtonEventKind::Pressed) return;
        if (dead_) {
            if (ev.button == Button::A) {
                reset_game();
                render();
            }
            return;
        }
        switch (ev.button) {
            case Button::Left:  try_move(-1, 0); break;
            case Button::Right: try_move(1, 0); break;
            case Button::Down:  soft_drop(); break;
            case Button::Up:    try_rotate(); break;
            case Button::A:     hard_drop(); break;
            case Button::B:     hold_piece(); break;
            default: break;
        }
        render();
    }

    void checkpoint_progress() override
    {
        update_high_score();
        persist_high_score();
    }

    void on_tick() override
    {
        if (dead_ || pause_.paused()) return;
        if (++grav_counter_ < grav_interval()) return;
        grav_counter_ = 0;
        if (!try_move(0, 1)) {
            settle_piece(0);
        }
        render();
    }

private:
    int grav_interval() const
    {
        int base = 24 - lines_ / 10 * 2;
        if (base < 4) base = 4;
        return base;
    }

    void reset_game()
    {
        memset(board_, 0, sizeof(board_));
        score_val_ = 0;
        lines_ = 0;
        dead_ = false;
        pause_.reset();
        grav_counter_ = 0;
        bag_pos_ = 7;
        hold_piece_ = -1;
        hold_used_ = false;
        next_piece_ = take_bag_piece();
        spawn_piece();
    }

    void refill_bag()
    {
        for (int i = 0; i < 7; ++i) bag_[i] = i;
        for (int i = 6; i > 0; --i) {
            int j = static_cast<int>(esp_random() % static_cast<uint32_t>(i + 1));
            int tmp = bag_[i];
            bag_[i] = bag_[j];
            bag_[j] = tmp;
        }
        bag_pos_ = 0;
    }

    int take_bag_piece()
    {
        if (bag_pos_ >= 7) refill_bag();
        return bag_[bag_pos_++];
    }

    void reset_active_piece()
    {
        rot_ = 0;
        px_ = kCols / 2;
        py_ = 1;
        if (!fits(px_, py_, rot_)) {
            dead_ = true;
            update_high_score();
            persist_high_score();
        }
    }

    void spawn_piece()
    {
        piece_ = next_piece_;
        next_piece_ = take_bag_piece();
        hold_used_ = false;
        reset_active_piece();
    }

    void hold_piece()
    {
        if (hold_used_) return;

        int outgoing = piece_;
        if (hold_piece_ < 0) {
            hold_piece_ = outgoing;
            spawn_piece();
        } else {
            piece_ = hold_piece_;
            hold_piece_ = outgoing;
            reset_active_piece();
        }
        hold_used_ = true;
    }

    bool fits(int nx, int ny, int nrot) const
    {
        const auto &cells = kPieces[piece_].cells[nrot];
        for (int i = 0; i < 4; i++) {
            int x = nx + cells[i][0];
            int y = ny + cells[i][1];
            if (x < 0 || x >= kCols || y >= kRows) return false;
            if (y >= 0 && board_[y][x]) return false;
        }
        return true;
    }

    bool try_move(int dx, int dy)
    {
        if (fits(px_ + dx, py_ + dy, rot_)) {
            px_ += dx; py_ += dy;
            return true;
        }
        return false;
    }

    void try_rotate()
    {
        int nr = (rot_ + 1) % 4;
        // A compact wall-kick table: try the rotation in place, against both
        // walls, two cells out for the I piece, and one/two cells upward when
        // rotating beside a stack. This keeps turns fluid without allowing a
        // piece to pass through occupied cells.
        static constexpr int8_t kKicks[][2] = {
            {0, 0}, {-1, 0}, {1, 0}, {-2, 0}, {2, 0},
            {0, -1}, {-1, -1}, {1, -1}, {0, -2},
        };
        for (const auto &kick : kKicks) {
            int nx = px_ + kick[0];
            int ny = py_ + kick[1];
            if (fits(nx, ny, nr)) {
                px_ = nx;
                py_ = ny;
                rot_ = nr;
                return;
            }
        }
    }

    void soft_drop()
    {
        if (try_move(0, 1)) {
            score_val_++;
            update_high_score();
        }
    }

    void hard_drop()
    {
        int distance = 0;
        while (try_move(0, 1)) distance++;
        settle_piece(distance * 2);
    }

    void settle_piece(int drop_score)
    {
        lock_piece();
        int cleared = clear_lines();
        if (cleared == 4) hal_lights_flash_success();
        lines_ += cleared;
        score_val_ += cleared * cleared * 100 + drop_score;
        update_high_score();
        spawn_piece();
    }

    void update_high_score()
    {
        if (score_val_ > hi_) hi_ = score_val_;
    }

    void persist_high_score()
    {
        if (hi_ <= saved_hi_) return;
        hal_config_set_int("tetris", "high_score", hi_);
        saved_hi_ = hi_;
    }

    void lock_piece()
    {
        const auto &cells = kPieces[piece_].cells[rot_];
        for (int i = 0; i < 4; i++) {
            int x = px_ + cells[i][0];
            int y = py_ + cells[i][1];
            if (y >= 0 && y < kRows && x >= 0 && x < kCols) board_[y][x] = (uint8_t)(piece_ + 1);
        }
    }

    int clear_lines()
    {
        int cleared = 0;
        for (int y = kRows - 1; y >= 0; y--) {
            bool full = true;
            for (int x = 0; x < kCols; x++) if (!board_[y][x]) { full = false; break; }
            if (full) {
                for (int yy = y; yy > 0; yy--)
                    for (int x = 0; x < kCols; x++) board_[yy][x] = board_[yy - 1][x];
                for (int x = 0; x < kCols; x++) board_[0][x] = 0;
                cleared++;
                y++; // recheck this row after shift
            }
        }
        return cleared;
    }

    static void board_draw_event(lv_event_t *event)
    {
        auto *app = static_cast<TetrisApp *>(lv_event_get_user_data(event));
        if (app) app->draw_board(lv_event_get_layer(event));
    }

    static void preview_draw_event(lv_event_t *event)
    {
        auto *app = static_cast<TetrisApp *>(lv_event_get_user_data(event));
        if (app) app->draw_previews(lv_event_get_layer(event));
    }

    void draw_block(lv_layer_t *layer, int x, int y, uint32_t color,
                    bool ghost = false)
    {
        if (x < 0 || x >= kCols || y < 0 || y >= kRows) return;
        lv_area_t area = {
            kBoardX + x * kCellPx,
            kBoardY + y * kCellPx,
            kBoardX + (x + 1) * kCellPx - 2,
            kBoardY + (y + 1) * kCellPx - 2,
        };
        lv_draw_rect_dsc_t block;
        lv_draw_rect_dsc_init(&block);
        block.radius = 1;
        if (ghost) {
            block.bg_opa = LV_OPA_TRANSP;
            block.border_opa = LV_OPA_60;
            block.border_width = 1;
            block.border_color = lv_color_hex(color);
        } else {
            block.bg_opa = LV_OPA_COVER;
            block.bg_color = lv_color_hex(color);
        }
        lv_draw_rect(layer, &block, &area);
    }

    void draw_piece(lv_layer_t *layer, int y, bool ghost)
    {
        const auto &cells = kPieces[piece_].cells[rot_];
        for (int i = 0; i < 4; ++i) {
            draw_block(layer, px_ + cells[i][0], y + cells[i][1],
                       kPieces[piece_].color, ghost);
        }
    }

    void draw_board(lv_layer_t *layer)
    {
        for (int r = 0; r < kRows; r++) {
            for (int c = 0; c < kCols; c++) {
                uint8_t v = board_[r][c];
                if (v) draw_block(layer, c, r, kPieces[v - 1].color);
            }
        }

        if (!dead_) {
            int ghost_y = py_;
            while (fits(px_, ghost_y + 1, rot_)) ++ghost_y;
            if (ghost_y != py_) draw_piece(layer, ghost_y, true);
            draw_piece(layer, py_, false);
        }
    }

    void draw_preview_panel(lv_layer_t *layer, int x, int piece)
    {
        constexpr int kPanelW = 58;
        constexpr int kPanelH = 38;
        constexpr int kPreviewCell = 7;
        constexpr int kPreviewY = kBoardY + 145;

        lv_area_t panel_area = {x, kPreviewY, x + kPanelW - 1,
                                kPreviewY + kPanelH - 1};
        lv_draw_rect_dsc_t panel;
        lv_draw_rect_dsc_init(&panel);
        panel.radius = 4;
        panel.bg_opa = LV_OPA_TRANSP;
        panel.border_opa = LV_OPA_COVER;
        panel.border_width = 1;
        panel.border_color = lv_color_hex(BadgeTheme::BORDER);
        lv_draw_rect(layer, &panel, &panel_area);

        if (piece < 0 || piece >= 7) return;
        const auto &cells = kPieces[piece].cells[0];
        int min_x = cells[0][0], max_x = cells[0][0];
        int min_y = cells[0][1], max_y = cells[0][1];
        for (int i = 1; i < 4; ++i) {
            if (cells[i][0] < min_x) min_x = cells[i][0];
            if (cells[i][0] > max_x) max_x = cells[i][0];
            if (cells[i][1] < min_y) min_y = cells[i][1];
            if (cells[i][1] > max_y) max_y = cells[i][1];
        }
        int piece_w = (max_x - min_x + 1) * kPreviewCell;
        int piece_h = (max_y - min_y + 1) * kPreviewCell;
        int origin_x = x + (kPanelW - piece_w) / 2 - min_x * kPreviewCell;
        int origin_y = kPreviewY + (kPanelH - piece_h) / 2 - min_y * kPreviewCell;

        lv_draw_rect_dsc_t block;
        lv_draw_rect_dsc_init(&block);
        block.radius = 1;
        block.bg_opa = LV_OPA_COVER;
        block.bg_color = lv_color_hex(kPieces[piece].color);
        for (int i = 0; i < 4; ++i) {
            int bx = origin_x + cells[i][0] * kPreviewCell;
            int by = origin_y + cells[i][1] * kPreviewCell;
            lv_area_t block_area = {bx, by, bx + kPreviewCell - 2,
                                    by + kPreviewCell - 2};
            lv_draw_rect(layer, &block, &block_area);
        }
    }

    void draw_previews(lv_layer_t *layer)
    {
        draw_preview_panel(layer, kSidebarX, hold_piece_);
        draw_preview_panel(layer, kSidebarX + 64, next_piece_);
    }

    void render()
    {
        if (board_canvas_) lv_obj_invalidate(board_canvas_);
        if (preview_canvas_) lv_obj_invalidate(preview_canvas_);
        if (score_label_) {
            lv_label_set_text_fmt(score_label_, "Your Current\nScore: %d", score_val_);
        }
        if (lines_label_) lv_label_set_text_fmt(lines_label_, "Lines: %d", lines_);
        if (hi_label_)    lv_label_set_text_fmt(hi_label_, "High Score:\n%d", hi_);
        if (game_over_card_ && game_over_text_) {
            if (dead_) {
                lv_label_set_text_fmt(game_over_text_,
                    "Game Over\nYour Current Score: %d\nA to Retry", score_val_);
                lv_obj_clear_flag(game_over_card_, LV_OBJ_FLAG_HIDDEN);
                lv_obj_move_foreground(game_over_card_);
            } else {
                lv_obj_add_flag(game_over_card_, LV_OBJ_FLAG_HIDDEN);
            }
        }
    }

    lv_obj_t *root_ = nullptr;
    lv_obj_t *board_canvas_ = nullptr;
    lv_obj_t *preview_canvas_ = nullptr;
    lv_obj_t *score_label_ = nullptr;
    lv_obj_t *lines_label_ = nullptr;
    lv_obj_t *hi_label_ = nullptr;
    lv_obj_t *game_over_card_ = nullptr;
    lv_obj_t *game_over_text_ = nullptr;
    uint8_t board_[kRows][kCols] = {};
    int piece_ = 0, rot_ = 0, px_ = 0, py_ = 0;
    int next_piece_ = 0;
    int hold_piece_ = -1;
    int bag_[7] = {};
    int bag_pos_ = 7;
    int score_val_ = 0;
    int lines_ = 0;
    int grav_counter_ = 0;
    bool dead_ = false;
    bool hold_used_ = false;
    int hi_ = 0;
    int saved_hi_ = 0;
    GamePauseController pause_;
};

static TetrisApp s_instance;
App *app_tetris_instance() { return &s_instance; }
