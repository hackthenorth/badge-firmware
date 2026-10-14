#include "app.h"
#include "hal_config.h"
#include "hal_lights.h"

#include "esp_random.h"
#include "esp_timer.h"
#include <cstdio>
#include <cstring>

// Minesweeper on a 9x9 field with 10 mines. D-pad moves the cursor,
// A reveals, B flags. Mines are placed after the first reveal, keeping
// the 3x3 neighborhood around the opening tap mine-free so the first
// dig always opens a region.

extern "C" const lv_image_dsc_t icon_mines;

class MinesApp : public GameApp {
public:
    const char *name() const override { return "Mines9"; }
    const char *icon() const override { return "MN9"; }
    const lv_image_dsc_t *icon_image() const override { return &icon_mines; }
    const char *slug() const override { return "mines"; }

    void on_enter(lv_obj_t *root) override
    {
        best_ = hal_config_get_int("mines", "best_s", 0);

        title_ = lv_label_create(root);
        lv_label_set_text(title_, "Mines9");
        lv_obj_set_style_text_color(title_, lv_color_hex(BadgeTheme::TEXT), 0);
        lv_obj_set_style_text_font(title_, &lv_font_montserrat_18, 0);
        lv_obj_align(title_, LV_ALIGN_TOP_MID, 0, 4);

        status_ = lv_label_create(root);
        lv_obj_set_style_text_color(status_, lv_color_hex(BadgeTheme::ACCENT_DETAIL), 0);
        lv_obj_set_width(status_, 300);
        lv_obj_set_style_text_align(status_, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_long_mode(status_, LV_LABEL_LONG_WRAP);
        lv_obj_align(status_, LV_ALIGN_TOP_MID, 0, 28);

        constexpr int kCell = 17, kPad = 1;
        constexpr int kStep = kCell + kPad;
        int bx = 160 - (N * kStep - kPad) / 2;
        int by = 54;
        for (int y = 0; y < N; y++) {
            for (int x = 0; x < N; x++) {
                lv_obj_t *cell = lv_obj_create(root);
                lv_obj_remove_style_all(cell);
                lv_obj_set_size(cell, kCell, kCell);
                lv_obj_set_pos(cell, bx + x * kStep, by + y * kStep);
                lv_obj_set_style_bg_color(cell, lv_color_hex(BadgeTheme::SURFACE), 0);
                lv_obj_set_style_bg_opa(cell, LV_OPA_COVER, 0);
                lv_obj_set_style_radius(cell, 2, 0);
                lv_obj_set_style_border_width(cell, 1, 0);
                lv_obj_set_style_border_color(cell, lv_color_hex(BadgeTheme::BORDER), 0);
                lv_obj_clear_flag(cell, LV_OBJ_FLAG_SCROLLABLE);

                lv_obj_t *glyph = lv_label_create(cell);
                lv_obj_set_style_text_font(glyph, &lv_font_montserrat_14, 0);
                lv_obj_center(glyph);

                cells_[y][x] = cell;
                glyphs_[y][x] = glyph;
            }
        }

        hint_ = lv_label_create(root);
        lv_obj_set_style_text_color(hint_, lv_color_hex(BadgeTheme::TEXT_MUTED), 0);
        lv_obj_align(hint_, LV_ALIGN_BOTTOM_MID, 0, -8);

        reset();
    }

    void on_exit() override
    {
        hal_lights_clear();
        hal_lights_show();
        title_ = status_ = hint_ = nullptr;
        for (int y = 0; y < N; y++)
            for (int x = 0; x < N; x++) cells_[y][x] = glyphs_[y][x] = nullptr;
    }

    void on_home_confirmation_cancel(uint32_t paused_ms) override
    {
        start_us_ += static_cast<int64_t>(paused_ms) * 1000;
    }

    void on_button(ButtonEvent ev) override
    {
        if (ev.kind != ButtonEventKind::Pressed) return;
        if (over_) {
            if (ev.button == Button::A) reset();
            return;
        }
        const int old_x = cx_, old_y = cy_;
        switch (ev.button) {
            case Button::Up:    if (cy_ > 0) cy_--; break;
            case Button::Down:  if (cy_ < N - 1) cy_++; break;
            case Button::Left:  if (cx_ > 0) cx_--; break;
            case Button::Right: if (cx_ < N - 1) cx_++; break;
            case Button::A: reveal(cx_, cy_); break;
            case Button::B: toggle_flag(); break;
            default: return;
        }
        if (cx_ != old_x || cy_ != old_y) {
            render_cursor(old_x, old_y);
            render_cursor(cx_, cy_);
        }
        refresh_status();
    }

private:
    static constexpr int N = 9;
    static constexpr int kMines = 10;

    void reset()
    {
        for (int y = 0; y < N; y++) {
            for (int x = 0; x < N; x++) {
                mine_[y][x] = false;
                open_[y][x] = false;
                flag_[y][x] = false;
            }
        }
        placed_ = false;
        over_ = false;
        won_ = false;
        opened_ = 0;
        cx_ = N / 2;
        cy_ = N / 2;
        hal_lights_clear();
        hal_lights_show();
        for (int y = 0; y < N; y++)
            for (int x = 0; x < N; x++) render_cell(x, y);
        refresh_status();
        refresh_hint();
    }

    void place_mines(int safe_x, int safe_y)
    {
        // Keep the 3x3 neighborhood around the opening tap mine-free, so
        // the first reveal is always a zero that floods open a region.
        int left = kMines;
        while (left > 0) {
            int x = (int)(esp_random() % N);
            int y = (int)(esp_random() % N);
            if (mine_[y][x]) continue;
            if (x >= safe_x - 1 && x <= safe_x + 1 && y >= safe_y - 1 && y <= safe_y + 1) {
                continue;
            }
            mine_[y][x] = true;
            left--;
        }
        start_us_ = esp_timer_get_time();
    }

    int adjacent(int x, int y) const
    {
        int n = 0;
        for (int dy = -1; dy <= 1; dy++) {
            for (int dx = -1; dx <= 1; dx++) {
                if (!dx && !dy) continue;
                int nx = x + dx, ny = y + dy;
                if (nx >= 0 && nx < N && ny >= 0 && ny < N && mine_[ny][nx]) n++;
            }
        }
        return n;
    }

    void reveal(int x, int y)
    {
        if (open_[y][x] || flag_[y][x]) return;
        if (!placed_) {
            place_mines(x, y);
            placed_ = true;
        }
        if (mine_[y][x]) {
            over_ = true;
            won_ = false;
            // Reveal the mines; untouched safe cells keep their pixels.
            for (int my = 0; my < N; my++)
                for (int mx = 0; mx < N; mx++)
                    if (mine_[my][mx]) {
                        open_[my][mx] = true;
                        render_cell(mx, my);
                    }
            for (size_t i = 0; i < LED_COUNT; i++) hal_lights_set(i, 255, 0, 0);
            hal_lights_show();
            return;
        }
        flood(x, y);
        if (opened_ >= N * N - kMines) {
            over_ = true;
            won_ = true;
            // A zero-second clear is still a win; best_s alone is ambiguous.
            hal_config_set_int("mines", "cleared", 1);
            int s = elapsed_s();
            if (best_ == 0 || s < best_) {
                best_ = s;
                hal_config_set_int("mines", "best_s", best_);
            }
            hal_lights_flash_success();
            render_cursor(cx_, cy_);
            refresh_hint();
        }
    }

    void flood(int x, int y)
    {
        if (x < 0 || x >= N || y < 0 || y >= N) return;
        if (open_[y][x] || flag_[y][x] || mine_[y][x]) return;
        open_[y][x] = true;
        opened_++;
        render_cell(x, y);
        if (adjacent(x, y) > 0) return;
        for (int dy = -1; dy <= 1; dy++) {
            for (int dx = -1; dx <= 1; dx++) {
                if (dx || dy) flood(x + dx, y + dy);
            }
        }
    }

    void toggle_flag()
    {
        if (open_[cy_][cx_]) return;
        flag_[cy_][cx_] = !flag_[cy_][cx_];
        render_cell(cx_, cy_);
    }

    int elapsed_s() const
    {
        return (int)((esp_timer_get_time() - start_us_) / 1000000);
    }

    static uint32_t number_color(int a)
    {
        switch (a) {
            case 1: return 0x53d769; // green
            case 2: return 0xffc63a; // gold
            case 3: return 0xff9e2c; // orange
            case 4: return 0xff5c8a; // pink
            case 5: return 0xb087b1; // violet
            case 6: return 0xf4f4ef; // white
            case 7: return 0xbcbcb5; // soft
            default: return 0xa0a09a; // muted
        }
    }

    void refresh_status()
    {
        if (!status_) return;
        char text[64];
        if (over_) {
            if (won_) snprintf(text, sizeof(text), "clear in %ds  best %ds", elapsed_s(), best_);
            else snprintf(text, sizeof(text), "BOOM  A: retry");
        } else if (!placed_) {
            snprintf(text, sizeof(text), "10 mines  A: dig  B: flag");
        } else {
            snprintf(text, sizeof(text), "%ds  A: dig  B: flag", elapsed_s());
        }
        if (strcmp(text, lv_label_get_text(status_)) != 0)
            lv_label_set_text(status_, text);
    }

    void render_cursor(int x, int y)
    {
        lv_obj_t *cell = cells_[y][x];
        if (!cell) return;
        const bool cur = x == cx_ && y == cy_ && !over_;
        lv_obj_set_style_border_color(cell,
            lv_color_hex(cur ? 0xffffff : BadgeTheme::BORDER), 0);
        lv_obj_set_style_border_width(cell, cur ? 2 : 1, 0);
    }

    void render_cell(int x, int y)
    {
        lv_obj_t *cell = cells_[y][x];
        lv_obj_t *glyph = glyphs_[y][x];
        if (!cell || !glyph) return;
        render_cursor(x, y);
        if (!open_[y][x]) {
            lv_obj_set_style_bg_color(cell, lv_color_hex(BadgeTheme::SURFACE), 0);
            if (flag_[y][x]) {
                lv_label_set_text(glyph, LV_SYMBOL_WARNING);
                lv_obj_set_style_text_color(glyph, lv_color_hex(0xffc63a), 0);
            } else {
                lv_label_set_text(glyph, "");
            }
        } else if (mine_[y][x]) {
            const bool hit = x == cx_ && y == cy_;
            lv_obj_set_style_bg_color(cell,
                lv_color_hex(hit ? 0x7a1a1a : 0x3a0f0f), 0);
            lv_label_set_text(glyph, LV_SYMBOL_CLOSE);
            lv_obj_set_style_text_color(glyph, lv_color_hex(0xff5252), 0);
        } else {
            lv_obj_set_style_bg_color(cell, lv_color_hex(BadgeTheme::PANEL), 0);
            const int a = adjacent(x, y);
            if (a == 0) {
                lv_label_set_text(glyph, "");
            } else {
                char num[2] = { (char)('0' + a), '\0' };
                lv_label_set_text(glyph, num);
                lv_obj_set_style_text_color(glyph, lv_color_hex(number_color(a)), 0);
            }
        }
    }

    void refresh_hint()
    {
        if (hint_) {
            if (best_ > 0) lv_label_set_text_fmt(hint_, "best %ds", best_);
            else lv_label_set_text(hint_, "clear the field");
        }
    }

    lv_obj_t *title_ = nullptr;
    lv_obj_t *status_ = nullptr;
    lv_obj_t *hint_ = nullptr;
    lv_obj_t *cells_[N][N] = {};
    lv_obj_t *glyphs_[N][N] = {};
    bool mine_[N][N] = {};
    bool open_[N][N] = {};
    bool flag_[N][N] = {};
    int cx_ = 0;
    int cy_ = 0;
    int opened_ = 0;
    int best_ = 0;
    int64_t start_us_ = 0;
    bool placed_ = false;
    bool over_ = false;
    bool won_ = false;
};

static MinesApp s_instance;
App *app_mines_instance() { return &s_instance; }
