#include "app.h"
#include "hal_accel.h"

#include "esp_random.h"

#include <cmath>
#include <cstddef>
#include <cstdint>

namespace {

constexpr int kArenaLeft = 4;
constexpr int kArenaRight = 316;
constexpr int kArenaTop = 31;
constexpr int kArenaBottom = 214;
constexpr int kStartingItems = 5;
constexpr int kMaxItems = 9;
constexpr int kPhysicsSubsteps = 3;
constexpr float kMaxSpeed = 6.5f;
constexpr float kRestitution = 0.56f;
constexpr uint32_t kShakeCooldownMs = 30;

enum class HardwareKind : uint8_t {
    Arduino,
    Box,
    Hammer,
    SolderingIron,
    Printer,
    Multimeter,
    Breadboard,
    Count,
};

struct ItemSpec {
    int width;
    int height;
    uint32_t fill;
    uint32_t border;
};

constexpr ItemSpec kItemSpecs[] = {
    {58, 42, 0x123e45, 0x4dd9d0}, // Arduino-style board
    {52, 42, 0x704829, 0xe4ad65}, // shipping box
    {58, 38, 0x252a35, 0xaeb9c9}, // hammer
    {58, 34, 0x38233f, 0xd99be6}, // soldering iron
    {54, 48, 0x222329, 0xff8c42}, // 3D printer
    {44, 48, 0x4a4011, 0xffd84d}, // multimeter
    {58, 36, 0x3a3a37, 0xf1f1e8}, // breadboard
};

constexpr HardwareKind kStarterKinds[kStartingItems] = {
    HardwareKind::Arduino,
    HardwareKind::Box,
    HardwareKind::Hammer,
    HardwareKind::SolderingIron,
    HardwareKind::Printer,
};

static_assert(sizeof(kItemSpecs) / sizeof(kItemSpecs[0])
              == static_cast<size_t>(HardwareKind::Count));

struct PhysicsItem {
    lv_obj_t *sprite = nullptr;
    HardwareKind kind = HardwareKind::Box;
    float x = 0.0f;
    float y = 0.0f;
    float vx = 0.0f;
    float vy = 0.0f;
    int width = 0;
    int height = 0;
};

float clamp_float(float value, float low, float high)
{
    if (value < low) return low;
    if (value > high) return high;
    return value;
}

lv_obj_t *make_block(lv_obj_t *parent, int x, int y, int width, int height,
                     uint32_t color, int radius = 1)
{
    lv_obj_t *block = lv_obj_create(parent);
    lv_obj_remove_style_all(block);
    lv_obj_set_pos(block, x, y);
    lv_obj_set_size(block, width, height);
    lv_obj_set_style_bg_color(block, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(block, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(block, radius, 0);
    return block;
}

void add_label(lv_obj_t *parent, const char *text, uint32_t color,
               lv_align_t align, int x, int y)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_obj_align(label, align, x, y);
}

void draw_arduino(lv_obj_t *sprite)
{
    make_block(sprite, 3, 4, 4, 34, 0xe7c35d, 1);
    make_block(sprite, 51, 4, 4, 34, 0xe7c35d, 1);
    make_block(sprite, 19, 13, 22, 17, 0x11191c, 2);
    make_block(sprite, 23, 16, 14, 3, 0x657178, 1);
    make_block(sprite, 23, 23, 9, 3, 0x657178, 1);
    make_block(sprite, 9, 7, 7, 7, 0xc9f4ef, LV_RADIUS_CIRCLE);
    make_block(sprite, 43, 30, 5, 5, 0xff7a66, LV_RADIUS_CIRCLE);
}

void draw_box(lv_obj_t *sprite)
{
    make_block(sprite, 22, 1, 8, 40, 0xd69b57, 0);
    make_block(sprite, 2, 18, 48, 3, 0x4e301d, 0);
    make_block(sprite, 7, 7, 11, 3, 0xf2c37d, 0);
    make_block(sprite, 34, 28, 10, 3, 0x4e301d, 0);
    add_label(sprite, "HW", 0xffe0ad, LV_ALIGN_CENTER, 0, 7);
}

void draw_hammer(lv_obj_t *sprite)
{
    make_block(sprite, 8, 5, 38, 11, 0xc5ccd4, 3);
    make_block(sprite, 5, 7, 9, 7, 0xe5e9ee, 2);
    make_block(sprite, 43, 3, 9, 15, 0x87929e, 2);
    make_block(sprite, 25, 14, 9, 22, 0xb96c3b, 3);
    make_block(sprite, 27, 17, 5, 16, 0xe09a58, 2);
}

void draw_soldering_iron(lv_obj_t *sprite)
{
    make_block(sprite, 4, 11, 22, 13, 0x9f55b5, 5);
    make_block(sprite, 8, 14, 14, 3, 0xe0afea, 2);
    make_block(sprite, 25, 14, 21, 7, 0xbac2c9, 2);
    make_block(sprite, 45, 16, 10, 3, 0xffb45e, 1);
    make_block(sprite, 1, 25, 14, 2, 0x716779, 1);
    make_block(sprite, 1, 27, 3, 4, 0x716779, 1);
}

void draw_printer(lv_obj_t *sprite)
{
    make_block(sprite, 5, 4, 5, 37, 0xff914d, 1);
    make_block(sprite, 44, 4, 5, 37, 0xff914d, 1);
    make_block(sprite, 5, 4, 44, 5, 0xffb067, 1);
    make_block(sprite, 7, 38, 40, 5, 0x697078, 1);
    make_block(sprite, 12, 31, 31, 4, 0x58c6c0, 1);
    make_block(sprite, 24, 9, 7, 12, 0xd9dee3, 1);
    make_block(sprite, 26, 20, 3, 8, 0xffd166, 1);
    make_block(sprite, 20, 28, 15, 3, 0x9266cc, 1);
}

void draw_multimeter(lv_obj_t *sprite)
{
    make_block(sprite, 6, 5, 32, 15, 0x172126, 3);
    make_block(sprite, 10, 9, 24, 7, 0x7bd3b4, 1);
    make_block(sprite, 14, 26, 16, 16, 0x252525, LV_RADIUS_CIRCLE);
    make_block(sprite, 19, 29, 6, 10, 0xffd84d, 2);
    make_block(sprite, 6, 40, 5, 5, 0xf05d5e, LV_RADIUS_CIRCLE);
    make_block(sprite, 33, 40, 5, 5, 0x33363a, LV_RADIUS_CIRCLE);
}

void draw_breadboard(lv_obj_t *sprite)
{
    make_block(sprite, 4, 4, 50, 28, 0xf1f1e8, 4);
    make_block(sprite, 7, 7, 44, 2, 0xe85959, 0);
    make_block(sprite, 7, 27, 44, 2, 0x4b79d8, 0);
    for (int column = 0; column < 6; ++column) {
        make_block(sprite, 9 + column * 7, 13, 3, 3, 0x73736e, LV_RADIUS_CIRCLE);
        make_block(sprite, 9 + column * 7, 21, 3, 3, 0x73736e, LV_RADIUS_CIRCLE);
    }
}

void decorate_sprite(lv_obj_t *sprite, HardwareKind kind)
{
    switch (kind) {
        case HardwareKind::Arduino:       draw_arduino(sprite); break;
        case HardwareKind::Box:           draw_box(sprite); break;
        case HardwareKind::Hammer:        draw_hammer(sprite); break;
        case HardwareKind::SolderingIron: draw_soldering_iron(sprite); break;
        case HardwareKind::Printer:       draw_printer(sprite); break;
        case HardwareKind::Multimeter:    draw_multimeter(sprite); break;
        case HardwareKind::Breadboard:    draw_breadboard(sprite); break;
        case HardwareKind::Count:         break;
    }
}

extern "C" const lv_image_dsc_t icon_hardwarephysics;

class HardwarePhysicsApp final : public GameApp {
public:
    const char *name() const override { return "Hardware Physics"; }
    const char *icon() const override { return "PHY"; }
    const lv_image_dsc_t *icon_image() const override { return &icon_hardwarephysics; }
    const char *slug() const override { return "hardwarephysics"; }
    AppAccess access() const override { return AppAccess::Public; }
    bool wants_wake_lock() const override { return true; }
    uint32_t tick_period_ms() const override { return 10; }

    void on_enter(lv_obj_t *root) override
    {
        sensor_fast_mode_ =
            hal_accel_set_output_rate(AccelOutputRate::Hz800) == ESP_OK;
        if (!sensor_fast_mode_) {
            hal_accel_set_output_rate(AccelOutputRate::Hz100);
        }
        root_ = root;
        item_count_ = 0;
        gravity_x_ = 0.0f;
        gravity_y_ = 0.0f;
        have_previous_sample_ = false;
        last_shake_ms_ = 0;
        message_ticks_ = 0;
        rng_state_ = esp_random() ^ lv_tick_get() ^ 0x48575048u;
        if (rng_state_ == 0) rng_state_ = 0x91e10da5u;

        lv_obj_set_style_bg_color(root_, lv_color_hex(BadgeTheme::BACKGROUND), 0);
        lv_obj_set_style_bg_opa(root_, LV_OPA_COVER, 0);

        lv_obj_t *arena = lv_obj_create(root_);
        lv_obj_remove_style_all(arena);
        lv_obj_set_pos(arena, kArenaLeft, kArenaTop);
        lv_obj_set_size(arena, kArenaRight - kArenaLeft,
                        kArenaBottom - kArenaTop);
        lv_obj_set_style_bg_color(arena, lv_color_hex(0x0b0f12), 0);
        lv_obj_set_style_bg_opa(arena, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(arena, lv_color_hex(0x343b41), 0);
        lv_obj_set_style_border_width(arena, 2, 0);
        lv_obj_set_style_radius(arena, 8, 0);
        make_block(arena, 5, 5, 5, 5, 0x596168, LV_RADIUS_CIRCLE);
        make_block(arena, 297, 5, 5, 5, 0x596168, LV_RADIUS_CIRCLE);
        make_block(arena, 5, 168, 5, 5, 0x596168, LV_RADIUS_CIRCLE);
        make_block(arena, 297, 168, 5, 5, 0x596168, LV_RADIUS_CIRCLE);

        lv_obj_t *title = lv_label_create(root_);
        lv_label_set_text(title, "HARDWARE PHYSICS");
        lv_obj_set_style_text_font(title, &lv_font_montserrat_18, 0);
        lv_obj_set_style_text_color(title, lv_color_hex(BadgeTheme::TEXT), 0);
        lv_obj_set_pos(title, 8, 5);

        count_label_ = lv_label_create(root_);
        lv_obj_set_style_text_font(count_label_, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(count_label_, lv_color_hex(0x65d6c4), 0);
        lv_obj_align(count_label_, LV_ALIGN_TOP_RIGHT, -8, 8);

        empty_label_ = lv_label_create(root_);
        lv_label_set_text(empty_label_, "EMPTY BENCH\nPRESS A TO ADD A TOOL");
        lv_obj_set_width(empty_label_, 250);
        lv_obj_set_style_text_align(empty_label_, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_font(empty_label_, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(empty_label_, lv_color_hex(BadgeTheme::TEXT_DIM), 0);
        lv_obj_align(empty_label_, LV_ALIGN_CENTER, 0, 4);

        hint_label_ = lv_label_create(root_);
        lv_obj_set_width(hint_label_, 316);
        lv_obj_set_style_text_align(hint_label_, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_font(hint_label_, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(hint_label_, lv_color_hex(BadgeTheme::TEXT_MUTED), 0);
        lv_obj_align(hint_label_, LV_ALIGN_BOTTOM_MID, 0, -1);

        for (HardwareKind kind : kStarterKinds) add_item(kind);
        update_copy();
    }

    void on_exit() override
    {
        hal_accel_set_output_rate(AccelOutputRate::Hz100);
        root_ = nullptr;
        count_label_ = nullptr;
        hint_label_ = nullptr;
        empty_label_ = nullptr;
        for (PhysicsItem &item : items_) item.sprite = nullptr;
        item_count_ = 0;
    }

    void on_button(ButtonEvent event) override
    {
        if (event.kind != ButtonEventKind::Pressed) return;
        if (event.button == Button::A) {
            if (item_count_ < kMaxItems) {
                add_random_item();
                set_message("NEW TOOL DROPPED IN!");
            } else {
                set_message("WORKBENCH FULL - PRESS B");
            }
        } else if (event.button == Button::B) {
            if (item_count_ > 0) {
                remove_last_item();
                set_message(item_count_ == 0
                    ? "EMPTY - PRESS A TO ADD" : "TOOL REMOVED");
            } else {
                set_message("EMPTY - PRESS A TO ADD");
            }
        }
    }

    void on_tick() override
    {
        AccelSample sample;
        if (hal_accel_read(&sample) == ESP_OK) {
            const float target_x = clamp_float(-sample.x_mg * 0.00007f, -0.38f, 0.38f);
            const float target_y = clamp_float(sample.y_mg * 0.00007f, -0.38f, 0.38f);
            gravity_x_ = gravity_x_ * 0.45f + target_x * 0.55f;
            gravity_y_ = gravity_y_ * 0.45f + target_y * 0.55f;

            // A change in acceleration reacts to the actual direction of a
            // shake. This feels immediate and catches quick reversals that a
            // total-magnitude threshold misses.
            if (have_previous_sample_) {
                const float delta_x = sample.x_mg - previous_sample_.x_mg;
                const float delta_y = sample.y_mg - previous_sample_.y_mg;
                const float delta_z = sample.z_mg - previous_sample_.z_mg;
                const float jerk = sqrtf(delta_x * delta_x + delta_y * delta_y
                                         + delta_z * delta_z);
                const uint32_t now = lv_tick_get();
                if (jerk > 190.0f && now - last_shake_ms_ >= kShakeCooldownMs) {
                    const float impulse_x = clamp_float(-delta_x * 0.0042f,
                                                        -4.8f, 4.8f);
                    const float impulse_y = clamp_float(delta_y * 0.0042f,
                                                        -4.8f, 4.8f);
                    const float scatter = clamp_float((jerk - 190.0f) * 0.0015f,
                                                      0.25f, 2.2f);
                    apply_shake(impulse_x, impulse_y, scatter);
                    last_shake_ms_ = now;
                }
            }
            previous_sample_ = sample;
            have_previous_sample_ = true;
        }

        step_physics();
        render_positions();

        if (message_ticks_ > 0 && --message_ticks_ == 0) update_copy();
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

    float random_unit()
    {
        return static_cast<float>(random_u32() & 0xffffu) / 32767.5f - 1.0f;
    }

    bool overlaps_existing(float x, float y, int width, int height) const
    {
        for (int i = 0; i < item_count_; ++i) {
            const PhysicsItem &other = items_[i];
            if (fabsf(x - other.x) < (width + other.width) * 0.5f + 3.0f
                && fabsf(y - other.y) < (height + other.height) * 0.5f + 3.0f) {
                return true;
            }
        }
        return false;
    }

    lv_obj_t *create_sprite(HardwareKind kind, const ItemSpec &spec)
    {
        lv_obj_t *sprite = lv_obj_create(root_);
        lv_obj_remove_style_all(sprite);
        lv_obj_set_size(sprite, spec.width, spec.height);
        lv_obj_set_style_bg_color(sprite, lv_color_hex(spec.fill), 0);
        lv_obj_set_style_bg_opa(sprite, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(sprite, lv_color_hex(spec.border), 0);
        lv_obj_set_style_border_width(sprite, 2, 0);
        lv_obj_set_style_radius(sprite, 6, 0);
        lv_obj_clear_flag(sprite, LV_OBJ_FLAG_SCROLLABLE);
        decorate_sprite(sprite, kind);
        return sprite;
    }

    void add_random_item()
    {
        if (!root_ || item_count_ >= kMaxItems) return;
        add_item(static_cast<HardwareKind>(
            random_u32() % static_cast<uint32_t>(HardwareKind::Count)));
    }

    void add_item(HardwareKind kind)
    {
        if (!root_ || item_count_ >= kMaxItems) return;

        PhysicsItem &item = items_[item_count_];
        item.kind = kind;
        const ItemSpec &spec = kItemSpecs[static_cast<size_t>(item.kind)];
        item.width = spec.width;
        item.height = spec.height;

        const float min_x = kArenaLeft + item.width * 0.5f + 3.0f;
        const float max_x = kArenaRight - item.width * 0.5f - 3.0f;
        const float min_y = kArenaTop + item.height * 0.5f + 3.0f;
        const float max_y = kArenaBottom - item.height * 0.5f - 3.0f;
        for (int attempt = 0; attempt < 24; ++attempt) {
            const float x_fraction = static_cast<float>(random_u32() & 0xffffu) / 65535.0f;
            const float y_fraction = static_cast<float>(random_u32() & 0xffffu) / 65535.0f;
            item.x = min_x + (max_x - min_x) * x_fraction;
            item.y = min_y + (max_y - min_y) * y_fraction;
            if (!overlaps_existing(item.x, item.y, item.width, item.height)) break;
        }
        item.vx = random_unit() * 2.4f;
        item.vy = -1.6f + random_unit() * 1.8f;
        item.sprite = create_sprite(item.kind, spec);
        item_count_++;
        render_item(item);
        update_copy();
    }

    void remove_last_item()
    {
        if (item_count_ <= 0) return;
        PhysicsItem &item = items_[item_count_ - 1];
        if (item.sprite) lv_obj_delete(item.sprite);
        item = PhysicsItem{};
        item_count_--;
        update_copy();
    }

    void apply_shake(float impulse_x, float impulse_y, float scatter)
    {
        for (int i = 0; i < item_count_; ++i) {
            items_[i].vx = clamp_float(items_[i].vx + impulse_x
                                       + random_unit() * scatter,
                                       -kMaxSpeed, kMaxSpeed);
            items_[i].vy = clamp_float(items_[i].vy + impulse_y
                                       + random_unit() * scatter,
                                       -kMaxSpeed, kMaxSpeed);
        }
    }

    void constrain_to_arena(PhysicsItem &item)
    {
        const float min_x = kArenaLeft + item.width * 0.5f + 2.0f;
        const float max_x = kArenaRight - item.width * 0.5f - 2.0f;
        const float min_y = kArenaTop + item.height * 0.5f + 2.0f;
        const float max_y = kArenaBottom - item.height * 0.5f - 2.0f;

        if (item.x < min_x) {
            item.x = min_x;
            if (item.vx < 0.0f) item.vx = -item.vx * kRestitution;
            item.vy *= 0.985f;
        } else if (item.x > max_x) {
            item.x = max_x;
            if (item.vx > 0.0f) item.vx = -item.vx * kRestitution;
            item.vy *= 0.985f;
        }
        if (item.y < min_y) {
            item.y = min_y;
            if (item.vy < 0.0f) item.vy = -item.vy * kRestitution;
            item.vx *= 0.985f;
        } else if (item.y > max_y) {
            item.y = max_y;
            if (item.vy > 0.0f) item.vy = -item.vy * kRestitution;
            item.vx *= 0.985f;
        }
    }

    void resolve_pair(PhysicsItem &first, PhysicsItem &second)
    {
        const float dx = second.x - first.x;
        const float dy = second.y - first.y;
        const float overlap_x = (first.width + second.width) * 0.5f - fabsf(dx);
        const float overlap_y = (first.height + second.height) * 0.5f - fabsf(dy);
        if (overlap_x <= 0.0f || overlap_y <= 0.0f) return;

        if (overlap_x < overlap_y) {
            const float normal = dx < 0.0f ? -1.0f : 1.0f;
            const float correction = (overlap_x + 0.2f) * 0.5f;
            first.x -= normal * correction;
            second.x += normal * correction;
            const float relative = (second.vx - first.vx) * normal;
            if (relative < 0.0f) {
                const float impulse = -(1.0f + kRestitution) * relative * 0.5f;
                first.vx -= impulse * normal;
                second.vx += impulse * normal;
            }
            first.vy *= 0.985f;
            second.vy *= 0.985f;
        } else {
            const float normal = dy < 0.0f ? -1.0f : 1.0f;
            const float correction = (overlap_y + 0.2f) * 0.5f;
            first.y -= normal * correction;
            second.y += normal * correction;
            const float relative = (second.vy - first.vy) * normal;
            if (relative < 0.0f) {
                const float impulse = -(1.0f + kRestitution) * relative * 0.5f;
                first.vy -= impulse * normal;
                second.vy += impulse * normal;
            }
            first.vx *= 0.985f;
            second.vx *= 0.985f;
        }
    }

    void step_physics()
    {
        for (int i = 0; i < item_count_; ++i) {
            PhysicsItem &item = items_[i];
            item.vx = clamp_float((item.vx + gravity_x_) * 0.997f,
                                  -kMaxSpeed, kMaxSpeed);
            item.vy = clamp_float((item.vy + gravity_y_) * 0.997f,
                                  -kMaxSpeed, kMaxSpeed);
        }

        for (int substep = 0; substep < kPhysicsSubsteps; ++substep) {
            for (int i = 0; i < item_count_; ++i) {
                PhysicsItem &item = items_[i];
                item.x += item.vx / kPhysicsSubsteps;
                item.y += item.vy / kPhysicsSubsteps;
                constrain_to_arena(item);
            }
            for (int i = 0; i < item_count_; ++i) {
                for (int j = i + 1; j < item_count_; ++j) {
                    resolve_pair(items_[i], items_[j]);
                }
            }
            for (int i = 0; i < item_count_; ++i) constrain_to_arena(items_[i]);
        }
    }

    void render_item(PhysicsItem &item)
    {
        if (!item.sprite) return;
        lv_obj_set_pos(item.sprite,
                       static_cast<int>(lroundf(item.x - item.width * 0.5f)),
                       static_cast<int>(lroundf(item.y - item.height * 0.5f)));
    }

    void render_positions()
    {
        for (int i = 0; i < item_count_; ++i) render_item(items_[i]);
    }

    void update_copy()
    {
        if (count_label_) lv_label_set_text_fmt(count_label_, "%d/%d", item_count_, kMaxItems);
        if (empty_label_) {
            if (item_count_ == 0) lv_obj_remove_flag(empty_label_, LV_OBJ_FLAG_HIDDEN);
            else                  lv_obj_add_flag(empty_label_, LV_OBJ_FLAG_HIDDEN);
        }
        if (hint_label_ && message_ticks_ == 0) {
            lv_label_set_text(hint_label_, "TILT + SHAKE    A: ADD    B: REMOVE");
            lv_obj_set_style_text_color(hint_label_, lv_color_hex(BadgeTheme::TEXT_MUTED), 0);
        }
    }

    void set_message(const char *message)
    {
        message_ticks_ = 55;
        if (hint_label_) {
            lv_label_set_text(hint_label_, message);
            lv_obj_set_style_text_color(hint_label_, lv_color_hex(0xffd166), 0);
        }
    }

    lv_obj_t *root_ = nullptr;
    lv_obj_t *count_label_ = nullptr;
    lv_obj_t *hint_label_ = nullptr;
    lv_obj_t *empty_label_ = nullptr;
    PhysicsItem items_[kMaxItems] = {};
    int item_count_ = 0;
    int message_ticks_ = 0;
    float gravity_x_ = 0.0f;
    float gravity_y_ = 0.0f;
    AccelSample previous_sample_ = {};
    bool have_previous_sample_ = false;
    bool sensor_fast_mode_ = false;
    uint32_t last_shake_ms_ = 0;
    uint32_t rng_state_ = 0;
};

HardwarePhysicsApp s_instance;

} // namespace

App *app_hardware_physics_instance() { return &s_instance; }
