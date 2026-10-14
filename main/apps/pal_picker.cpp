#include "pal_picker.h"
#include "pal_catalog.h"
#include "solana_wallet.h"

extern "C" {
extern const lv_image_dsc_t pal_ginny_locked_in, pal_ginny_sparkle;
extern const lv_image_dsc_t pal_patch_snooze, pal_patch_sparkle;
extern const lv_image_dsc_t pal_vinyl_curious, pal_ginny_heart;
extern const lv_image_dsc_t pal_patch_curious, pal_vinyl_snooze, pal_vinyl_sparkle;
extern const lv_image_dsc_t pal_thumb_ginny_locked_in, pal_thumb_ginny_sparkle;
extern const lv_image_dsc_t pal_thumb_patch_snooze, pal_thumb_patch_sparkle;
extern const lv_image_dsc_t pal_thumb_vinyl_curious, pal_thumb_ginny_heart;
extern const lv_image_dsc_t pal_thumb_patch_curious, pal_thumb_vinyl_snooze, pal_thumb_vinyl_sparkle;
}

namespace {
const lv_image_dsc_t *const images[PAL_COUNT] = {
    nullptr, &pal_ginny_locked_in, &pal_ginny_sparkle, &pal_patch_snooze,
    &pal_patch_sparkle, &pal_vinyl_curious, &pal_ginny_heart,
    &pal_patch_curious, &pal_vinyl_snooze, &pal_vinyl_sparkle,
};
const lv_image_dsc_t *const thumbnails[PAL_COUNT] = {
    nullptr, &pal_thumb_ginny_locked_in, &pal_thumb_ginny_sparkle,
    &pal_thumb_patch_snooze, &pal_thumb_patch_sparkle, &pal_thumb_vinyl_curious,
    &pal_thumb_ginny_heart, &pal_thumb_patch_curious, &pal_thumb_vinyl_snooze,
    &pal_thumb_vinyl_sparkle,
};
constexpr uint8_t order[] = {1, 2, 6, 3, 4, 7, 5, 8, 9, PAL_NONE};

lv_obj_t *label(lv_obj_t *parent, const char *text, int y,
                const lv_font_t *font = &lv_font_montserrat_12)
{
    lv_obj_t *obj = lv_label_create(parent);
    lv_label_set_text(obj, text);
    lv_obj_set_style_text_font(obj, font, 0);
    lv_obj_set_style_text_color(obj, lv_color_hex(BadgeTheme::TEXT), 0);
    lv_obj_align(obj, LV_ALIGN_TOP_MID, 0, y);
    return obj;
}
}

const lv_image_dsc_t *pal_image(uint8_t id)
{
    return id < PAL_COUNT ? images[id] : nullptr;
}

int pal_unlocked_count()
{
    int count = 0;
    for (uint8_t id = 1; id < PAL_COUNT; ++id)
        if (solana_wallet_pal_unlocked(id)) ++count;
    return count;
}

void PalPicker::show(lv_obj_t *root)
{
    root_ = root;
    error_ = nullptr;
    focus_ = 9;
    for (int i = 0; i < 10; ++i)
        if (order[i] == solana_wallet_equipped_pal()) focus_ = i;
    column_ = focus_ < 9 ? focus_ % 3 : 0;
    render();
}

bool PalPicker::on_button(ButtonEvent ev)
{
    if (ev.kind != ButtonEventKind::Pressed) return false;
    error_ = nullptr;
    switch (ev.button) {
        case Button::B: return true;
        case Button::A:
            if (!solana_wallet_pal_unlocked(order[focus_])) {
                error_ = "Find a Solana box to unlock this pal";
            } else if (solana_wallet_set_equipped_pal(order[focus_])) {
                return true;
            } else {
                error_ = "Couldn't save. Press A to retry.";
            }
            break;
        case Button::Left:
            if (focus_ < 9) focus_ = (focus_ / 3) * 3 + (focus_ + 2) % 3;
            break;
        case Button::Right:
            if (focus_ < 9) focus_ = (focus_ / 3) * 3 + (focus_ + 1) % 3;
            break;
        case Button::Up: focus_ = focus_ == 9 ? 6 + column_ : focus_ < 3 ? 9 : focus_ - 3; break;
        case Button::Down: focus_ = focus_ == 9 ? column_ : focus_ >= 6 ? 9 : focus_ + 3; break;
        default: return false;
    }
    if (focus_ < 9) column_ = focus_ % 3;
    render();
    return false;
}

void PalPicker::render()
{
    lv_obj_clean(root_);
    label(root_, "Customize QR", 5, &lv_font_montserrat_18);
    lv_obj_t *count = label(root_, "", 26);
    lv_label_set_text_fmt(count, "%d/9 pals unlocked", pal_unlocked_count());
    for (int i = 0; i < 10; ++i) {
        const uint8_t id = order[i];
        const bool unlocked = solana_wallet_pal_unlocked(id);
        lv_obj_t *cell = lv_obj_create(root_);
        lv_obj_remove_style_all(cell);
        lv_obj_clear_flag(cell, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(cell, i == 9 ? 292 : 94, i == 9 ? 22 : 43);
        lv_obj_set_pos(cell, i == 9 ? 14 : 14 + (i % 3) * 99,
                       i == 9 ? 195 : 43 + (i / 3) * 45);
        lv_obj_set_style_radius(cell, 6, 0);
        lv_obj_set_style_bg_color(cell, lv_color_hex(BadgeTheme::PANEL), 0);
        lv_obj_set_style_bg_opa(cell, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(cell, focus_ == i ? 2 : 1, 0);
        lv_obj_set_style_border_color(cell, lv_color_hex(focus_ == i
            ? BadgeTheme::TEXT : BadgeTheme::BORDER), 0);
        if (id == PAL_NONE) {
            label(cell, "None", 3);
        } else {
            lv_obj_t *img = lv_image_create(cell);
            lv_image_set_src(img, thumbnails[id]);
            lv_obj_center(img);
            if (!unlocked) {
                lv_obj_set_style_image_recolor(img, lv_color_hex(0x69717d), 0);
                lv_obj_set_style_image_recolor_opa(img, LV_OPA_COVER, 0);
                lv_obj_set_style_image_opa(img, LV_OPA_60, 0);
            }
        }
        if (id == solana_wallet_equipped_pal()) {
            lv_obj_t *mark = label(cell, LV_SYMBOL_OK, 2);
            lv_obj_align(mark, LV_ALIGN_TOP_RIGHT, -5, 2);
            lv_obj_set_style_text_color(mark, lv_color_hex(BadgeTheme::ACCENT_DETAIL), 0);
        }
    }
    const PalDefinition *pal = pal_catalog_get(order[focus_]);
    lv_obj_t *name = label(root_, error_ ? error_ : pal ? pal->name : "QR code only", 180);
    if (!error_ && pal && !solana_wallet_pal_unlocked(pal->id))
        lv_label_set_text_fmt(name, "%s (locked)", pal->name);
    label(root_, "D-pad: choose   A: equip   B: back", 223);
}
