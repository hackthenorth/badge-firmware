#include "app.h"
#include "hal_config.h"

#include "esp_random.h"

#include <cstdint>
#include <climits>

namespace {

constexpr int kColumns = 6;
constexpr int kRows = 4;
constexpr int kBubbleCount = kColumns * kRows;
constexpr int kAnimationFrames = 10;
// LittleFS rewrites the whole config file. Keep that work out of the button
// handler, then coalesce a burst of pops after its animation has settled.
constexpr uint32_t kPersistIdleMs = 500;

constexpr uint32_t kBubbleFills[] = {
    0x173747,
    0x1c4254,
    0x225066,
    0x285a70,
};
constexpr uint32_t kBubbleBorder = 0x65cde2;
constexpr uint32_t kPopFlash = 0xd8f7ff;

constexpr int kPopSizes[kAnimationFrames] = {
    42, 47, 50, 46, 40, 34, 28, 23, 18, 14,
};

extern "C" const lv_image_dsc_t icon_bubblewrap;

class BubbleWrapApp final : public GameApp {
public:
    const char *name() const override { return "Bubble Wrap"; }
    const char *icon() const override { return "POP"; }
    const lv_image_dsc_t *icon_image() const override { return &icon_bubblewrap; }
    const char *slug() const override { return "bubblewrap"; }

    void on_enter(lv_obj_t *root) override
    {
        root_ = root;
        total_popped_ = hal_config_get_int(slug(), "total_popped", 0);
        if (total_popped_ < 0) total_popped_ = 0;
        total_dirty_ = false;
        rng_state_ = esp_random() ^ lv_tick_get() ^ 0x504f5021u;
        if (rng_state_ == 0) rng_state_ = 0x6d2b79f5u;

        lv_obj_set_style_bg_color(root_, lv_color_hex(BadgeTheme::BACKGROUND), 0);
        lv_obj_set_style_bg_opa(root_, LV_OPA_COVER, 0);

        title_ = lv_label_create(root_);
        lv_label_set_text(title_, "BUBBLE WRAP");
        lv_obj_set_style_text_font(title_, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(title_, lv_color_hex(BadgeTheme::TEXT), 0);
        lv_obj_set_pos(title_, 10, 5);

        counter_ = lv_label_create(root_);
        lv_obj_set_style_text_font(counter_, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(counter_, lv_color_hex(BadgeTheme::ACCENT_DETAIL), 0);
        lv_obj_align(counter_, LV_ALIGN_TOP_RIGHT, -10, 9);

        total_ = lv_label_create(root_);
        lv_obj_set_style_text_font(total_, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(total_, lv_color_hex(BadgeTheme::TEXT_SOFT), 0);
        lv_obj_align(total_, LV_ALIGN_TOP_MID, 0, 27);

        for (int i = 0; i < kBubbleCount; ++i) {
            bubbles_[i] = lv_obj_create(root_);
            lv_obj_remove_style_all(bubbles_[i]);
            lv_obj_set_style_bg_opa(bubbles_[i], LV_OPA_COVER, 0);
            lv_obj_set_style_radius(bubbles_[i], LV_RADIUS_CIRCLE, 0);
        }

        hint_ = lv_label_create(root_);
        lv_obj_set_width(hint_, 310);
        lv_obj_set_style_text_align(hint_, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_font(hint_, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(hint_, lv_color_hex(BadgeTheme::TEXT_DIM), 0);
        lv_obj_align(hint_, LV_ALIGN_BOTTOM_MID, 0, -5);

        reset_sheet();
    }

    void on_exit() override
    {
        persist_total();
        root_ = nullptr;
        title_ = nullptr;
        counter_ = nullptr;
        total_ = nullptr;
        hint_ = nullptr;
        for (lv_obj_t *&bubble : bubbles_) bubble = nullptr;
    }

    void checkpoint_progress() override
    {
        persist_total();
    }

    void on_button(ButtonEvent event) override
    {
        if (event.kind != ButtonEventKind::Pressed || event.button == Button::Home) return;

        if (popped_count_ == kBubbleCount) {
            reset_sheet();
            return;
        }
        pop_random_bubble();
    }

    void on_tick() override
    {
        for (int i = 0; i < kBubbleCount; ++i) {
            if (animation_frame_[i] < 0) continue;
            animation_frame_[i]++;
            if (animation_frame_[i] >= kAnimationFrames) animation_frame_[i] = -1;
            update_bubble(i);
        }
        if (total_dirty_ && lv_tick_elaps(last_pop_ms_) >= kPersistIdleMs) {
            persist_total();
        }
    }

private:
    uint32_t random_u32()
    {
        uint32_t value = rng_state_;
        value ^= value << 13;
        value ^= value >> 17;
        value ^= value << 5;
        rng_state_ = value;
        return value;
    }

    void reset_sheet()
    {
        popped_count_ = 0;
        for (int i = 0; i < kBubbleCount; ++i) {
            popped_[i] = false;
            animation_frame_[i] = -1;
            fill_index_[i] = static_cast<uint8_t>(random_u32() % 4u);
            update_bubble(i);
        }
        update_copy();
    }

    void pop_random_bubble()
    {
        int available[kBubbleCount];
        int available_count = 0;
        for (int i = 0; i < kBubbleCount; ++i) {
            if (!popped_[i]) available[available_count++] = i;
        }
        if (available_count == 0) return;

        const int pick = available[random_u32() % static_cast<uint32_t>(available_count)];
        popped_[pick] = true;
        animation_frame_[pick] = 0;
        popped_count_++;
        if (total_popped_ < INT_MAX) {
            ++total_popped_;
            total_dirty_ = true;
            last_pop_ms_ = lv_tick_get();
        }
        update_bubble(pick);
        update_copy();
    }

    void persist_total()
    {
        if (!total_dirty_) return;
        hal_config_set_int(slug(), "total_popped", total_popped_);
        total_dirty_ = false;
    }

    void update_bubble(int index)
    {
        lv_obj_t *bubble = bubbles_[index];
        if (!bubble) return;

        int size = 40;
        uint32_t fill = kBubbleFills[fill_index_[index]];
        uint32_t border = kBubbleBorder;
        int border_width = 2;

        if (popped_[index]) {
            const int frame = animation_frame_[index];
            if (frame >= 0) {
                size = kPopSizes[frame];
                fill = frame < 4 ? kPopFlash : BadgeTheme::ACCENT;
                border = frame < 4 ? 0xffffff : BadgeTheme::ACCENT_DETAIL;
                border_width = frame < 6 ? 3 : 2;
            } else {
                size = 14;
                fill = BadgeTheme::TRACK;
                border = BadgeTheme::BORDER;
                border_width = 1;
            }
        }

        const int column = index % kColumns;
        const int row = index / kColumns;
        const int centre_x = 30 + column * 52;
        const int centre_y = 68 + row * 40;
        lv_obj_set_size(bubble, size, size);
        lv_obj_set_pos(bubble, centre_x - size / 2, centre_y - size / 2);
        lv_obj_set_style_bg_color(bubble, lv_color_hex(fill), 0);
        lv_obj_set_style_border_color(bubble, lv_color_hex(border), 0);
        lv_obj_set_style_border_width(bubble, border_width, 0);
    }

    void update_copy()
    {
        if (!counter_ || !total_ || !hint_) return;
        lv_label_set_text_fmt(total_, "ALL-TIME POPPED: %d", total_popped_);
        const int remaining = kBubbleCount - popped_count_;
        if (remaining == 0) {
            lv_label_set_text(counter_, "ALL POPPED!");
            lv_obj_set_style_text_color(counter_, lv_color_hex(0x65e6a8), 0);
            lv_label_set_text(hint_, "PRESS ANY BUTTON FOR A NEW SHEET");
            lv_obj_set_style_text_color(hint_, lv_color_hex(BadgeTheme::ACCENT_DETAIL), 0);
        } else {
            lv_label_set_text_fmt(counter_, "%d LEFT", remaining);
            lv_obj_set_style_text_color(counter_, lv_color_hex(BadgeTheme::ACCENT_DETAIL), 0);
            lv_label_set_text(hint_, "PRESS ANY BUTTON TO POP   HOME: APPS");
            lv_obj_set_style_text_color(hint_, lv_color_hex(BadgeTheme::TEXT_DIM), 0);
        }
    }

    lv_obj_t *root_ = nullptr;
    lv_obj_t *title_ = nullptr;
    lv_obj_t *counter_ = nullptr;
    lv_obj_t *total_ = nullptr;
    lv_obj_t *hint_ = nullptr;
    lv_obj_t *bubbles_[kBubbleCount] = {};

    bool popped_[kBubbleCount] = {};
    int8_t animation_frame_[kBubbleCount] = {};
    uint8_t fill_index_[kBubbleCount] = {};
    int popped_count_ = 0;
    int total_popped_ = 0;
    bool total_dirty_ = false;
    uint32_t last_pop_ms_ = 0;
    uint32_t rng_state_ = 0;
};

BubbleWrapApp s_instance;

} // namespace

App *app_bubble_wrap_instance() { return &s_instance; }
