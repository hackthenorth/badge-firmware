#include "ui_error_card.h"

#include "ui_theme.h"

#include "lvgl.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

constexpr std::size_t kCallbackBytes = 64;
constexpr std::size_t kMessageBytes = 801;
constexpr std::size_t kRepeatBytes = 40;
constexpr std::size_t kHintBytes = 80;

lv_obj_t *s_card = nullptr;
lv_obj_t *s_callback_label = nullptr;
lv_obj_t *s_message_label = nullptr;
lv_obj_t *s_repeat_label = nullptr;
lv_obj_t *s_hint_label = nullptr;

char s_title[] = "LUA ERROR";
char s_callback[kCallbackBytes] = {};
char s_message[kMessageBytes] = {};
char s_repeat[kRepeatBytes] = {};
char s_hint[kHintBytes] = {};

void copy_text(char *dst, std::size_t dst_size, const char *src)
{
    if (!src) src = "";
    std::size_t len = std::strlen(src);
    if (len >= dst_size) len = dst_size - 1;
    std::memcpy(dst, src, len);
    dst[len] = '\0';
}

void style_label(lv_obj_t *label, const lv_font_t *font, std::uint32_t color)
{
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
}

} // namespace

void ui_error_card_prepare()
{
    if (s_card) return;

    s_card = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_card);
    lv_obj_set_size(s_card, 308, 228);
    lv_obj_center(s_card);
    lv_obj_set_style_bg_color(s_card, lv_color_hex(BadgeTheme::PANEL), 0);
    lv_obj_set_style_bg_opa(s_card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_card, lv_color_hex(BadgeTheme::ACCENT_DETAIL), 0);
    lv_obj_set_style_border_width(s_card, 2, 0);
    lv_obj_set_style_radius(s_card, 8, 0);
    lv_obj_clear_flag(s_card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(s_card);
    lv_label_set_text_static(title, s_title);
    style_label(title, &lv_font_montserrat_20, BadgeTheme::TEXT);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 12, 10);

    s_callback_label = lv_label_create(s_card);
    lv_label_set_text_static(s_callback_label, s_callback);
    style_label(s_callback_label, &lv_font_montserrat_16, BadgeTheme::ACCENT_DETAIL);
    lv_label_set_long_mode(s_callback_label, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(s_callback_label, 280);
    lv_obj_align(s_callback_label, LV_ALIGN_TOP_LEFT, 12, 38);

    s_message_label = lv_label_create(s_card);
    lv_label_set_text_static(s_message_label, s_message);
    style_label(s_message_label, &lv_font_montserrat_14, BadgeTheme::TEXT_SOFT);
    lv_label_set_long_mode(s_message_label, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_size(s_message_label, 280, 119);
    lv_obj_align(s_message_label, LV_ALIGN_TOP_LEFT, 12, 64);

    s_repeat_label = lv_label_create(s_card);
    lv_label_set_text_static(s_repeat_label, s_repeat);
    style_label(s_repeat_label, &lv_font_montserrat_14, BadgeTheme::TEXT_MUTED);
    lv_obj_align(s_repeat_label, LV_ALIGN_BOTTOM_LEFT, 12, -25);

    s_hint_label = lv_label_create(s_card);
    lv_label_set_text_static(s_hint_label, s_hint);
    style_label(s_hint_label, &lv_font_montserrat_14, BadgeTheme::TEXT);
    lv_obj_set_width(s_hint_label, 280);
    lv_obj_set_style_text_align(s_hint_label, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(s_hint_label, LV_ALIGN_BOTTOM_RIGHT, -12, -7);

    lv_obj_add_flag(s_card, LV_OBJ_FLAG_HIDDEN);
}

void ui_error_card_show(const char *callback,
                        const char *message,
                        std::uint32_t repeat_count,
                        const char *hint)
{
    if (!s_card) return;

    copy_text(s_callback, sizeof(s_callback), callback);
    copy_text(s_message, sizeof(s_message), message);
    copy_text(s_hint, sizeof(s_hint), hint);
    std::snprintf(s_repeat, sizeof(s_repeat), "Occurrences: %lu",
                  (unsigned long)repeat_count);

    // Re-setting the same static pointers refreshes label layout without
    // asking LVGL to allocate copies of the updated strings.
    lv_label_set_text_static(s_callback_label, s_callback);
    lv_label_set_text_static(s_message_label, s_message);
    lv_label_set_text_static(s_repeat_label, s_repeat);
    lv_label_set_text_static(s_hint_label, s_hint);

    lv_obj_remove_flag(s_card, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_to_index(s_card, -1);
}

void ui_error_card_hide()
{
    if (s_card) lv_obj_add_flag(s_card, LV_OBJ_FLAG_HIDDEN);
}
