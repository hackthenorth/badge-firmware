#include "app.h"
#include "hal_lights.h"
#include "app_registry.h"
#include "hal_config.h"

#include <cstring>

extern "C" const lv_image_dsc_t icon_tictactoe;

class TicTacToeApp : public GameApp {
public:
    const char *name() const override { return "TicTacToe"; }
    const char *icon() const override { return "T3"; }
    const char *slug() const override { return "tictactoe"; }
    const lv_image_dsc_t *icon_image() const override { return &icon_tictactoe; }

    void on_enter(lv_obj_t *root) override
    {
        reset_game();
        x_wins_ = hal_config_get_int("tictactoe", "x_wins", 0);
        o_wins_ = hal_config_get_int("tictactoe", "o_wins", 0);
        draws_  = hal_config_get_int("tictactoe", "draws",  0);

        lv_obj_t *title = lv_label_create(root);
        lv_label_set_text(title, "Tic Tac Toe");
        lv_obj_set_style_text_color(title, lv_color_hex(BadgeTheme::TEXT), 0);
        lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
        lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 4);

        constexpr int kCell = 50, kPad = 3;
        int board_px = 3 * kCell + 2 * kPad;
        int bx = 160 - board_px / 2;
        int by = 34;

        for (int r = 0; r < 3; r++) {
            for (int c = 0; c < 3; c++) {
                lv_obj_t *cell = lv_obj_create(root);
                lv_obj_remove_style_all(cell);
                lv_obj_set_size(cell, kCell, kCell);
                lv_obj_set_pos(cell, bx + c * (kCell + kPad), by + r * (kCell + kPad));
                lv_obj_set_style_bg_color(cell, lv_color_hex(BadgeTheme::SURFACE), 0);
                lv_obj_set_style_bg_opa(cell, LV_OPA_COVER, 0);
                lv_obj_set_style_radius(cell, 0, 0);
                lv_obj_set_style_border_width(cell, 2, 0);
                lv_obj_set_style_border_color(cell, lv_color_hex(BadgeTheme::BORDER), 0);
                lv_obj_clear_flag(cell, LV_OBJ_FLAG_SCROLLABLE);

                lv_obj_t *lbl = lv_label_create(cell);
                lv_obj_set_style_text_font(lbl, &lv_font_montserrat_24, 0);
                lv_obj_center(lbl);
                cells_[r][c] = cell;
                marks_[r][c] = lbl;
            }
        }

        status_ = lv_label_create(root);
        lv_obj_set_style_text_font(status_, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(status_, lv_color_hex(BadgeTheme::TEXT), 0);
        lv_obj_align(status_, LV_ALIGN_TOP_MID, 0, 194);

        score_ = lv_label_create(root);
        lv_obj_set_style_text_font(score_, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(score_, lv_color_hex(BadgeTheme::TEXT_MUTED), 0);
        lv_obj_align(score_, LV_ALIGN_TOP_MID, 0, 217);

        render();
    }

    void on_exit() override
    {
        status_ = score_ = nullptr;
        for (int r = 0; r < 3; r++)
            for (int c = 0; c < 3; c++) cells_[r][c] = marks_[r][c] = nullptr;
    }

    void on_button(ButtonEvent ev) override
    {
        if (ev.kind != ButtonEventKind::Pressed) return;
        if (winner_ != 0) {
            if (ev.button == Button::A || ev.button == Button::Start) {
                reset_game();
                render();
            }
            return;
        }
        switch (ev.button) {
            case Button::Up:    if (row_ > 0) row_--; break;
            case Button::Down:  if (row_ < 2) row_++; break;
            case Button::Left:  if (col_ > 0) col_--; break;
            case Button::Right: if (col_ < 2) col_++; break;
            case Button::A: place_mark(); break;
            case Button::Start:
                reset_game();
                break;
            default: return;
        }
        render();
    }

private:
    void reset_game()
    {
        memset(board_, 0, sizeof(board_));
        turn_ = 1; // X
        winner_ = 0;
        row_ = col_ = 1;
    }

    void place_mark()
    {
        if (board_[row_][col_] != 0) return;
        board_[row_][col_] = turn_;
        int w = check_winner();
        if (w != 0) {
            winner_ = w;
            if (w == 1 || w == 2) hal_lights_flash_success();
            if (w == 1)  hal_config_set_int("tictactoe", "x_wins", ++x_wins_);
            if (w == 2)  hal_config_set_int("tictactoe", "o_wins", ++o_wins_);
            if (w == 3)  hal_config_set_int("tictactoe", "draws",  ++draws_);
        } else {
            turn_ = (turn_ == 1) ? 2 : 1;
        }
    }

    int check_winner()
    {
        int lines[8][3][2] = {
            { {0,0},{0,1},{0,2} }, { {1,0},{1,1},{1,2} }, { {2,0},{2,1},{2,2} },
            { {0,0},{1,0},{2,0} }, { {0,1},{1,1},{2,1} }, { {0,2},{1,2},{2,2} },
            { {0,0},{1,1},{2,2} }, { {0,2},{1,1},{2,0} },
        };
        for (auto &ln : lines) {
            int a = board_[ln[0][0]][ln[0][1]];
            int b = board_[ln[1][0]][ln[1][1]];
            int c = board_[ln[2][0]][ln[2][1]];
            if (a != 0 && a == b && b == c) return a;
        }
        for (int r = 0; r < 3; r++)
            for (int c = 0; c < 3; c++)
                if (board_[r][c] == 0) return 0;
        return 3; // draw
    }

    void render()
    {
        for (int r = 0; r < 3; r++) {
            for (int c = 0; c < 3; c++) {
                lv_obj_t *cell = cells_[r][c];
                lv_obj_t *lbl = marks_[r][c];
                if (!cell || !lbl) continue;

                uint8_t v = board_[r][c];
                lv_label_set_text(lbl, v == 1 ? "X" : v == 2 ? "O" : "");
                lv_obj_set_style_text_color(lbl,
                    lv_color_hex(v == 1 ? BadgeTheme::ACCENT_DETAIL : 0xff5c8a), 0);

                bool focus = (r == row_ && c == col_) && winner_ == 0;
                lv_obj_set_style_border_color(cell,
                    lv_color_hex(focus ? 0xffffff : BadgeTheme::BORDER), 0);
                lv_obj_set_style_border_width(cell, focus ? 3 : 2, 0);
            }
        }

        if (status_) {
            if (winner_ == 0) {
                lv_label_set_text_fmt(status_, "Turn: %c", turn_ == 1 ? 'X' : 'O');
            } else if (winner_ == 3) {
                lv_label_set_text(status_, "Draw!  (A to Restart)");
            } else {
                lv_label_set_text_fmt(status_, "%c Wins!  (A to Restart)",
                    winner_ == 1 ? 'X' : 'O');
            }
        }
        if (score_) lv_label_set_text_fmt(score_,
            "X %d  O %d  Draws %d", x_wins_, o_wins_, draws_);
    }

    lv_obj_t *cells_[3][3] = {};
    lv_obj_t *marks_[3][3] = {};
    lv_obj_t *status_ = nullptr;
    lv_obj_t *score_ = nullptr;
    uint8_t board_[3][3] = {};
    int turn_ = 1;
    int winner_ = 0;
    int row_ = 1;
    int col_ = 1;
    int x_wins_ = 0;
    int o_wins_ = 0;
    int draws_ = 0;
};

static TicTacToeApp s_instance;
App *app_tictactoe_instance() { return &s_instance; }
