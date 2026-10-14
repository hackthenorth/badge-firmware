#include "app_launcher.h"
#include "app.h"
#include "app_registry.h"
#include "app_reboot.h"
#include "launcher_slugs.h"

#include <cstdint>
#include <cstring>

static constexpr int GRID_ROWS = 4;
static constexpr int GRID_COLS = 4;
static constexpr int PAGE_SIZE = GRID_ROWS * GRID_COLS;
static constexpr int EDGE_PAD = 2;   // padding from each screen edge
static constexpr int CELL_GAP = 2;   // black gutters keep the grid technical
// Reserve 12px at the bottom for the page indicator.
static constexpr int BOTTOM_RESERVE = 12;
static constexpr int TOP_RESERVE = 22;
static constexpr int CELL_W = (320 - 2 * EDGE_PAD - (GRID_COLS - 1) * CELL_GAP) / GRID_COLS;
static constexpr int CELL_H = (240 - 2 * EDGE_PAD - BOTTOM_RESERVE - TOP_RESERVE - (GRID_ROWS - 1) * CELL_GAP) / GRID_ROWS;

// Sparse black/plum palette, matching the badge website rather than a phone UI.
static constexpr uint32_t COLOR_BG          = BadgeTheme::BACKGROUND;
static constexpr uint32_t COLOR_CELL        = BadgeTheme::PANEL;
static constexpr uint32_t COLOR_CELL_EMPTY  = BadgeTheme::BACKGROUND;
static constexpr uint32_t COLOR_CELL_FOCUS  = BadgeTheme::ACCENT;
static constexpr uint32_t COLOR_BORDER      = BadgeTheme::BORDER;
static constexpr uint32_t COLOR_BORDER_FOC  = BadgeTheme::ACCENT_DETAIL;
static constexpr uint32_t COLOR_TEXT        = BadgeTheme::TEXT;
static constexpr uint32_t COLOR_TEXT_DIM    = BadgeTheme::TEXT_DIM;
static constexpr uint32_t COLOR_DOT_ACTIVE  = BadgeTheme::ACCENT_DETAIL;
static constexpr uint32_t COLOR_DOT_IDLE    = BadgeTheme::BORDER;
static constexpr uint32_t COLOR_INTERNAL    = 0xffd84d;

// Pagination strip: filled circle for the current page, hollow rings for the
// rest, plus prominent arrows at the screen edges when more pages are available.
static constexpr int PAGER_H    = BOTTOM_RESERVE;
static constexpr int DOT_SIZE   = 6;   // outer diameter of a page circle
static constexpr int DOT_GAP    = 6;   // space between circles
static constexpr int ARROW_W    = 20;
static constexpr int ARROW_H    = 8;
static constexpr int ARROW_HEAD = 6;
static constexpr int MAX_DOTS   = 16;  // 16 pages * 16 slots = 256 apps

// lv_line does not copy its points, so these must outlive the objects.
static constexpr lv_point_precise_t ARROW_LEFT_PTS[] = {
    { ARROW_HEAD, 0 }, { 0, ARROW_H / 2 }, { ARROW_W, ARROW_H / 2 },
    { 0, ARROW_H / 2 }, { ARROW_HEAD, ARROW_H },
};
static constexpr lv_point_precise_t ARROW_RIGHT_PTS[] = {
    { ARROW_W - ARROW_HEAD, 0 }, { ARROW_W, ARROW_H / 2 }, { 0, ARROW_H / 2 },
    { ARROW_W, ARROW_H / 2 }, { ARROW_W - ARROW_HEAD, ARROW_H },
};

class LauncherApp : public App {
public:
    const char *name() const override { return "Launcher"; }
    const char *icon() const override { return "APP"; }
    const char *slug() const override { return "launcher"; }
    AppAccess access() const override { return AppAccess::Default; }

    void on_enter(lv_obj_t *root) override
    {
        a_held_ = false;
        grid_ = lv_obj_create(root);
        lv_obj_remove_style_all(grid_);
        lv_obj_set_size(grid_, LV_PCT(100), LV_PCT(100));
        lv_obj_set_style_bg_color(grid_, lv_color_hex(COLOR_BG), 0);
        lv_obj_set_style_bg_opa(grid_, LV_OPA_COVER, 0);
        lv_obj_clear_flag(grid_, LV_OBJ_FLAG_SCROLLABLE);

        name_ = lv_label_create(grid_);
        lv_obj_set_width(name_, 308);
        lv_label_set_long_mode(name_, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_font(name_, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(name_, lv_color_hex(COLOR_TEXT), 0);
        lv_obj_set_style_text_align(name_, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(name_, LV_ALIGN_TOP_MID, 0, 1);

        for (int r = 0; r < GRID_ROWS; r++) {
            for (int c = 0; c < GRID_COLS; c++) {
                lv_obj_t *cell = lv_obj_create(grid_);
                lv_obj_remove_style_all(cell);
                lv_obj_set_size(cell, CELL_W, CELL_H);
                lv_obj_set_pos(cell,
                    EDGE_PAD + c * (CELL_W + CELL_GAP),
                    TOP_RESERVE + EDGE_PAD + r * (CELL_H + CELL_GAP));
                lv_obj_set_style_bg_opa(cell, LV_OPA_COVER, 0);
                lv_obj_set_style_radius(cell, 0, 0);
                lv_obj_set_style_border_width(cell, 1, 0);
                lv_obj_clear_flag(cell, LV_OBJ_FLAG_SCROLLABLE);
                lv_obj_add_event_cb(cell, draw_icon_outline, LV_EVENT_DRAW_MAIN, nullptr);
                lv_obj_add_event_cb(cell, draw_internal_outline, LV_EVENT_DRAW_POST, nullptr);

                lv_obj_t *lbl = lv_label_create(cell);
                lv_obj_set_style_text_font(lbl, &lv_font_montserrat_24, 0);
                lv_obj_center(lbl);

                lv_obj_t *img = lv_image_create(cell);
                lv_obj_center(img);
                lv_image_set_scale(img, 224);
                lv_obj_add_flag(img, LV_OBJ_FLAG_HIDDEN);

                cells_[r][c] = cell;
                labels_[r][c] = lbl;
                images_[r][c] = img;
            }
        }

        pager_ = lv_obj_create(grid_);
        lv_obj_remove_style_all(pager_);
        lv_obj_set_size(pager_, LV_PCT(100), PAGER_H);
        lv_obj_align(pager_, LV_ALIGN_BOTTOM_MID, 0, 0);
        lv_obj_clear_flag(pager_, LV_OBJ_FLAG_SCROLLABLE);

        arrow_left_ = make_arrow(ARROW_LEFT_PTS);
        arrow_right_ = make_arrow(ARROW_RIGHT_PTS);

        for (int i = 0; i < MAX_DOTS; i++) {
            lv_obj_t *dot = lv_obj_create(pager_);
            lv_obj_remove_style_all(dot);
            lv_obj_set_size(dot, DOT_SIZE, DOT_SIZE);
            lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
            lv_obj_clear_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_add_flag(dot, LV_OBJ_FLAG_HIDDEN);
            dots_[i] = dot;
        }

        size_t n = visible_app_count();
        if (last_launched_idx_ < n) {
            page_ = (int)(last_launched_idx_ / PAGE_SIZE);
            int within = (int)(last_launched_idx_ % PAGE_SIZE);
            row_ = within / GRID_COLS;
            col_ = within % GRID_COLS;
        } else {
            page_ = 0;
            row_ = 0;
            col_ = 0;
            // Seamless reboot-exit: the radio/NFC session we just left stashed
            // its slug, so the cursor lands back on it. One-shot — a slug
            // that is no longer visible (or none) keeps the default cursor.
            char focus[64];
            if (app_reboot_take_launcher_focus(focus, sizeof(focus))) {
                for (size_t i = 0; i < n; i++) {
                    const char *slug = launcher_slugs_at(i);
                    if (slug && strcmp(slug, focus) == 0) {
                        last_launched_idx_ = i;
                        page_ = (int)(i / PAGE_SIZE);
                        const int within = (int)(i % PAGE_SIZE);
                        row_ = within / GRID_COLS;
                        col_ = within % GRID_COLS;
                        break;
                    }
                }
            }
        }
        refresh_cells();
        update_focus();
    }

    void on_exit() override
    {
        mode_ = Mode::Grid;
        target_app_ = nullptr;
        a_held_ = false;
        grid_ = name_ = nullptr;
        pager_ = nullptr;
        arrow_left_ = nullptr;
        arrow_right_ = nullptr;
        for (int i = 0; i < MAX_DOTS; i++) dots_[i] = nullptr;
        for (int r = 0; r < GRID_ROWS; r++)
            for (int c = 0; c < GRID_COLS; c++) {
                cells_[r][c] = nullptr;
                labels_[r][c] = nullptr;
                images_[r][c] = nullptr;
            }
    }

    void on_button(ButtonEvent ev) override
    {
        switch (mode_) {
            case Mode::Grid: on_button_grid(ev); break;
            case Mode::Move: on_button_move(ev); break;
        }
    }

    void on_button_grid(ButtonEvent ev)
    {
        if (ev.button == Button::A) {
            if (ev.kind == ButtonEventKind::Pressed) {
                a_held_ = true;
            } else if (a_held_) {
                a_held_ = false;
                launch_focused();
            }
            return;
        }
        if (ev.kind != ButtonEventKind::Pressed) return;

        if (a_held_) {
            if (is_direction(ev.button) && begin_move_for_focused()) {
                move_carry(ev.button);
            }
            return;
        }

        switch (ev.button) {
            case Button::Up:    if (row_ > 0) { row_--; update_focus(); } break;
            case Button::Down:  if (row_ < GRID_ROWS - 1) { row_++; update_focus(); } break;
            case Button::Left:
                if (col_ > 0) { col_--; update_focus(); }
                else if (page_ > 0) { page_--; col_ = GRID_COLS - 1; refresh_cells(); update_focus(); }
                break;
            case Button::Right:
                if (col_ < GRID_COLS - 1) { col_++; update_focus(); }
                else if (page_ < page_count() - 1) { page_++; col_ = 0; refresh_cells(); update_focus(); }
                break;
            case Button::Start: launch_focused(); break;
            default: break;
        }
    }

private:
    int page_count() const
    {
        size_t n = visible_app_count();
        if (n == 0) return 1;
        return (int)((n + PAGE_SIZE - 1) / PAGE_SIZE);
    }

    static size_t visible_app_count()
    {
        return launcher_slugs_count();
    }

    static App *visible_app_at(size_t idx)
    {
        const char *slug = launcher_slugs_at(idx);
        return slug ? app_registry_find_by_slug(slug) : nullptr;
    }

    static bool is_direction(Button b)
    {
        return b == Button::Up || b == Button::Down
            || b == Button::Left || b == Button::Right;
    }

    void launch_focused()
    {
        size_t idx = focused_idx();
        App *a = visible_app_at(idx);
        if (a && a != this) {
            last_launched_idx_ = idx;
            app_registry_launch(a);
        }
    }

    void refresh_cells()
    {
        for (int r = 0; r < GRID_ROWS; r++) {
            for (int c = 0; c < GRID_COLS; c++) {
                size_t idx = (size_t)page_ * PAGE_SIZE + r * GRID_COLS + c;
                App *a = visible_app_at(idx);
                lv_obj_set_user_data(cells_[r][c], a);
                lv_obj_set_style_bg_color(cells_[r][c],
                    lv_color_hex(a ? COLOR_CELL : COLOR_CELL_EMPTY), 0);

                const lv_image_dsc_t *dsc = a ? a->icon_image() : nullptr;
                const char *path = a ? a->icon_image_path() : nullptr;
                lv_obj_set_style_image_recolor(images_[r][c], lv_color_hex(COLOR_TEXT), 0);
                lv_obj_set_style_image_recolor_opa(images_[r][c],
                    a && a->icon_is_mask() ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
                // Uploaded icons arrive at their final size from the flasher.
                // File images stream one row at a time, so never scale them.
                lv_image_set_scale(images_[r][c], dsc ? 224 : LV_SCALE_NONE);
                lv_image_set_src(images_[r][c], dsc ? static_cast<const void *>(dsc) : path);
                if (lv_image_get_src(images_[r][c])) {
                    lv_obj_clear_flag(images_[r][c], LV_OBJ_FLAG_HIDDEN);
                    lv_obj_add_flag(labels_[r][c], LV_OBJ_FLAG_HIDDEN);
                } else {
                    lv_label_set_text(labels_[r][c], a ? a->icon() : "-");
                    lv_obj_set_style_text_color(labels_[r][c],
                        lv_color_hex(a ? COLOR_TEXT : COLOR_TEXT_DIM), 0);
                    lv_obj_clear_flag(labels_[r][c], LV_OBJ_FLAG_HIDDEN);
                    lv_obj_add_flag(images_[r][c], LV_OBJ_FLAG_HIDDEN);
                }
            }
        }
        render_dots();
    }

    static void draw_icon_outline(lv_event_t *event)
    {
        lv_obj_t *cell = lv_event_get_target_obj(event);
        auto *app = static_cast<App *>(lv_obj_get_user_data(cell));
        if (!app || !app->icon_needs_outline() || !app->icon_image()) return;

        // Reuse the original alpha mask, avoiding extra image assets or
        // widgets. The normal image child draws over these four silhouettes.
        lv_area_t area;
        lv_obj_get_coords(lv_obj_get_child(cell, 1), &area);
        lv_draw_image_dsc_t outline;
        lv_draw_image_dsc_init(&outline);
        outline.src = app->icon_image();
        outline.scale_x = outline.scale_y = 224;
        outline.pivot = {24, 24};
        outline.recolor = lv_color_hex(0xffffff);
        outline.recolor_opa = LV_OPA_COVER;
        static constexpr int offsets[][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}};
        for (const auto &offset : offsets) {
            lv_area_t shifted = {area.x1 + offset[0], area.y1 + offset[1],
                                 area.x2 + offset[0], area.y2 + offset[1]};
            lv_draw_image(lv_event_get_layer(event), &outline, &shifted);
        }
    }

    static void draw_internal_outline(lv_event_t *event)
    {
        lv_obj_t *cell = lv_event_get_target_obj(event);
        auto *app = static_cast<App *>(lv_obj_get_user_data(cell));
        if (!app || app->access() != AppAccess::Internal) return;

        // Draw after the icon so the diagnostic marking stays visible in
        // normal, focused, and move states without extra per-dot objects.
        lv_area_t area;
        lv_obj_get_coords(cell, &area);
        lv_layer_t *layer = lv_event_get_layer(event);
        lv_draw_rect_dsc_t dot;
        lv_draw_rect_dsc_init(&dot);
        dot.bg_color = lv_color_hex(COLOR_INTERNAL);
        dot.bg_opa = LV_OPA_COVER;
        const int left = area.x1 + 2;
        const int right = area.x2 - 3;
        const int top = area.y1 + 2;
        const int bottom = area.y2 - 3;
        for (int x = left; x <= right; x += 5) {
            lv_area_t upper = {x, top, x + 1, top + 1};
            lv_area_t lower = {x, bottom, x + 1, bottom + 1};
            lv_draw_rect(layer, &dot, &upper);
            lv_draw_rect(layer, &dot, &lower);
        }
        for (int y = top + 5; y < bottom; y += 5) {
            lv_area_t lhs = {left, y, left + 1, y + 1};
            lv_area_t rhs = {right, y, right + 1, y + 1};
            lv_draw_rect(layer, &dot, &lhs);
            lv_draw_rect(layer, &dot, &rhs);
        }
    }

    lv_obj_t *make_arrow(const lv_point_precise_t *pts)
    {
        lv_obj_t *arrow = lv_line_create(pager_);
        lv_line_set_points(arrow, pts, 5);
        lv_obj_set_size(arrow, ARROW_W + 1, ARROW_H + 1);
        lv_obj_set_style_line_width(arrow, 2, 0);
        lv_obj_set_style_line_color(arrow, lv_color_hex(COLOR_TEXT), 0);
        lv_obj_set_style_line_rounded(arrow, false, 0);
        lv_obj_add_flag(arrow, LV_OBJ_FLAG_HIDDEN);
        return arrow;
    }

    void render_dots()
    {
        if (!pager_) return;

        const int pages = page_count();
        // A single page has nothing to page between, so show no strip at all.
        if (pages <= 1) {
            lv_obj_add_flag(pager_, LV_OBJ_FLAG_HIDDEN);
            return;
        }
        lv_obj_clear_flag(pager_, LV_OBJ_FLAG_HIDDEN);

        const int shown = pages < MAX_DOTS ? pages : MAX_DOTS;
        const int strip_w = shown * DOT_SIZE + (shown - 1) * DOT_GAP;
        const int x0 = (320 - strip_w) / 2;
        const int dot_y = (PAGER_H - DOT_SIZE) / 2;

        for (int i = 0; i < MAX_DOTS; i++) {
            lv_obj_t *dot = dots_[i];
            if (!dot) continue;
            if (i >= shown) {
                lv_obj_add_flag(dot, LV_OBJ_FLAG_HIDDEN);
                continue;
            }
            lv_obj_clear_flag(dot, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_pos(dot, x0 + i * (DOT_SIZE + DOT_GAP), dot_y);

            const bool active = (i == page_);
            lv_obj_set_style_bg_color(dot, lv_color_hex(COLOR_DOT_ACTIVE), 0);
            lv_obj_set_style_bg_opa(dot, active ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_color(dot, lv_color_hex(COLOR_DOT_IDLE), 0);
            lv_obj_set_style_border_width(dot, active ? 0 : 1, 0);
        }

        const int arrow_y = (PAGER_H - ARROW_H) / 2;
        place_arrow(arrow_left_, EDGE_PAD, arrow_y, page_ > 0);
        place_arrow(arrow_right_, 320 - EDGE_PAD - ARROW_W - 1, arrow_y,
                    page_ < pages - 1);
    }

    static void place_arrow(lv_obj_t *arrow, int x, int y, bool visible)
    {
        if (!arrow) return;
        lv_obj_set_pos(arrow, x, y);
        if (visible) lv_obj_clear_flag(arrow, LV_OBJ_FLAG_HIDDEN);
        else         lv_obj_add_flag(arrow, LV_OBJ_FLAG_HIDDEN);
    }

    void update_focus()
    {
        App *focused = visible_app_at(focused_idx());
        if (name_) lv_label_set_text(name_, focused ? focused->name() : "Apps");
        bool moving = (mode_ == Mode::Move);
        for (int r = 0; r < GRID_ROWS; r++) {
            for (int c = 0; c < GRID_COLS; c++) {
                lv_obj_t *cell = cells_[r][c];
                if (!cell) continue;
                bool focused = (r == row_ && c == col_);
                const size_t idx = (size_t)page_ * PAGE_SIZE + r * GRID_COLS + c;
                App *a = visible_app_at(idx);
                lv_obj_set_style_bg_color(cell, lv_color_hex(
                    focused ? (a ? COLOR_CELL_FOCUS : BadgeTheme::TRACK)
                            : (a ? COLOR_CELL : COLOR_CELL_EMPTY)), 0);
                lv_obj_set_style_border_color(cell,
                    lv_color_hex(focused ? COLOR_BORDER_FOC : COLOR_BORDER), 0);
                lv_obj_set_style_border_width(cell, focused ? (moving ? 3 : 2) : 1, 0);
                // User-installed (Lua) apps get a persistent outer outline so
                // they read as user-built in every state; move mode reuses the
                // same ring on the focused cell.
                const bool user_outline = (a && a->is_user_installed());
                const bool show_outline = user_outline || (focused && moving);
                lv_obj_set_style_outline_color(cell, lv_color_hex(COLOR_BORDER_FOC), 0);
                lv_obj_set_style_outline_opa(cell, show_outline ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
                lv_obj_set_style_outline_width(cell, show_outline ? 1 : 0, 0);
                lv_obj_set_style_outline_pad(cell, 1, 0);
                lv_obj_set_style_text_color(labels_[r][c],
                    lv_color_hex(focused && a ? COLOR_TEXT
                                              : (a ? COLOR_BORDER_FOC : COLOR_TEXT_DIM)), 0);
            }
        }
    }

    size_t focused_idx() const
    {
        return (size_t)page_ * PAGE_SIZE + row_ * GRID_COLS + col_;
    }

    // ---- Move mode ----

    bool begin_move_for_focused()
    {
        App *a = visible_app_at(focused_idx());
        if (!a || a == this) return false;
        target_app_ = a;
        enter_move_mode();
        return mode_ == Mode::Move;
    }

    void enter_move_mode()
    {
        if (!target_app_) return;
        size_t grid_idx = focused_idx();
        carry_grid_idx_ = grid_idx;
        mode_ = Mode::Move;
        update_focus();
    }

    void on_button_move(ButtonEvent ev)
    {
        if (ev.button == Button::A && ev.kind == ButtonEventKind::Released) {
            a_held_ = false;
            exit_move_mode();
            return;
        }
        if (ev.kind != ButtonEventKind::Pressed) return;
        if (a_held_) move_carry(ev.button);
    }

    void move_carry(Button b)
    {
        size_t total = visible_app_count();
        switch (b) {
            case Button::Left:
                if (carry_grid_idx_ > 0) move_carry_to(carry_grid_idx_ - 1);
                break;
            case Button::Right:
                if (carry_grid_idx_ + 1 < total) move_carry_to(carry_grid_idx_ + 1);
                break;
            case Button::Up:
                if (carry_grid_idx_ >= (size_t)GRID_COLS) {
                    size_t t = carry_grid_idx_ - GRID_COLS;
                    move_carry_to(t);
                }
                break;
            case Button::Down: {
                size_t t = carry_grid_idx_ + GRID_COLS;
                if (t < total) move_carry_to(t);
                break;
            }
            default: break;
        }
    }

    void move_carry_to(size_t new_grid_idx)
    {
        if (target_app_) launcher_slugs_move(target_app_->slug(), new_grid_idx);
        carry_grid_idx_ = new_grid_idx;
        page_ = (int)(carry_grid_idx_ / PAGE_SIZE);
        int within = (int)(carry_grid_idx_ % PAGE_SIZE);
        row_ = within / GRID_COLS;
        col_ = within % GRID_COLS;
        refresh_cells();
        update_focus();
    }

    void exit_move_mode()
    {
        target_app_ = nullptr;
        mode_ = Mode::Grid;
        refresh_cells();
        update_focus();
    }

    enum class Mode { Grid, Move };

    lv_obj_t *grid_ = nullptr;
    lv_obj_t *name_ = nullptr;
    lv_obj_t *pager_ = nullptr;
    lv_obj_t *dots_[MAX_DOTS] = {};
    lv_obj_t *arrow_left_ = nullptr;
    lv_obj_t *arrow_right_ = nullptr;
    lv_obj_t *cells_[GRID_ROWS][GRID_COLS] = {};
    lv_obj_t *labels_[GRID_ROWS][GRID_COLS] = {};
    lv_obj_t *images_[GRID_ROWS][GRID_COLS] = {};
    int page_ = 0;
    int row_ = 0;
    int col_ = 0;
    size_t last_launched_idx_ = SIZE_MAX;

    Mode mode_ = Mode::Grid;
    App *target_app_ = nullptr;

    // Defaults and unlocked apps share this persisted move index space.
    size_t carry_grid_idx_ = 0;
    bool a_held_ = false;
};

static LauncherApp s_launcher;
App *app_launcher_instance() { return &s_launcher; }
