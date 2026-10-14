#include "lua_internal.h"

extern "C" {
#include "lauxlib.h"
}

#include "ui_theme.h"

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace {

constexpr lv_style_selector_t kDefaultSelector =
    static_cast<lv_style_selector_t>(LV_PART_MAIN);

using StyleThunk = void (*)(lua_State *L, lv_obj_t *obj, int value_index,
                            lv_style_selector_t selector);

struct StyleProperty {
    const char *name;
    enum ValueKind : std::uint8_t {
        COLOR,
        OPACITY,
        DIMENSION,
        OFFSET,
        FLEX_FLOW,
        TEXT_ALIGN,
        TEXT_FONT,
    } value_kind;
    StyleThunk apply;
};

bool equals(const char *value, std::size_t value_length, const char *literal)
{
    const std::size_t literal_length = std::strlen(literal);
    return value_length == literal_length
        && std::memcmp(value, literal, value_length) == 0;
}

std::uint32_t check_color(lua_State *L, int index, const char *property)
{
    const lua_Integer value = luaL_checkinteger(L, index);
    if (value < 0 || value > 0xffffff) {
        luaL_error(L, "style property '%s' must be a 24-bit RGB integer", property);
        return 0;
    }
    return static_cast<std::uint32_t>(value);
}

lv_opa_t check_opacity(lua_State *L, int index, const char *property)
{
    const lua_Integer value = luaL_checkinteger(L, index);
    if (value < 0 || value > 255) {
        luaL_error(L, "style property '%s' must be an integer between 0 and 255", property);
        return LV_OPA_TRANSP;
    }
    return static_cast<lv_opa_t>(value);
}

int32_t check_dimension(lua_State *L, int index, const char *property)
{
    const lua_Integer value = luaL_checkinteger(L, index);
    if (value < 0 || value > LV_COORD_MAX) {
        luaL_error(L, "style property '%s' must be between 0 and %d", property,
                   LV_COORD_MAX);
        return 0;
    }
    return static_cast<int32_t>(value);
}

int32_t check_offset(lua_State *L, int index, const char *property)
{
    const lua_Integer value = luaL_checkinteger(L, index);
    if (value < LV_COORD_MIN || value > LV_COORD_MAX) {
        luaL_error(L, "style property '%s' is outside LVGL coordinate range", property);
        return 0;
    }
    return static_cast<int32_t>(value);
}

const char *check_string(lua_State *L, int index, const char *property,
                         std::size_t *length)
{
    if (lua_type(L, index) != LUA_TSTRING) {
        luaL_error(L, "style property '%s' must be a string", property);
        return nullptr;
    }
    return lua_tolstring(L, index, length);
}

const lv_font_t *check_font(lua_State *L, int index)
{
    if (lua_type(L, index) == LUA_TNUMBER) {
        switch (luaL_checkinteger(L, index)) {
        case 14: return &lv_font_montserrat_14;
        case 16: return &lv_font_montserrat_16;
        case 18: return &lv_font_montserrat_18;
        case 20: return &lv_font_montserrat_20;
        case 22: return &lv_font_montserrat_22;
        case 24: return &lv_font_montserrat_24;
        default:
            luaL_error(L, "style property 'text_font' must be 14, 16, 18, 20, 22, or 24");
            return nullptr;
        }
    }

    std::size_t length = 0;
    const char *name = check_string(L, index, "text_font", &length);
    if (equals(name, length, "small")) return &lv_font_montserrat_14;
    if (equals(name, length, "default")) return &lv_font_montserrat_16;
    if (equals(name, length, "large")) return &lv_font_montserrat_20;

    luaL_error(L, "style property 'text_font' must be a supported size or small, default, or large");
    return nullptr;
}

lv_text_align_t check_text_align(lua_State *L, int index)
{
    std::size_t length = 0;
    const char *name = check_string(L, index, "text_align", &length);
    if (equals(name, length, "left")) return LV_TEXT_ALIGN_LEFT;
    if (equals(name, length, "center")) return LV_TEXT_ALIGN_CENTER;
    if (equals(name, length, "right")) return LV_TEXT_ALIGN_RIGHT;

    luaL_error(L, "style property 'text_align' must be left, center, or right");
    return LV_TEXT_ALIGN_LEFT;
}

lv_flex_flow_t check_flex_flow(lua_State *L, int index)
{
    std::size_t length = 0;
    const char *name = check_string(L, index, "flex_flow", &length);
    if (equals(name, length, "row")) return LV_FLEX_FLOW_ROW;
    if (equals(name, length, "column")) return LV_FLEX_FLOW_COLUMN;
    if (equals(name, length, "row_wrap")) return LV_FLEX_FLOW_ROW_WRAP;
    if (equals(name, length, "column_wrap")) return LV_FLEX_FLOW_COLUMN_WRAP;

    luaL_error(L, "style property 'flex_flow' must be row, column, row_wrap, or column_wrap");
    return LV_FLEX_FLOW_ROW;
}

void style_arc_color(lua_State *L, lv_obj_t *obj, int index, lv_style_selector_t selector)
{
    lv_obj_set_style_arc_color(obj, lv_color_hex(check_color(L, index, "arc_color")), selector);
}

void style_arc_opa(lua_State *L, lv_obj_t *obj, int index, lv_style_selector_t selector)
{
    lv_obj_set_style_arc_opa(obj, check_opacity(L, index, "arc_opa"), selector);
}

void style_arc_width(lua_State *L, lv_obj_t *obj, int index, lv_style_selector_t selector)
{
    lv_obj_set_style_arc_width(obj, check_dimension(L, index, "arc_width"), selector);
}

void style_bg_color(lua_State *L, lv_obj_t *obj, int index, lv_style_selector_t selector)
{
    // Imply opaque, mirroring badge_lua_style_apply_color: style-stripped
    // boxes default to a transparent background, so a bare bg_color would
    // otherwise render nothing. An explicit bg_opa still wins (applied
    // separately whenever present).
    lv_obj_set_style_bg_color(obj, lv_color_hex(check_color(L, index, "bg_color")), selector);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, selector);
}

void style_bg_opa(lua_State *L, lv_obj_t *obj, int index, lv_style_selector_t selector)
{
    lv_obj_set_style_bg_opa(obj, check_opacity(L, index, "bg_opa"), selector);
}

void style_border_color(lua_State *L, lv_obj_t *obj, int index, lv_style_selector_t selector)
{
    lv_obj_set_style_border_color(obj, lv_color_hex(check_color(L, index, "border_color")), selector);
}

void style_border_opa(lua_State *L, lv_obj_t *obj, int index, lv_style_selector_t selector)
{
    lv_obj_set_style_border_opa(obj, check_opacity(L, index, "border_opa"), selector);
}

void style_border_width(lua_State *L, lv_obj_t *obj, int index, lv_style_selector_t selector)
{
    lv_obj_set_style_border_width(obj, check_dimension(L, index, "border_width"), selector);
}

void style_color(lua_State *L, lv_obj_t *obj, int index, lv_style_selector_t selector)
{
    badge_lua_style_apply_color(obj, check_color(L, index, "color"), selector);
}

void style_flex_flow(lua_State *L, lv_obj_t *obj, int index, lv_style_selector_t selector)
{
    lv_obj_set_style_flex_flow(obj, check_flex_flow(L, index), selector);
}

void style_line_color(lua_State *L, lv_obj_t *obj, int index, lv_style_selector_t selector)
{
    lv_obj_set_style_line_color(obj, lv_color_hex(check_color(L, index, "line_color")), selector);
}

void style_line_opa(lua_State *L, lv_obj_t *obj, int index, lv_style_selector_t selector)
{
    lv_obj_set_style_line_opa(obj, check_opacity(L, index, "line_opa"), selector);
}

void style_line_width(lua_State *L, lv_obj_t *obj, int index, lv_style_selector_t selector)
{
    lv_obj_set_style_line_width(obj, check_dimension(L, index, "line_width"), selector);
}

void style_opa(lua_State *L, lv_obj_t *obj, int index, lv_style_selector_t selector)
{
    lv_obj_set_style_opa(obj, check_opacity(L, index, "opa"), selector);
}

void style_pad_all(lua_State *L, lv_obj_t *obj, int index, lv_style_selector_t selector)
{
    lv_obj_set_style_pad_all(obj, check_dimension(L, index, "pad_all"), selector);
}

void style_pad_bottom(lua_State *L, lv_obj_t *obj, int index, lv_style_selector_t selector)
{
    lv_obj_set_style_pad_bottom(obj, check_dimension(L, index, "pad_bottom"), selector);
}

void style_pad_column(lua_State *L, lv_obj_t *obj, int index, lv_style_selector_t selector)
{
    lv_obj_set_style_pad_column(obj, check_dimension(L, index, "pad_column"), selector);
}

void style_pad_left(lua_State *L, lv_obj_t *obj, int index, lv_style_selector_t selector)
{
    lv_obj_set_style_pad_left(obj, check_dimension(L, index, "pad_left"), selector);
}

void style_pad_right(lua_State *L, lv_obj_t *obj, int index, lv_style_selector_t selector)
{
    lv_obj_set_style_pad_right(obj, check_dimension(L, index, "pad_right"), selector);
}

void style_pad_row(lua_State *L, lv_obj_t *obj, int index, lv_style_selector_t selector)
{
    lv_obj_set_style_pad_row(obj, check_dimension(L, index, "pad_row"), selector);
}

void style_pad_top(lua_State *L, lv_obj_t *obj, int index, lv_style_selector_t selector)
{
    lv_obj_set_style_pad_top(obj, check_dimension(L, index, "pad_top"), selector);
}

void style_radius(lua_State *L, lv_obj_t *obj, int index, lv_style_selector_t selector)
{
    lv_obj_set_style_radius(obj, check_dimension(L, index, "radius"), selector);
}

void style_shadow_color(lua_State *L, lv_obj_t *obj, int index, lv_style_selector_t selector)
{
    lv_obj_set_style_shadow_color(obj, lv_color_hex(check_color(L, index, "shadow_color")), selector);
}

void style_shadow_offset_x(lua_State *L, lv_obj_t *obj, int index, lv_style_selector_t selector)
{
    lv_obj_set_style_shadow_offset_x(obj, check_offset(L, index, "shadow_offset_x"), selector);
}

void style_shadow_offset_y(lua_State *L, lv_obj_t *obj, int index, lv_style_selector_t selector)
{
    lv_obj_set_style_shadow_offset_y(obj, check_offset(L, index, "shadow_offset_y"), selector);
}

void style_shadow_opa(lua_State *L, lv_obj_t *obj, int index, lv_style_selector_t selector)
{
    lv_obj_set_style_shadow_opa(obj, check_opacity(L, index, "shadow_opa"), selector);
}

void style_shadow_spread(lua_State *L, lv_obj_t *obj, int index, lv_style_selector_t selector)
{
    lv_obj_set_style_shadow_spread(obj, check_offset(L, index, "shadow_spread"), selector);
}

void style_shadow_width(lua_State *L, lv_obj_t *obj, int index, lv_style_selector_t selector)
{
    lv_obj_set_style_shadow_width(obj, check_dimension(L, index, "shadow_width"), selector);
}

void style_text_align(lua_State *L, lv_obj_t *obj, int index, lv_style_selector_t selector)
{
    lv_obj_set_style_text_align(obj, check_text_align(L, index), selector);
}

void style_text_color(lua_State *L, lv_obj_t *obj, int index, lv_style_selector_t selector)
{
    lv_obj_set_style_text_color(obj, lv_color_hex(check_color(L, index, "text_color")), selector);
}

void style_text_font(lua_State *L, lv_obj_t *obj, int index, lv_style_selector_t selector)
{
    lv_obj_set_style_text_font(obj, check_font(L, index), selector);
}

void style_text_opa(lua_State *L, lv_obj_t *obj, int index, lv_style_selector_t selector)
{
    lv_obj_set_style_text_opa(obj, check_opacity(L, index, "text_opa"), selector);
}

// Keep this table alphabetized: lookup is a bounded binary search.
static const StyleProperty kStyleProperties[] = {
    { "arc_color",       StyleProperty::COLOR,     style_arc_color       },
    { "arc_opa",         StyleProperty::OPACITY,   style_arc_opa         },
    { "arc_width",       StyleProperty::DIMENSION, style_arc_width       },
    { "bg_color",        StyleProperty::COLOR,     style_bg_color        },
    { "bg_opa",          StyleProperty::OPACITY,   style_bg_opa          },
    { "border_color",    StyleProperty::COLOR,     style_border_color    },
    { "border_opa",      StyleProperty::OPACITY,   style_border_opa      },
    { "border_width",    StyleProperty::DIMENSION, style_border_width    },
    { "color",           StyleProperty::COLOR,     style_color           },
    { "flex_flow",       StyleProperty::FLEX_FLOW, style_flex_flow       },
    { "line_color",      StyleProperty::COLOR,     style_line_color      },
    { "line_opa",        StyleProperty::OPACITY,   style_line_opa        },
    { "line_width",      StyleProperty::DIMENSION, style_line_width      },
    { "opa",             StyleProperty::OPACITY,   style_opa             },
    { "pad_all",         StyleProperty::DIMENSION, style_pad_all         },
    { "pad_bottom",      StyleProperty::DIMENSION, style_pad_bottom      },
    { "pad_column",      StyleProperty::DIMENSION, style_pad_column      },
    { "pad_left",        StyleProperty::DIMENSION, style_pad_left        },
    { "pad_right",       StyleProperty::DIMENSION, style_pad_right       },
    { "pad_row",         StyleProperty::DIMENSION, style_pad_row         },
    { "pad_top",         StyleProperty::DIMENSION, style_pad_top         },
    { "radius",          StyleProperty::DIMENSION, style_radius          },
    { "shadow_color",    StyleProperty::COLOR,     style_shadow_color    },
    { "shadow_offset_x", StyleProperty::OFFSET,    style_shadow_offset_x },
    { "shadow_offset_y", StyleProperty::OFFSET,    style_shadow_offset_y },
    { "shadow_opa",      StyleProperty::OPACITY,   style_shadow_opa      },
    { "shadow_spread",   StyleProperty::OFFSET,    style_shadow_spread   },
    { "shadow_width",    StyleProperty::DIMENSION, style_shadow_width    },
    { "text_align",      StyleProperty::TEXT_ALIGN, style_text_align     },
    { "text_color",      StyleProperty::COLOR,     style_text_color      },
    { "text_font",       StyleProperty::TEXT_FONT, style_text_font       },
    { "text_opa",        StyleProperty::OPACITY,   style_text_opa        },
};

constexpr std::size_t kStylePropertyCount =
    sizeof(kStyleProperties) / sizeof(kStyleProperties[0]);
constexpr std::size_t kMaxStylePropertyNameLength = sizeof("shadow_offset_x") - 1;

int compare_name(const char *name, std::size_t name_length, const char *candidate)
{
    const std::size_t candidate_length = std::strlen(candidate);
    const std::size_t shared_length = name_length < candidate_length
        ? name_length : candidate_length;
    const int compared = std::memcmp(name, candidate, shared_length);
    if (compared != 0) return compared;
    if (name_length < candidate_length) return -1;
    if (name_length > candidate_length) return 1;
    return 0;
}

const StyleProperty *find_property(const char *name, std::size_t name_length)
{
    std::size_t first = 0;
    std::size_t last = kStylePropertyCount;
    while (first < last) {
        const std::size_t middle = first + (last - first) / 2;
        const int comparison = compare_name(name, name_length, kStyleProperties[middle].name);
        if (comparison == 0) return &kStyleProperties[middle];
        if (comparison < 0) last = middle;
        else first = middle + 1;
    }
    return nullptr;
}

bool is_edit_distance_one(const char *name, std::size_t name_length, const char *candidate)
{
    const std::size_t candidate_length = std::strlen(candidate);
    if (name_length + 1 < candidate_length || candidate_length + 1 < name_length) {
        return false;
    }

    std::size_t name_index = 0;
    std::size_t candidate_index = 0;
    bool changed = false;
    while (name_index < name_length && candidate_index < candidate_length) {
        if (name[name_index] == candidate[candidate_index]) {
            ++name_index;
            ++candidate_index;
            continue;
        }
        if (changed) return false;
        changed = true;
        if (name_length > candidate_length) ++name_index;
        else if (name_length < candidate_length) ++candidate_index;
        else {
            ++name_index;
            ++candidate_index;
        }
    }
    return changed || name_length != candidate_length;
}

const char *find_suggestion(const char *name, std::size_t name_length)
{
    if (name_length > kMaxStylePropertyNameLength + 1) return nullptr;
    for (std::size_t i = 0; i < kStylePropertyCount; ++i) {
        if (is_edit_distance_one(name, name_length, kStyleProperties[i].name)) {
            return kStyleProperties[i].name;
        }
    }
    return nullptr;
}

void validate_property(lua_State *L, const StyleProperty *property, int value_index)
{
    switch (property->value_kind) {
    case StyleProperty::COLOR:
        check_color(L, value_index, property->name);
        break;
    case StyleProperty::OPACITY:
        check_opacity(L, value_index, property->name);
        break;
    case StyleProperty::DIMENSION:
        check_dimension(L, value_index, property->name);
        break;
    case StyleProperty::OFFSET:
        check_offset(L, value_index, property->name);
        break;
    case StyleProperty::FLEX_FLOW:
        check_flex_flow(L, value_index);
        break;
    case StyleProperty::TEXT_ALIGN:
        check_text_align(L, value_index);
        break;
    case StyleProperty::TEXT_FONT:
        check_font(L, value_index);
        break;
    }
}

void visit_properties(lua_State *L, lv_obj_t *obj, int table_index,
                      lv_style_selector_t selector, bool apply)
{
    table_index = lua_absindex(L, table_index);
    const int top = lua_gettop(L);

    lua_pushnil(L);
    while (lua_next(L, table_index) != 0) {
        if (lua_type(L, -2) != LUA_TSTRING) {
            luaL_error(L, "style property keys must be strings");
        }

        std::size_t name_length = 0;
        const char *name = lua_tolstring(L, -2, &name_length);
        const StyleProperty *property = find_property(name, name_length);
        if (!property) {
            const char *suggestion = find_suggestion(name, name_length);
            if (suggestion) {
                luaL_error(L, "unknown style property '%s' (did you mean '%s'?)",
                           name, suggestion);
            }
            luaL_error(L, "unknown style property '%s'", name);
        }

        validate_property(L, property, -1);
        if (apply) property->apply(L, obj, -1, selector);
        lua_pop(L, 1);
    }
    lua_settop(L, top);
}

lv_style_selector_t parse_selector(lua_State *L)
{
    if (lua_isnoneornil(L, 3)) return kDefaultSelector;
    luaL_checktype(L, 3, LUA_TSTRING);

    std::size_t selector_length = 0;
    const char *selector = lua_tolstring(L, 3, &selector_length);
    const char *colon = static_cast<const char *>(std::memchr(selector, ':', selector_length));
    const std::size_t part_length = colon ? static_cast<std::size_t>(colon - selector)
                                          : selector_length;
    const char *state = nullptr;
    std::size_t state_length = 0;
    if (colon) {
        state = colon + 1;
        state_length = selector_length - part_length - 1;
        if (state_length == 0
            || std::memchr(state, ':', state_length) != nullptr) {
            luaL_error(L, "unsupported style selector '%s'", selector);
            return LV_PART_MAIN;
        }
    }

    lv_style_selector_t result = LV_PART_MAIN;
    if (equals(selector, part_length, "main") || equals(selector, part_length, "default")) {
        result = LV_PART_MAIN;
    } else if (equals(selector, part_length, "indicator")) {
        result = LV_PART_INDICATOR;
    } else if (equals(selector, part_length, "knob")) {
        result = LV_PART_KNOB;
    } else if (equals(selector, part_length, "items")) {
        result = LV_PART_ITEMS;
    } else if (equals(selector, part_length, "scrollbar")) {
        result = LV_PART_SCROLLBAR;
    } else {
        luaL_error(L, "unsupported style selector '%s'", selector);
        return LV_PART_MAIN;
    }

    if (!state) return result;
    if (equals(state, state_length, "pressed")) return result | LV_STATE_PRESSED;
    if (equals(state, state_length, "checked")) return result | LV_STATE_CHECKED;
    if (equals(state, state_length, "disabled")) return result | LV_STATE_DISABLED;
    if (equals(state, state_length, "focused")) return result | LV_STATE_FOCUSED;

    luaL_error(L, "unsupported style selector '%s'", selector);
    return LV_PART_MAIN;
}

int obj_style(lua_State *L)
{
    BadgeObjRecord *record = badge_lua_obj_check_mutable_record(L, 1);
    luaL_checktype(L, 2, LUA_TTABLE);
    const lv_style_selector_t selector = parse_selector(L);
    badge_lua_style_apply_props(L, record->obj, 2, selector);
    return 0;
}

void set_theme_value(lua_State *L, const char *name, std::uint32_t value)
{
    lua_pushinteger(L, static_cast<lua_Integer>(value));
    lua_setfield(L, -2, name);
}

} // namespace

void badge_lua_style_apply_color(lv_obj_t *obj, std::uint32_t rgb,
                                 lv_style_selector_t selector)
{
    if (lv_obj_has_class(obj, &lv_label_class)) {
        lv_obj_set_style_text_color(obj, lv_color_hex(rgb), selector);
    } else {
        lv_obj_set_style_bg_color(obj, lv_color_hex(rgb), selector);
        lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, selector);
    }
}

void badge_lua_style_validate_props(lua_State *L, int table_index)
{
    luaL_checktype(L, table_index, LUA_TTABLE);
    visit_properties(L, nullptr, table_index, kDefaultSelector, false);
}

void badge_lua_style_apply_props(lua_State *L, lv_obj_t *obj, int table_index,
                                 lv_style_selector_t selector)
{
    luaL_checktype(L, table_index, LUA_TTABLE);
    visit_properties(L, obj, table_index, selector, true);
}

void badge_lua_style_add_object_methods(lua_State *L, int methods_index)
{
    methods_index = lua_absindex(L, methods_index);
    lua_pushcfunction(L, obj_style);
    lua_setfield(L, methods_index, "style");
}

void badge_lua_style_install_theme(lua_State *L, int badge_index)
{
    badge_index = lua_absindex(L, badge_index);
    lua_getfield(L, badge_index, "ui");
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushvalue(L, -1);
        lua_setfield(L, badge_index, "ui");
    }

    lua_getfield(L, -1, "theme");
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushvalue(L, -1);
        lua_setfield(L, -3, "theme");
    }

    set_theme_value(L, "background", BadgeTheme::BACKGROUND);
    set_theme_value(L, "panel", BadgeTheme::PANEL);
    set_theme_value(L, "surface", BadgeTheme::SURFACE);
    set_theme_value(L, "track", BadgeTheme::TRACK);
    set_theme_value(L, "border", BadgeTheme::BORDER);
    set_theme_value(L, "accent", BadgeTheme::ACCENT);
    set_theme_value(L, "accent_detail", BadgeTheme::ACCENT_DETAIL);
    set_theme_value(L, "text", BadgeTheme::TEXT);
    set_theme_value(L, "text_soft", BadgeTheme::TEXT_SOFT);
    set_theme_value(L, "text_muted", BadgeTheme::TEXT_MUTED);
    set_theme_value(L, "text_dim", BadgeTheme::TEXT_DIM);
    lua_pop(L, 1);

    lua_pushinteger(L, 320);
    lua_setfield(L, -2, "screen_width");
    lua_pushinteger(L, 240);
    lua_setfield(L, -2, "screen_height");
    lua_pop(L, 1);
}
