#include "app.h"
#include "hal_config.h"
#include "hal_lights.h"

#include <cstdio>
#include <string>
#include <array>
#include <cstdint>

// Sokoban: push every box onto a dock. D-pad moves, A resets the level,
// B undoes, Start selects a level. Fewest moves per level are remembered.

extern "C" const lv_image_dsc_t icon_sokoban;
extern "C" const lv_image_dsc_t spr_soko_player;
extern "C" const lv_image_dsc_t spr_soko_box;
extern "C" const lv_image_dsc_t spr_soko_wall;
extern "C" const lv_image_dsc_t spr_soko_dock;

class SokobanApp : public GameApp {
public:
    const char *name() const override { return "Sokoban"; }
    const char *icon() const override { return "SB"; }
    const lv_image_dsc_t *icon_image() const override { return &icon_sokoban; }
    const char *slug() const override { return "sokoban"; }

    bool wants_wake_lock() const override { return true; }

    void on_enter(lv_obj_t *root) override
    {
        root_ = root;
        level_ = hal_config_get_int("sokoban", "level", 0);
        if (level_ < 0 || level_ >= kLevels) level_ = 0;

        title_ = lv_label_create(root);
        lv_obj_set_style_text_color(title_, lv_color_hex(BadgeTheme::TEXT), 0);
        lv_obj_set_style_text_font(title_, &lv_font_montserrat_16, 0);
        lv_obj_align(title_, LV_ALIGN_TOP_MID, 0, 4);

        board_ = lv_obj_create(root);
        lv_obj_remove_style_all(board_);
        lv_obj_set_style_bg_color(board_, lv_color_hex(BadgeTheme::PANEL), 0);
        lv_obj_set_style_bg_opa(board_, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(board_, 6, 0);
        lv_obj_clear_flag(board_, LV_OBJ_FLAG_SCROLLABLE);

        for (int r = 0; r < kMax; r++) {
            for (int c = 0; c < kMax; c++) {
                base_[r][c] = lv_image_create(board_);
                lv_obj_clear_flag(base_[r][c], LV_OBJ_FLAG_SCROLLABLE);
                lv_obj_add_flag(base_[r][c], LV_OBJ_FLAG_HIDDEN);
                top_[r][c] = lv_image_create(board_);
                lv_obj_clear_flag(top_[r][c], LV_OBJ_FLAG_SCROLLABLE);
                lv_obj_add_flag(top_[r][c], LV_OBJ_FLAG_HIDDEN);
            }
        }

        status_ = lv_label_create(root);
        lv_obj_set_style_text_color(status_, lv_color_hex(BadgeTheme::ACCENT_DETAIL), 0);
        lv_obj_set_width(status_, 300);
        lv_obj_set_style_text_align(status_, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_long_mode(status_, LV_LABEL_LONG_WRAP);
        lv_obj_align(status_, LV_ALIGN_BOTTOM_MID, 0, -26);

        hint_ = lv_label_create(root);
        lv_label_set_text(hint_, "D-pad: push  A: reset  B: undo");
        lv_obj_set_style_text_color(hint_, lv_color_hex(BadgeTheme::TEXT_MUTED), 0);
        lv_obj_align(hint_, LV_ALIGN_BOTTOM_MID, 0, -8);

        load_level(level_);
    }

    void on_exit() override
    {
        hal_lights_clear();
        hal_lights_show();
        root_ = title_ = board_ = status_ = hint_ = nullptr;
        level_menu_ = level_detail_ = nullptr;
        for (auto &choice : level_choices_) choice = nullptr;
        for (int r = 0; r < kMax; r++)
            for (int c = 0; c < kMax; c++) base_[r][c] = top_[r][c] = nullptr;
    }

    void on_button(ButtonEvent ev) override
    {
        if (ev.kind != ButtonEventKind::Pressed) return;
        if (level_menu_) {
            handle_level_selection(ev.button);
            return;
        }
        if (ev.button == Button::Start) {
            open_level_selection();
            return;
        }
        if (ev.button == Button::A) {
            if (solved_ && level_ + 1 < kLevels) {
                level_++;
                hal_config_set_int("sokoban", "level", level_);
                load_level(level_);
            } else {
                load_level(level_);
            }
            return;
        }
        if (ev.button == Button::B) {
            undo();
            return;
        }
        int dx = 0, dy = 0;
        switch (ev.button) {
            case Button::Up: dy = -1; break;
            case Button::Down: dy = 1; break;
            case Button::Left: dx = -1; break;
            case Button::Right: dx = 1; break;
            default: return;
        }
        move(dx, dy);
    }

private:
    static constexpr int kTile = 20;
    static constexpr int kMax = 8;
    static constexpr int kLevels = 12;
    static constexpr int kLevelCols = 4;
    // Optimal pars, machine-verified with a BFS solver.
    static constexpr int kPar[kLevels] = { 1, 6, 16, 16, 27, 25, 27, 27, 28, 29, 35, 36 };

    static constexpr const char *kMaps[kLevels][8] = {
        {
            "#####",
            "#@$.#",
            "#   #",
            "#   #",
            "#####",
        },
        {
            "######",
            "#.   #",
            "#  @ #",
            "# $  #",
            "#    #",
            "######",
        },
        {
            "#######",
            "#.# #.#",
            "#  @  #",
            "# $$  #",
            "#     #",
            "#     #",
            "#######",
        },
        {
            "#######",
            "#.#.#.#",
            "#     #",
            "# $$$ #",
            "#  @  #",
            "#######",
        },
        {
            "########",
            "#.#.#.##",
            "#      #",
            "#  @   #",
            "#  $$  #",
            "#  $   #",
            "#      #",
            "########",
        },
        {
            "#######",
            "##.#..#",
            "#     #",
            "#  @  #",
            "# $$$ #",
            "#     #",
            "#######",
        },
        {
            "#######",
            "#.#.#.#",
            "# # # #",
            "#  @  #",
            "# $$$ #",
            "#     #",
            "#######",
        },
        {
            "#######",
            "#.#.#.#",
            "#     #",
            "#  @  #",
            "# $$$ #",
            "#     #",
            "#######",
        },
        {
            "########",
            "#.#..###",
            "#      #",
            "#   @  #",
            "#  $$$ #",
            "#      #",
            "########",
        },
        {
            "#######",
            "#.#.#.#",
            "#     #",
            "#  @  #",
            "#  $  #",
            "# $ $ #",
            "#     #",
            "#######",
        },
        {
            "########",
            "#..##..#",
            "#      #",
            "#@     #",
            "#  $$$$#",
            "#      #",
            "########",
        },
        {
            "########",
            "#..##..#",
            "#      #",
            "#   @  #",
            "# $$$$ #",
            "#      #",
            "########",
        },
    };

    struct UndoMove { uint8_t x, y; int8_t dx, dy; bool pushed; };
    static constexpr size_t kUndoLimit = 128;

    void open_level_selection()
    {
        selected_level_ = level_;
        level_menu_ = lv_obj_create(root_);
        lv_obj_remove_style_all(level_menu_);
        lv_obj_set_size(level_menu_, 320, 240);
        lv_obj_set_style_bg_color(level_menu_, lv_color_hex(BadgeTheme::BACKGROUND), 0);
        lv_obj_set_style_bg_opa(level_menu_, LV_OPA_COVER, 0);
        lv_obj_clear_flag(level_menu_, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *heading = lv_label_create(level_menu_);
        lv_label_set_text(heading, "Sokoban levels");
        lv_obj_set_style_text_font(heading, &lv_font_montserrat_18, 0);
        lv_obj_set_style_text_color(heading, lv_color_hex(BadgeTheme::TEXT), 0);
        lv_obj_align(heading, LV_ALIGN_TOP_MID, 0, 8);

        for (int i = 0; i < kLevels; ++i) {
            char key[16];
            snprintf(key, sizeof(key), "best%d", i);
            level_bests_[i] = hal_config_get_int("sokoban", key, 0);
            lv_obj_t *choice = lv_obj_create(level_menu_);
            lv_obj_remove_style_all(choice);
            lv_obj_set_size(choice, 64, 40);
            lv_obj_set_pos(choice, 20 + (i % kLevelCols) * 72,
                          44 + (i / kLevelCols) * 48);
            lv_obj_set_style_radius(choice, 4, 0);
            lv_obj_set_style_bg_opa(choice, LV_OPA_COVER, 0);
            lv_obj_clear_flag(choice, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_t *number = lv_label_create(choice);
            lv_label_set_text_fmt(number, "%d", i + 1);
            lv_obj_set_style_text_font(number, &lv_font_montserrat_18, 0);
            lv_obj_set_style_text_color(number,
                lv_color_hex(level_bests_[i] > 0 ? 0x53d769 : BadgeTheme::TEXT), 0);
            lv_obj_center(number);
            level_choices_[i] = choice;
            render_level_choice(i);
        }

        level_detail_ = lv_label_create(level_menu_);
        lv_obj_set_width(level_detail_, 296);
        lv_obj_set_style_text_font(level_detail_, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(level_detail_, lv_color_hex(BadgeTheme::TEXT_SOFT), 0);
        lv_obj_set_style_text_align(level_detail_, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(level_detail_, LV_ALIGN_TOP_MID, 0, 196);
        refresh_level_detail();
    }

    void close_level_selection()
    {
        lv_obj_delete(level_menu_);
        level_menu_ = level_detail_ = nullptr;
        for (auto &choice : level_choices_) choice = nullptr;
    }

    void render_level_choice(int idx)
    {
        const bool selected = idx == selected_level_;
        lv_obj_t *choice = level_choices_[idx];
        lv_obj_set_style_bg_color(choice,
            lv_color_hex(selected ? BadgeTheme::TRACK : BadgeTheme::SURFACE), 0);
        lv_obj_set_style_border_color(choice,
            lv_color_hex(selected ? BadgeTheme::ACCENT_DETAIL : BadgeTheme::BORDER), 0);
        lv_obj_set_style_border_width(choice, selected ? 2 : 1, 0);
    }

    void refresh_level_detail()
    {
        const int best = level_bests_[selected_level_];
        if (best > 0)
            lv_label_set_text_fmt(level_detail_, "Par: %d  Best: %d", kPar[selected_level_], best);
        else
            lv_label_set_text_fmt(level_detail_, "Par: %d  Best: --", kPar[selected_level_]);
    }

    void handle_level_selection(Button button)
    {
        if (button == Button::B || button == Button::Start) {
            close_level_selection();
            return;
        }
        if (button == Button::A) {
            level_ = selected_level_;
            hal_config_set_int("sokoban", "level", level_);
            close_level_selection();
            load_level(level_);
            return;
        }
        const int old = selected_level_;
        switch (button) {
            case Button::Left:  if (selected_level_ % kLevelCols > 0) --selected_level_; break;
            case Button::Right: if (selected_level_ % kLevelCols < kLevelCols - 1) ++selected_level_; break;
            case Button::Up:    if (selected_level_ >= kLevelCols) selected_level_ -= kLevelCols; break;
            case Button::Down:  if (selected_level_ + kLevelCols < kLevels) selected_level_ += kLevelCols; break;
            default: return;
        }
        if (old != selected_level_) {
            render_level_choice(old);
            render_level_choice(selected_level_);
            refresh_level_detail();
        }
    }

    void load_level(int idx)
    {
        w_ = 0;
        h_ = 0;
        for (int r = 0; r < 8; r++) {
            const char *row = kMaps[idx][r];
            if (!row || !row[0]) break;
            int len = 0;
            while (row[len]) len++;
            if (len > w_) w_ = len;
            h_ = r + 1;
        }
        cells_.assign(w_ * h_, '#');
        for (int r = 0; r < h_; r++) {
            const char *row = kMaps[idx][r];
            for (int c = 0; c < w_; c++) {
                char ch = row[c] ? row[c] : '#';
                if (ch == '@') {
                    px_ = c;
                    py_ = r;
                    ch = ' ';
                }
                cells_[r * w_ + c] = (ch == '$' || ch == '.' || ch == '#') ? ch : ' ';
            }
        }
        moves_ = 0;
        solved_ = false;
        history_head_ = history_count_ = 0;
        best_ = hal_config_get_int("sokoban", ("best" + std::to_string(idx)).c_str(), 0);

        int bw = w_ * kTile, bh = h_ * kTile;
        lv_obj_set_size(board_, bw + 8, bh + 8);
        lv_obj_align(board_, LV_ALIGN_CENTER, 0, 6);
        int ox = 4, oy = 4;
        for (int r = 0; r < kMax; r++) {
            for (int c = 0; c < kMax; c++) {
                bool live = (r < h_ && c < w_);
                if (live) {
                    lv_obj_set_pos(base_[r][c], ox + c * kTile, oy + r * kTile);
                    lv_obj_set_pos(top_[r][c], ox + c * kTile, oy + r * kTile);
                } else {
                    lv_obj_add_flag(base_[r][c], LV_OBJ_FLAG_HIDDEN);
                    lv_obj_add_flag(top_[r][c], LV_OBJ_FLAG_HIDDEN);
                }
            }
        }
        hal_lights_clear();
        hal_lights_show();
        render();
    }

    char at(int x, int y) const
    {
        if (x < 0 || y < 0 || x >= w_ || y >= h_) return '#';
        return cells_[y * w_ + x];
    }

    bool is_dock_raw(int x, int y) const
    {
        const char *row = kMaps[level_][y];
        char ch = (row && row[x]) ? row[x] : '#';
        return ch == '.' || ch == '*' || ch == '+';
    }

    void undo()
    {
        if (!history_count_) return;
        history_head_ = (history_head_ + kUndoLimit - 1) % kUndoLimit;
        const auto step = history_[history_head_];
        --history_count_;
        if (step.pushed) {
            cells_[(py_ + step.dy) * w_ + px_ + step.dx] = ' ';
            cells_[py_ * w_ + px_] = '$';
        }
        px_ = step.x;
        py_ = step.y;
        --moves_;
        solved_ = false;
        render();
    }

    void move(int dx, int dy)
    {
        if (solved_) return;
        int nx = px_ + dx, ny = py_ + dy;
        char dest = at(nx, ny);
        if (dest == '#') return;
        if (dest == '$') {
            int bx = nx + dx, by = ny + dy;
            char beyond = at(bx, by);
            if (beyond == '#' || beyond == '$') {
                return;
            }
            cells_[ny * w_ + nx] = ' ';
            cells_[by * w_ + bx] = '$';
        }
        history_[history_head_] = {static_cast<uint8_t>(px_), static_cast<uint8_t>(py_),
            static_cast<int8_t>(dx), static_cast<int8_t>(dy), dest == '$'};
        history_head_ = (history_head_ + 1) % kUndoLimit;
        if (history_count_ < kUndoLimit) ++history_count_;
        px_ = nx;
        py_ = ny;
        moves_++;
        check_solved();
        render();
    }

    void check_solved()
    {
        for (int r = 0; r < h_; r++) {
            for (int c = 0; c < w_; c++) {
                if (is_dock_raw(c, r) && cells_[r * w_ + c] != '$') return;
            }
        }
        solved_ = true;
        char key[16];
        snprintf(key, sizeof(key), "best%d", level_);
        if (best_ == 0 || moves_ < best_) {
            best_ = moves_;
            hal_config_set_int("sokoban", key, best_);
        }
        hal_lights_flash_success();
    }

    void render()
    {
        if (title_) {
            lv_label_set_text_fmt(title_, "Sokoban %d/%d", level_ + 1, kLevels);
        }
        for (int r = 0; r < h_; r++) {
            for (int c = 0; c < w_; c++) {
                lv_obj_t *base = base_[r][c];
                lv_obj_t *top = top_[r][c];
                if (!base || !top) continue;
                char cell = cells_[r * w_ + c];
                bool dock = is_dock_raw(c, r);
                if (cell == '#') {
                    lv_image_set_src(base, &spr_soko_wall);
                    lv_obj_clear_flag(base, LV_OBJ_FLAG_HIDDEN);
                    lv_obj_add_flag(top, LV_OBJ_FLAG_HIDDEN);
                } else {
                    if (dock) {
                        lv_image_set_src(base, &spr_soko_dock);
                        lv_obj_clear_flag(base, LV_OBJ_FLAG_HIDDEN);
                    } else {
                        lv_obj_add_flag(base, LV_OBJ_FLAG_HIDDEN);
                    }
                    if (cell == '$') {
                        lv_image_set_src(top, &spr_soko_box);
                        if (dock) {
                            lv_obj_set_style_image_recolor(top, lv_color_hex(0x53d769), 0);
                            lv_obj_set_style_image_recolor_opa(top, LV_OPA_60, 0);
                        } else {
                            lv_obj_set_style_image_recolor_opa(top, LV_OPA_TRANSP, 0);
                        }
                        lv_obj_clear_flag(top, LV_OBJ_FLAG_HIDDEN);
                    } else if (c == px_ && r == py_) {
                        lv_image_set_src(top, &spr_soko_player);
                        lv_obj_set_style_image_recolor_opa(top, LV_OPA_TRANSP, 0);
                        lv_obj_clear_flag(top, LV_OBJ_FLAG_HIDDEN);
                    } else {
                        lv_obj_add_flag(top, LV_OBJ_FLAG_HIDDEN);
                    }
                }
            }
        }
        if (status_) {
            if (solved_) {
                if (level_ + 1 < kLevels) {
                    lv_label_set_text_fmt(status_, "Solved in %d!  A: level %d",
                                          moves_, level_ + 2);
                } else {
                    lv_label_set_text_fmt(status_, "All solved in %d!  A: replay", moves_);
                }
            } else if (best_ > 0) {
                lv_label_set_text_fmt(status_, "moves %d  par %d  best %d", moves_,
                                      kPar[level_], best_);
            } else {
                lv_label_set_text_fmt(status_, "moves %d  par %d", moves_, kPar[level_]);
            }
        }
    }

    lv_obj_t *root_ = nullptr;
    lv_obj_t *level_menu_ = nullptr;
    lv_obj_t *level_detail_ = nullptr;
    lv_obj_t *level_choices_[kLevels] = {};
    int level_bests_[kLevels] = {};
    int selected_level_ = 0;
    lv_obj_t *title_ = nullptr;
    lv_obj_t *board_ = nullptr;
    lv_obj_t *status_ = nullptr;
    lv_obj_t *hint_ = nullptr;
    lv_obj_t *base_[kMax][kMax] = {};
    lv_obj_t *top_[kMax][kMax] = {};
    std::string cells_;
    std::array<UndoMove, kUndoLimit> history_{};
    size_t history_head_ = 0, history_count_ = 0;
    int w_ = 0, h_ = 0, px_ = 0, py_ = 0;
    int moves_ = 0, best_ = 0, level_ = 0;
    bool solved_ = false;
};

static SokobanApp s_instance;
App *app_sokoban_instance() { return &s_instance; }
