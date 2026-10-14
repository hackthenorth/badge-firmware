#include "app.h"
#include "hal_config.h"

#include "esp_random.h"

#include <cstddef>
#include <cstdint>

namespace {

constexpr int kBoardSize = 4;
constexpr int kCellSize = 50;
constexpr int kCellGap = 4;
constexpr int kBoardX = 6;
constexpr int kBoardY = 9;
constexpr int kCellStride = kCellSize + kCellGap;
constexpr uint32_t kSlideDurationMs = 130;
constexpr uint32_t kPopDurationMs = 140;

enum class Direction : uint8_t { Left, Right, Up, Down };

uint32_t tile_color(uint32_t value)
{
    switch (value) {
    case 0: return 0x1a2532;
    case 2: return 0xddebd6;
    case 4: return 0xc4e8d6;
    case 8: return 0x8de0c1;
    case 16: return 0x58cfb2;
    case 32: return 0x39bba9;
    case 64: return 0x278fa1;
    case 128: return 0x537fd1;
    case 256: return 0x766bd4;
    case 512: return 0x9a58c7;
    case 1024: return 0xd052a8;
    default: return 0xf05f78;
    }
}

uint32_t tile_text_color(uint32_t value)
{
    return value > 0 && value <= 8 ? 0x14201c : 0xffffff;
}

extern "C" const lv_image_dsc_t icon_2048;

class Game2048App final : public GameApp {
public:
    const char *name() const override { return "Merge Mode"; }
    const char *icon() const override { return "MRG"; }
    const lv_image_dsc_t *icon_image() const override { return &icon_2048; }
    const char *slug() const override { return "2048"; }
    bool wants_wake_lock() const override { return true; }

    void on_enter(lv_obj_t *root) override
    {
        root_ = root;
        high_score_ = hal_config_get_int("2048", "high_score", 0);
        saved_high_score_ = high_score_;

        lv_obj_set_style_bg_color(root_, lv_color_hex(BadgeTheme::BACKGROUND), 0);
        lv_obj_set_style_bg_opa(root_, LV_OPA_COVER, 0);

        board_panel_ = lv_obj_create(root_);
        lv_obj_set_size(board_panel_, 218, 218);
        lv_obj_set_pos(board_panel_, kBoardX, kBoardY);
        lv_obj_set_style_bg_color(board_panel_, lv_color_hex(0x0d151e), 0);
        lv_obj_set_style_bg_opa(board_panel_, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(board_panel_, 1, 0);
        lv_obj_set_style_border_color(board_panel_, lv_color_hex(0x33485c), 0);
        lv_obj_set_style_radius(board_panel_, 8, 0);
        lv_obj_set_style_pad_all(board_panel_, 0, 0);
        lv_obj_clear_flag(board_panel_, LV_OBJ_FLAG_SCROLLABLE);

        for (int row = 0; row < kBoardSize; ++row) {
            for (int col = 0; col < kBoardSize; ++col) {
                const int index = row * kBoardSize + col;
                tiles_[index] = lv_obj_create(board_panel_);
                lv_obj_set_size(tiles_[index], kCellSize, kCellSize);
                lv_obj_set_pos(tiles_[index], 4 + col * kCellStride,
                    4 + row * kCellStride);
                lv_obj_set_style_radius(tiles_[index], 7, 0);
                lv_obj_set_style_border_width(tiles_[index], 0, 0);
                lv_obj_set_style_pad_all(tiles_[index], 0, 0);
                lv_obj_clear_flag(tiles_[index], LV_OBJ_FLAG_SCROLLABLE);

                labels_[index] = lv_label_create(tiles_[index]);
                lv_obj_center(labels_[index]);
            }
        }

        lv_obj_t *title = lv_label_create(root_);
        lv_label_set_text(title, "MERGE MODE");
        lv_obj_set_width(title, 88);
        lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(title, lv_color_hex(BadgeTheme::ACCENT_DETAIL), 0);
        lv_obj_set_pos(title, 228, 13);

        score_label_ = lv_label_create(root_);
        lv_obj_set_width(score_label_, 88);
        lv_obj_set_style_text_align(score_label_, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_font(score_label_, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(score_label_, lv_color_hex(BadgeTheme::TEXT), 0);
        lv_obj_set_pos(score_label_, 228, 48);

        high_label_ = lv_label_create(root_);
        lv_obj_set_width(high_label_, 88);
        lv_obj_set_style_text_align(high_label_, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_font(high_label_, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(high_label_, lv_color_hex(BadgeTheme::TEXT_DIM), 0);
        lv_obj_set_pos(high_label_, 228, 86);

        status_label_ = lv_label_create(root_);
        lv_obj_set_width(status_label_, 88);
        lv_label_set_long_mode(status_label_, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_align(status_label_, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_font(status_label_, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(status_label_, lv_color_hex(0xffd166), 0);
        lv_obj_set_pos(status_label_, 228, 122);

        lv_obj_t *hint = lv_label_create(root_);
        lv_label_set_text(hint, "D-PAD\nslide\n\nSTART\nnew game");
        lv_obj_set_width(hint, 88);
        lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_font(hint, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(hint, lv_color_hex(BadgeTheme::TEXT_DIM), 0);
        lv_obj_set_pos(hint, 228, 157);

        new_game();
    }

    void on_exit() override
    {
        persist_high_score();
        stop_animations();
        root_ = board_panel_ = score_label_ = high_label_ = status_label_ = nullptr;
        for (lv_obj_t *&tile : tiles_) tile = nullptr;
        for (lv_obj_t *&label : labels_) label = nullptr;
    }

    void on_home_confirmation_cancel(uint32_t paused_ms) override
    {
        animation_started_ms_ += paused_ms;
    }

    void checkpoint_progress() override
    {
        if (score_ > high_score_) high_score_ = score_;
        persist_high_score();
    }

    void on_tick() override
    {
        const uint32_t now = lv_tick_get();
        if (slide_animating_) tick_slide_animation(now);
        else if (pop_animating_) tick_pop_animation(now);
    }

    void on_button(ButtonEvent ev) override
    {
        if (ev.kind != ButtonEventKind::Pressed) return;
        if (ev.button == Button::Start) {
            new_game();
            return;
        }

        // A movement animation is deliberately shorter than a normal button
        // repeat interval. Ignoring input during that tiny window keeps model
        // state and visual state in lockstep without a move queue.
        if (slide_animating_) return;

        Direction direction;
        if (ev.button == Button::Left) direction = Direction::Left;
        else if (ev.button == Button::Right) direction = Direction::Right;
        else if (ev.button == Button::Up) direction = Direction::Up;
        else if (ev.button == Button::Down) direction = Direction::Down;
        else if (game_over_ && ev.button == Button::A) {
            new_game();
            return;
        } else {
            return;
        }

        if (game_over_) return;
        stop_animations();
        reset_transition_plan();
        const bool changed = slide(direction);
        if (changed) {
            const int spawned = spawn_tile();
            if (spawned >= 0) spawned_cells_[spawned] = true;
        }
        game_over_ = !moves_available();
        if (score_ > high_score_) high_score_ = score_;
        render();
        if (changed) {
            uint32_t largest = 0;
            for (uint32_t value : board_) if (value > largest) largest = value;
            if (largest > static_cast<uint32_t>(hal_config_get_int("2048", "max_tile", 0)))
                hal_config_set_int("2048", "max_tile", static_cast<int>(largest));
            start_slide_animation();
        }
        if (game_over_) persist_high_score();
    }

private:
    struct CompactTile {
        uint32_t value;
        int source;
    };

    static int cell_index(Direction direction, int line, int offset)
    {
        switch (direction) {
        case Direction::Left:  return line * kBoardSize + offset;
        case Direction::Right: return line * kBoardSize + (kBoardSize - 1 - offset);
        case Direction::Up:    return offset * kBoardSize + line;
        case Direction::Down:  return (kBoardSize - 1 - offset) * kBoardSize + line;
        }
        return 0;
    }

    bool slide(Direction direction)
    {
        bool changed = false;
        for (int line = 0; line < kBoardSize; ++line) {
            CompactTile compact[kBoardSize] = {};
            int compact_count = 0;
            for (int offset = 0; offset < kBoardSize; ++offset) {
                const int source = cell_index(direction, line, offset);
                const uint32_t value = board_[source];
                if (value != 0) compact[compact_count++] = { value, source };
            }

            uint32_t merged[kBoardSize] = {};
            int merged_count = 0;
            for (int i = 0; i < compact_count;) {
                const int destination = cell_index(direction, line, merged_count);
                if (i + 1 < compact_count
                    && compact[i].value == compact[i + 1].value) {
                    merged[merged_count] = compact[i].value * 2;
                    animation_sources_[destination] = compact[i + 1].source;
                    merged_cells_[destination] = true;
                    score_ += static_cast<int>(merged[merged_count]);
                    ++merged_count;
                    i += 2;
                } else {
                    merged[merged_count] = compact[i].value;
                    animation_sources_[destination] = compact[i].source;
                    ++merged_count;
                    ++i;
                }
            }

            for (int offset = 0; offset < kBoardSize; ++offset) {
                const int index = cell_index(direction, line, offset);
                const uint32_t next = offset < merged_count ? merged[offset] : 0;
                if (board_[index] != next) changed = true;
                board_[index] = next;
            }
        }
        return changed;
    }

    int spawn_tile()
    {
        int empty[kBoardSize * kBoardSize];
        int empty_count = 0;
        for (int i = 0; i < kBoardSize * kBoardSize; ++i) {
            if (board_[i] == 0) empty[empty_count++] = i;
        }
        if (empty_count == 0) return -1;

        const int index = empty[esp_random() % static_cast<uint32_t>(empty_count)];
        board_[index] = (esp_random() % 10 == 0) ? 4 : 2;
        return index;
    }

    bool moves_available() const
    {
        for (int row = 0; row < kBoardSize; ++row) {
            for (int col = 0; col < kBoardSize; ++col) {
                const uint32_t value = board_[row * kBoardSize + col];
                if (value == 0) return true;
                if (col + 1 < kBoardSize
                    && value == board_[row * kBoardSize + col + 1]) return true;
                if (row + 1 < kBoardSize
                    && value == board_[(row + 1) * kBoardSize + col]) return true;
            }
        }
        return false;
    }

    bool reached_2048() const
    {
        for (uint32_t value : board_) {
            if (value >= 2048) return true;
        }
        return false;
    }

    void new_game()
    {
        persist_high_score();
        stop_animations();
        reset_transition_plan();
        for (uint32_t &value : board_) value = 0;
        score_ = 0;
        game_over_ = false;
        const int first = spawn_tile();
        const int second = spawn_tile();
        if (first >= 0) spawned_cells_[first] = true;
        if (second >= 0) spawned_cells_[second] = true;
        render();
        start_pop_animation(lv_tick_get());
    }

    static int eased_progress(uint32_t elapsed, uint32_t duration)
    {
        if (elapsed >= duration) return 256;
        const int progress = static_cast<int>(elapsed * 256u / duration);
        const int remaining = 256 - progress;
        return 256 - (remaining * remaining * remaining) / (256 * 256);
    }

    void reset_transition_plan()
    {
        for (int i = 0; i < kBoardSize * kBoardSize; ++i) {
            animation_sources_[i] = -1;
            merged_cells_[i] = false;
            spawned_cells_[i] = false;
        }
    }

    void stop_animations()
    {
        slide_animating_ = false;
        pop_animating_ = false;
        for (lv_obj_t *tile : tiles_) {
            if (!tile) continue;
            lv_obj_set_style_translate_x(tile, 0, 0);
            lv_obj_set_style_translate_y(tile, 0, 0);
            lv_obj_set_style_transform_scale(tile, 256, 0);
            lv_obj_set_style_opa(tile, LV_OPA_COVER, 0);
        }
    }

    void start_slide_animation()
    {
        bool has_motion = false;
        for (int destination = 0; destination < kBoardSize * kBoardSize;
             ++destination) {
            const int source = animation_sources_[destination];
            if (source < 0 || source == destination) continue;
            const int source_col = source % kBoardSize;
            const int source_row = source / kBoardSize;
            const int destination_col = destination % kBoardSize;
            const int destination_row = destination / kBoardSize;
            lv_obj_set_style_translate_x(tiles_[destination],
                (source_col - destination_col) * kCellStride, 0);
            lv_obj_set_style_translate_y(tiles_[destination],
                (source_row - destination_row) * kCellStride, 0);
            has_motion = true;
        }
        for (int i = 0; i < kBoardSize * kBoardSize; ++i) {
            if (spawned_cells_[i]) lv_obj_set_style_opa(tiles_[i], LV_OPA_TRANSP, 0);
        }

        if (!has_motion) {
            start_pop_animation(lv_tick_get());
            return;
        }
        slide_animating_ = true;
        animation_started_ms_ = lv_tick_get();
    }

    void tick_slide_animation(uint32_t now)
    {
        const uint32_t elapsed = now - animation_started_ms_;
        const int progress = eased_progress(elapsed, kSlideDurationMs);
        const int remaining = 256 - progress;
        for (int destination = 0; destination < kBoardSize * kBoardSize;
             ++destination) {
            const int source = animation_sources_[destination];
            if (source < 0 || source == destination) continue;
            const int source_col = source % kBoardSize;
            const int source_row = source / kBoardSize;
            const int destination_col = destination % kBoardSize;
            const int destination_row = destination / kBoardSize;
            lv_obj_set_style_translate_x(tiles_[destination],
                (source_col - destination_col) * kCellStride * remaining / 256, 0);
            lv_obj_set_style_translate_y(tiles_[destination],
                (source_row - destination_row) * kCellStride * remaining / 256, 0);
        }

        if (elapsed < kSlideDurationMs) return;
        slide_animating_ = false;
        for (lv_obj_t *tile : tiles_) {
            lv_obj_set_style_translate_x(tile, 0, 0);
            lv_obj_set_style_translate_y(tile, 0, 0);
        }
        start_pop_animation(now);
    }

    void start_pop_animation(uint32_t now)
    {
        bool has_effect = false;
        for (int i = 0; i < kBoardSize * kBoardSize; ++i) {
            if (!merged_cells_[i] && !spawned_cells_[i]) continue;
            has_effect = true;
            lv_obj_set_style_opa(tiles_[i], LV_OPA_COVER, 0);
            lv_obj_set_style_transform_pivot_x(tiles_[i], kCellSize / 2, 0);
            lv_obj_set_style_transform_pivot_y(tiles_[i], kCellSize / 2, 0);
            lv_obj_set_style_transform_scale(tiles_[i],
                spawned_cells_[i] ? 176 : 256, 0);
        }
        pop_animating_ = has_effect;
        animation_started_ms_ = now;
    }

    void tick_pop_animation(uint32_t now)
    {
        const uint32_t elapsed = now - animation_started_ms_;
        const int progress = elapsed >= kPopDurationMs
            ? 256
            : static_cast<int>(elapsed * 256u / kPopDurationMs);

        for (int i = 0; i < kBoardSize * kBoardSize; ++i) {
            if (spawned_cells_[i]) {
                const int scale = 176 + eased_progress(elapsed, kPopDurationMs) * 80 / 256;
                lv_obj_set_style_transform_scale(tiles_[i], scale, 0);
            } else if (merged_cells_[i]) {
                const int pulse = progress < 128 ? progress : 256 - progress;
                lv_obj_set_style_transform_scale(tiles_[i], 256 + pulse / 3, 0);
            }
        }

        if (elapsed < kPopDurationMs) return;
        pop_animating_ = false;
        for (lv_obj_t *tile : tiles_) {
            lv_obj_set_style_transform_scale(tile, 256, 0);
            lv_obj_set_style_opa(tile, LV_OPA_COVER, 0);
        }
    }

    void persist_high_score()
    {
        if (high_score_ <= saved_high_score_) return;
        hal_config_set_int("2048", "high_score", high_score_);
        saved_high_score_ = high_score_;
    }

    void render()
    {
        for (int i = 0; i < kBoardSize * kBoardSize; ++i) {
            const uint32_t value = board_[i];
            lv_obj_set_style_bg_color(tiles_[i], lv_color_hex(tile_color(value)), 0);
            lv_obj_set_style_bg_opa(tiles_[i], LV_OPA_COVER, 0);
            lv_obj_set_style_text_color(labels_[i], lv_color_hex(tile_text_color(value)), 0);
            const lv_font_t *font = value >= 1024 ? &lv_font_montserrat_14
                : value >= 128 ? &lv_font_montserrat_14
                : &lv_font_montserrat_18;
            lv_obj_set_style_text_font(labels_[i], font, 0);
            if (value == 0) lv_label_set_text(labels_[i], "");
            else lv_label_set_text_fmt(labels_[i], "%lu", static_cast<unsigned long>(value));
            lv_obj_center(labels_[i]);
        }

        lv_label_set_text_fmt(score_label_, "SCORE\n%d", score_);
        lv_label_set_text_fmt(high_label_, "BEST\n%d", high_score_);
        if (game_over_) lv_label_set_text(status_label_, "No moves!\nA to retry");
        else if (reached_2048()) lv_label_set_text(status_label_, "2048!\nKeep going");
        else lv_label_set_text(status_label_, "Join equal\nnumbers");
    }

    lv_obj_t *root_ = nullptr;
    lv_obj_t *board_panel_ = nullptr;
    lv_obj_t *tiles_[kBoardSize * kBoardSize] = {};
    lv_obj_t *labels_[kBoardSize * kBoardSize] = {};
    lv_obj_t *score_label_ = nullptr;
    lv_obj_t *high_label_ = nullptr;
    lv_obj_t *status_label_ = nullptr;
    uint32_t board_[kBoardSize * kBoardSize] = {};
    int score_ = 0;
    int high_score_ = 0;
    int saved_high_score_ = 0;
    bool game_over_ = false;
    int animation_sources_[kBoardSize * kBoardSize] = {};
    bool merged_cells_[kBoardSize * kBoardSize] = {};
    bool spawned_cells_[kBoardSize * kBoardSize] = {};
    bool slide_animating_ = false;
    bool pop_animating_ = false;
    uint32_t animation_started_ms_ = 0;
};

Game2048App s_instance;

} // namespace

App *app_2048_instance() { return &s_instance; }
