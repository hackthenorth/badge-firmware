#include "lua_internal.h"
#include "ui_theme.h"

extern "C" {
#include "lauxlib.h"
}

#include "lvgl.h"

#include <climits>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace {

enum class WidgetKind : std::uint8_t {
    Label,
    Box,
    Bar,
    Arc,
    Slider,
    Image,
    Line,
    Button,
    Switch,
    Checkbox,
    Roller,
    Textarea,
};

struct TablePreparation {
    BadgeObjRecord *parent;
    bool has_points;
    int style_properties;
};

bool key_is(const char *name, std::size_t length, const char *literal)
{
    const std::size_t literal_length = std::strlen(literal);
    return length == literal_length && std::memcmp(name, literal, length) == 0;
}

const char *kind_name(WidgetKind kind)
{
    switch (kind) {
    case WidgetKind::Label: return "label";
    case WidgetKind::Box: return "box";
    case WidgetKind::Bar: return "bar";
    case WidgetKind::Arc: return "arc";
    case WidgetKind::Slider: return "slider";
    case WidgetKind::Image: return "image";
    case WidgetKind::Line: return "line";
    case WidgetKind::Button: return "button";
    case WidgetKind::Switch: return "switch";
    case WidgetKind::Checkbox: return "checkbox";
    case WidgetKind::Roller: return "roller";
    case WidgetKind::Textarea: return "textarea";
    }
    return "widget";
}

bool supports_text(WidgetKind kind)
{
    return kind == WidgetKind::Label || kind == WidgetKind::Checkbox
        || kind == WidgetKind::Textarea;
}

bool supports_value(WidgetKind kind)
{
    return kind == WidgetKind::Bar || kind == WidgetKind::Arc
        || kind == WidgetKind::Slider;
}

bool supports_checked(WidgetKind kind)
{
    return kind == WidgetKind::Switch || kind == WidgetKind::Checkbox;
}

bool is_common_key(const char *name, std::size_t length)
{
    return key_is(name, length, "parent") || key_is(name, length, "x")
        || key_is(name, length, "y") || key_is(name, length, "w")
        || key_is(name, length, "h") || key_is(name, length, "align")
        || key_is(name, length, "align_x") || key_is(name, length, "align_y")
        || key_is(name, length, "hidden") || key_is(name, length, "clickable");
}

bool is_any_widget_key(const char *name, std::size_t length)
{
    return is_common_key(name, length) || key_is(name, length, "text")
        || key_is(name, length, "value") || key_is(name, length, "min")
        || key_is(name, length, "max") || key_is(name, length, "checked")
        || key_is(name, length, "options") || key_is(name, length, "src")
        || key_is(name, length, "points");
}

bool is_supported_widget_key(WidgetKind kind, const char *name, std::size_t length)
{
    if (is_common_key(name, length)) return true;
    if (key_is(name, length, "text")) return supports_text(kind);
    if (key_is(name, length, "value") || key_is(name, length, "min")
        || key_is(name, length, "max")) return supports_value(kind);
    if (key_is(name, length, "checked")) return supports_checked(kind);
    if (key_is(name, length, "options")) return kind == WidgetKind::Roller;
    if (key_is(name, length, "src")) return kind == WidgetKind::Image;
    if (key_is(name, length, "points")) return kind == WidgetKind::Line;
    return false;
}

int32_t check_coordinate(lua_State *L, int index, const char *name)
{
    const lua_Integer value = luaL_checkinteger(L, index);
    if (value < LV_COORD_MIN || value > LV_COORD_MAX) {
        luaL_error(L, "%s is outside the LVGL coordinate range", name);
    }
    return static_cast<int32_t>(value);
}

int32_t check_dimension(lua_State *L, int index, const char *name)
{
    const lua_Integer value = luaL_checkinteger(L, index);
    if (value < 0 || value > LV_COORD_MAX) {
        luaL_error(L, "%s must be between 0 and %d", name, LV_COORD_MAX);
    }
    return static_cast<int32_t>(value);
}

int32_t check_int32(lua_State *L, int index, const char *name)
{
    const lua_Integer value = luaL_checkinteger(L, index);
    if (value < INT32_MIN || value > INT32_MAX) {
        luaL_error(L, "%s must fit in a signed 32-bit integer", name);
    }
    return static_cast<int32_t>(value);
}

void check_text_value(lua_State *L, int index, const char *name)
{
    if (lua_type(L, index) != LUA_TSTRING) {
        luaL_error(L, "%s must be a string", name);
    }
    std::size_t length = 0;
    const char *value = lua_tolstring(L, index, &length);
    if (length > 1024) luaL_error(L, "%s must be at most 1024 bytes", name);
    if (std::memchr(value, '\0', length) != nullptr) {
        luaL_error(L, "%s must not contain NUL bytes", name);
    }
}

bool get_field(lua_State *L, int table_index, const char *name)
{
    lua_getfield(L, table_index, name);
    if (!lua_isnil(L, -1)) return true;
    lua_pop(L, 1);
    return false;
}

bool get_integer_field(lua_State *L, int table_index, const char *name, int32_t *value)
{
    if (!get_field(L, table_index, name)) return false;
    *value = static_cast<int32_t>(lua_tointeger(L, -1));
    lua_pop(L, 1);
    return true;
}

bool get_boolean_field(lua_State *L, int table_index, const char *name, bool *value)
{
    if (!get_field(L, table_index, name)) return false;
    *value = lua_toboolean(L, -1);
    lua_pop(L, 1);
    return true;
}

void validate_field(lua_State *L, LuaCtx *ctx, WidgetKind kind,
                    const char *name, std::size_t length, int value_index,
                    TablePreparation *preparation)
{
    if (key_is(name, length, "parent")) {
        if (!lua_isnil(L, value_index)) {
            preparation->parent = badge_lua_obj_check_record(L, value_index);
        }
    } else if (key_is(name, length, "x") || key_is(name, length, "y")
               || key_is(name, length, "align_x") || key_is(name, length, "align_y")) {
        check_coordinate(L, value_index, name);
    } else if (key_is(name, length, "w") || key_is(name, length, "h")) {
        check_dimension(L, value_index, name);
    } else if (key_is(name, length, "align")) {
        if (lua_type(L, value_index) != LUA_TSTRING) {
            luaL_error(L, "align must be a string");
        }
        lv_align_t align;
        if (!badge_lua_obj_parse_align(lua_tostring(L, value_index), &align)) {
            luaL_error(L, "unknown alignment '%s'", lua_tostring(L, value_index));
        }
    } else if (key_is(name, length, "hidden") || key_is(name, length, "clickable")
               || key_is(name, length, "checked")) {
        luaL_checktype(L, value_index, LUA_TBOOLEAN);
    } else if (key_is(name, length, "text") || key_is(name, length, "options")) {
        check_text_value(L, value_index, name);
    } else if (key_is(name, length, "value") || key_is(name, length, "min")
               || key_is(name, length, "max")) {
        check_int32(L, value_index, name);
    } else if (key_is(name, length, "src")) {
        badge_lua_obj_validate_image_src(L, ctx, value_index);
    } else if (key_is(name, length, "points")) {
        luaL_checktype(L, value_index, LUA_TTABLE);
        preparation->has_points = true;
    } else {
        luaL_error(L, "unsupported %s property '%s'", kind_name(kind), name);
    }
}

void validate_range_fields(lua_State *L, int table_index)
{
    int32_t minimum = 0;
    int32_t maximum = 0;
    const bool has_minimum = get_integer_field(L, table_index, "min", &minimum);
    const bool has_maximum = get_integer_field(L, table_index, "max", &maximum);
    if (has_minimum && has_maximum && minimum > maximum) {
        luaL_error(L, "minimum must not exceed maximum");
    }
}

TablePreparation prepare_table(lua_State *L, WidgetKind kind)
{
    LuaCtx *ctx = badge_lua_get_ctx(L);
    if (!ctx || !ctx->root) luaL_error(L, "Lua app context is unavailable");

    TablePreparation preparation = { ctx->root_record, false, 0 };
    if (!preparation.parent) {
        if (!badge_lua_obj_push(L, ctx->root, BADGE_OBJ_ROOT | BADGE_OBJ_FOREIGN)) {
            luaL_error(L, "failed to allocate root handle");
        }
        preparation.parent = badge_lua_obj_check_record(L, -1);
        lua_pop(L, 1);
    }

    const int properties = lua_absindex(L, 1);
    lua_newtable(L);
    preparation.style_properties = lua_gettop(L);

    lua_pushnil(L);
    while (lua_next(L, properties) != 0) {
        if (lua_type(L, -2) != LUA_TSTRING) {
            luaL_error(L, "widget property keys must be strings");
        }
        std::size_t length = 0;
        const char *name = lua_tolstring(L, -2, &length);
        if (is_any_widget_key(name, length)) {
            if (!is_supported_widget_key(kind, name, length)) {
                luaL_error(L, "%s does not support property '%s'", kind_name(kind), name);
            }
            validate_field(L, ctx, kind, name, length, -1, &preparation);
        } else {
            lua_pushvalue(L, -2);
            lua_pushvalue(L, -2);
            lua_rawset(L, preparation.style_properties);
        }
        lua_pop(L, 1);
    }

    if (supports_value(kind)) validate_range_fields(L, properties);
    badge_lua_style_validate_props(L, preparation.style_properties);
    badge_lua_obj_check_object_budget(L, preparation.parent->ctx);

    return preparation;
}

lv_obj_t *create_widget(WidgetKind kind, lv_obj_t *parent)
{
    lv_obj_t *obj = nullptr;
    switch (kind) {
    case WidgetKind::Label:
        obj = lv_label_create(parent);
        // LVGL's default text color is built for light backgrounds; on this
        // dark badge it renders as dim grey. Default to the theme text
        // color so plain labels are readable; text_color overrides it.
        if (obj) {
            lv_obj_set_style_text_color(obj, lv_color_hex(BadgeTheme::TEXT), 0);
        }
        break;
    case WidgetKind::Box: obj = lv_obj_create(parent); break;
    case WidgetKind::Bar: obj = lv_bar_create(parent); break;
    case WidgetKind::Arc: obj = lv_arc_create(parent); break;
    case WidgetKind::Slider: obj = lv_slider_create(parent); break;
    case WidgetKind::Image: obj = lv_image_create(parent); break;
    case WidgetKind::Line: obj = lv_line_create(parent); break;
    case WidgetKind::Button: obj = lv_button_create(parent); break;
    case WidgetKind::Switch: obj = lv_switch_create(parent); break;
    case WidgetKind::Checkbox: obj = lv_checkbox_create(parent); break;
    case WidgetKind::Roller: obj = lv_roller_create(parent); break;
    case WidgetKind::Textarea: obj = lv_textarea_create(parent); break;
    }
    if (obj && (kind == WidgetKind::Box || kind == WidgetKind::Button)) {
        lv_obj_remove_style_all(obj);
        lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    }
    return obj;
}

void apply_common_table_fields(lua_State *L, lv_obj_t *obj, int table_index)
{
    int32_t value = 0;
    if (get_integer_field(L, table_index, "x", &value)) lv_obj_set_x(obj, value);
    if (get_integer_field(L, table_index, "y", &value)) lv_obj_set_y(obj, value);
    if (get_integer_field(L, table_index, "w", &value)) lv_obj_set_width(obj, value);
    if (get_integer_field(L, table_index, "h", &value)) lv_obj_set_height(obj, value);

    int32_t align_x = 0;
    int32_t align_y = 0;
    const bool has_align_x = get_integer_field(L, table_index, "align_x", &align_x);
    const bool has_align_y = get_integer_field(L, table_index, "align_y", &align_y);
    if (get_field(L, table_index, "align")) {
        lv_align_t align;
        badge_lua_obj_parse_align(lua_tostring(L, -1), &align);
        lua_pop(L, 1);
        lv_obj_align(obj, align, align_x, align_y);
    } else if (has_align_x || has_align_y) {
        lv_obj_align(obj, LV_ALIGN_CENTER, align_x, align_y);
    }

    bool enabled = false;
    if (get_boolean_field(L, table_index, "hidden", &enabled)) {
        if (enabled) lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
    }
    if (get_boolean_field(L, table_index, "clickable", &enabled)) {
        if (enabled) lv_obj_add_flag(obj, LV_OBJ_FLAG_CLICKABLE);
        else lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    }
}

void apply_value(lv_obj_t *obj, WidgetKind kind, int32_t value)
{
    if (kind == WidgetKind::Slider) lv_slider_set_value(obj, value, LV_ANIM_OFF);
    else if (kind == WidgetKind::Bar) lv_bar_set_value(obj, value, LV_ANIM_OFF);
    else lv_arc_set_value(obj, value);
}

void apply_range(lv_obj_t *obj, WidgetKind kind, int32_t minimum, int32_t maximum)
{
    if (kind == WidgetKind::Slider) lv_slider_set_range(obj, minimum, maximum);
    else if (kind == WidgetKind::Bar) lv_bar_set_range(obj, minimum, maximum);
    else lv_arc_set_range(obj, minimum, maximum);
}

void apply_table_widget_fields(lua_State *L, BadgeObjRecord *record,
                               WidgetKind kind, int table_index,
                               const TablePreparation &preparation)
{
    if (supports_value(kind)) {
        int32_t minimum = 0;
        int32_t maximum = 100;
        const bool has_minimum = get_integer_field(L, table_index, "min", &minimum);
        const bool has_maximum = get_integer_field(L, table_index, "max", &maximum);
        if (has_minimum || has_maximum) apply_range(record->obj, kind, minimum, maximum);

        int32_t value = 0;
        if (get_integer_field(L, table_index, "value", &value)) {
            apply_value(record->obj, kind, value);
        }
    }
    if (supports_text(kind) && get_field(L, table_index, "text")) {
        badge_lua_obj_set_text(L, record, -1);
        lua_pop(L, 1);
    }
    if (kind == WidgetKind::Image && get_field(L, table_index, "src")) {
        badge_lua_obj_set_src(L, record, -1);
        lua_pop(L, 1);
    }
    if (supports_checked(kind) && get_field(L, table_index, "checked")) {
        badge_lua_obj_set_checked(L, record, -1);
        lua_pop(L, 1);
    }
    if (kind == WidgetKind::Roller && get_field(L, table_index, "options")) {
        badge_lua_obj_set_options(L, record, -1);
        lua_pop(L, 1);
    }
    if (kind == WidgetKind::Line && preparation.has_points) {
        lua_getfield(L, table_index, "points");
        std::uint32_t point_count = 0;
        lv_point_precise_t *points = badge_lua_obj_copy_points(L, -1, &point_count);
        lua_pop(L, 1);
        badge_lua_obj_set_native_points(record, points, point_count);
    }
}

int create_from_table(lua_State *L, WidgetKind kind)
{
    if (lua_gettop(L) != 1) {
        return luaL_error(L, "%s table form accepts one argument", kind_name(kind));
    }
    TablePreparation preparation = prepare_table(L, kind);
    lv_obj_t *obj = create_widget(kind, preparation.parent->obj);
    if (!obj) {
        return luaL_error(L, "failed to create %s widget", kind_name(kind));
    }

    apply_common_table_fields(L, obj, 1);
    badge_lua_style_apply_props(L, obj, preparation.style_properties,
                                 static_cast<lv_style_selector_t>(LV_PART_MAIN));
    if (!badge_lua_obj_push(L, obj, BADGE_OBJ_COUNTED)) {
        lv_obj_delete(obj);
        return luaL_error(L, "failed to allocate %s handle", kind_name(kind));
    }
    BadgeObjRecord *record = badge_lua_obj_check_record(L, -1);
    apply_table_widget_fields(L, record, kind, 1, preparation);

    lua_replace(L, 1);
    lua_settop(L, 1);
    return 1;
}

void check_arg_count(lua_State *L, WidgetKind kind, int maximum)
{
    const int count = lua_gettop(L);
    if (count < 1 || count > maximum) {
        luaL_error(L, "%s positional form expects between 1 and %d arguments",
                   kind_name(kind), maximum);
    }
}

int create_from_positional(lua_State *L, WidgetKind kind)
{
    const int maximum = (kind == WidgetKind::Label || kind == WidgetKind::Image
                         || kind == WidgetKind::Line || kind == WidgetKind::Switch
                         || kind == WidgetKind::Roller || kind == WidgetKind::Textarea)
        ? 2 : (kind == WidgetKind::Box || kind == WidgetKind::Button
               || kind == WidgetKind::Checkbox) ? 3 : 4;
    check_arg_count(L, kind, maximum);
    BadgeObjRecord *parent = badge_lua_obj_check_record(L, 1);

    int32_t width = 0;
    int32_t height = 0;
    int32_t minimum = 0;
    int32_t maximum_value = 100;
    int32_t value = 0;
    badge_lua_obj_check_object_budget(L, parent->ctx);

    switch (kind) {
    case WidgetKind::Label:
    case WidgetKind::Textarea:
    case WidgetKind::Roller:
        if (lua_gettop(L) == 2) check_text_value(L, 2, kind == WidgetKind::Roller ? "options" : "text");
        break;
    case WidgetKind::Checkbox:
        if (lua_gettop(L) >= 2) check_text_value(L, 2, "text");
        if (lua_gettop(L) == 3) {
            luaL_checktype(L, 3, LUA_TBOOLEAN);
        }
        break;
    case WidgetKind::Box:
        width = lua_gettop(L) >= 2 ? check_dimension(L, 2, "width") : 20;
        height = lua_gettop(L) >= 3 ? check_dimension(L, 3, "height") : 20;
        break;
    case WidgetKind::Button:
        width = lua_gettop(L) >= 2 ? check_dimension(L, 2, "width") : 80;
        height = lua_gettop(L) >= 3 ? check_dimension(L, 3, "height") : 32;
        break;
    case WidgetKind::Bar:
    case WidgetKind::Arc:
    case WidgetKind::Slider:
        if (lua_gettop(L) >= 2) minimum = check_int32(L, 2, "minimum");
        if (lua_gettop(L) >= 3) maximum_value = check_int32(L, 3, "maximum");
        if (minimum > maximum_value) luaL_error(L, "minimum must not exceed maximum");
        value = lua_gettop(L) >= 4 ? check_int32(L, 4, "value") : minimum;
        break;
    case WidgetKind::Image:
        if (lua_gettop(L) != 2) luaL_error(L, "image positional form expects parent and src");
        badge_lua_obj_validate_image_src(L, parent->ctx, 2);
        break;
    case WidgetKind::Line:
        if (lua_gettop(L) != 2) luaL_error(L, "line positional form expects parent and points");
        luaL_checktype(L, 2, LUA_TTABLE);
        break;
    case WidgetKind::Switch:
        if (lua_gettop(L) == 2) {
            luaL_checktype(L, 2, LUA_TBOOLEAN);
        }
        break;
    }

    lv_obj_t *obj = create_widget(kind, parent->obj);
    if (!obj) {
        return luaL_error(L, "failed to create %s widget", kind_name(kind));
    }
    if (!badge_lua_obj_push(L, obj, BADGE_OBJ_COUNTED)) {
        lv_obj_delete(obj);
        return luaL_error(L, "failed to allocate %s handle", kind_name(kind));
    }
    BadgeObjRecord *record = badge_lua_obj_check_record(L, -1);

    switch (kind) {
    case WidgetKind::Label:
    case WidgetKind::Textarea:
        if (lua_gettop(L) >= 2) badge_lua_obj_set_text(L, record, 2);
        break;
    case WidgetKind::Checkbox:
        if (lua_gettop(L) >= 2) badge_lua_obj_set_text(L, record, 2);
        if (lua_gettop(L) >= 3) badge_lua_obj_set_checked(L, record, 3);
        break;
    case WidgetKind::Box:
    case WidgetKind::Button:
        lv_obj_set_size(obj, width, height);
        break;
    case WidgetKind::Bar:
    case WidgetKind::Arc:
    case WidgetKind::Slider:
        apply_range(obj, kind, minimum, maximum_value);
        apply_value(obj, kind, value);
        break;
    case WidgetKind::Image:
        badge_lua_obj_set_src(L, record, 2);
        break;
    case WidgetKind::Line: {
        std::uint32_t point_count = 0;
        lv_point_precise_t *points = badge_lua_obj_copy_points(L, 2, &point_count);
        badge_lua_obj_set_native_points(record, points, point_count);
        break;
    }
    case WidgetKind::Switch:
        if (lua_gettop(L) >= 2) badge_lua_obj_set_checked(L, record, 2);
        break;
    case WidgetKind::Roller:
        if (lua_gettop(L) >= 2) badge_lua_obj_set_options(L, record, 2);
        break;
    }
    return 1;
}

int create_widget_factory(lua_State *L, WidgetKind kind)
{
    if (lua_istable(L, 1)) return create_from_table(L, kind);
    return create_from_positional(L, kind);
}

int ui_label(lua_State *L) { return create_widget_factory(L, WidgetKind::Label); }
int ui_box(lua_State *L) { return create_widget_factory(L, WidgetKind::Box); }
int ui_bar(lua_State *L) { return create_widget_factory(L, WidgetKind::Bar); }
int ui_arc(lua_State *L) { return create_widget_factory(L, WidgetKind::Arc); }
int ui_slider(lua_State *L) { return create_widget_factory(L, WidgetKind::Slider); }
int ui_image(lua_State *L) { return create_widget_factory(L, WidgetKind::Image); }
int ui_line(lua_State *L) { return create_widget_factory(L, WidgetKind::Line); }
int ui_button(lua_State *L) { return create_widget_factory(L, WidgetKind::Button); }
int ui_switch(lua_State *L) { return create_widget_factory(L, WidgetKind::Switch); }
int ui_checkbox(lua_State *L) { return create_widget_factory(L, WidgetKind::Checkbox); }
int ui_roller(lua_State *L) { return create_widget_factory(L, WidgetKind::Roller); }
int ui_textarea(lua_State *L) { return create_widget_factory(L, WidgetKind::Textarea); }

void install_functions(lua_State *L, int table_index, bool all)
{
    table_index = lua_absindex(L, table_index);
    static const luaL_Reg all_functions[] = {
        { "label", ui_label },
        { "box", ui_box },
        { "bar", ui_bar },
        { "arc", ui_arc },
        { "slider", ui_slider },
        { "image", ui_image },
        { "line", ui_line },
        { "button", ui_button },
        { "switch", ui_switch },
        { "checkbox", ui_checkbox },
        { "roller", ui_roller },
        { "textarea", ui_textarea },
        { nullptr, nullptr },
    };
    if (all) {
        luaL_setfuncs(L, all_functions, 0);
    } else {
        lua_pushcfunction(L, ui_label);
        lua_setfield(L, table_index, "label");
        lua_pushcfunction(L, ui_box);
        lua_setfield(L, table_index, "box");
    }
}

} // namespace

void badge_lua_ui_install(lua_State *L, int badge_index)
{
    badge_index = lua_absindex(L, badge_index);
    lua_getfield(L, badge_index, "ui");
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushvalue(L, -1);
        lua_setfield(L, badge_index, "ui");
    }
    install_functions(L, -1, true);
    lua_pop(L, 1);
}

void badge_lua_ui_install_legacy(lua_State *L, int badge_index)
{
    install_functions(L, badge_index, false);
}
