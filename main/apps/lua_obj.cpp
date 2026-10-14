#include "lua_internal.h"

extern "C" {
#include "lauxlib.h"
}

#include "lvgl.h"

#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/stat.h>
#include <unistd.h>

namespace {

constexpr const char *kObjMetatable = "badge.obj";
constexpr std::uint32_t kMaxLiveObjects = 512;

char kCtxRegistryKey;
char kRecordMapRegistryKey;
char kUserdataMapRegistryKey;

void record_release(BadgeObjRecord *record)
{
    if (!record || record->refcount == 0) return;
    --record->refcount;
    if (record->refcount != 0) return;

    if (record->aux) {
        void *aux = record->aux;
        BadgeObjAuxDeleter deleter = record->aux_deleter;
        record->aux = nullptr;
        record->aux_deleter = nullptr;
        if (deleter) deleter(aux);
    }

    if (record->ctx && record->ctx->record_count > 0) {
        --record->ctx->record_count;
    }
    std::free(record);
}

void remove_pointer_entries(LuaCtx *ctx, lv_obj_t *obj, BadgeObjRecord *record)
{
    if (!ctx || !ctx->state_live || !ctx->L || !obj || !record) return;

    lua_State *L = ctx->L;
    int top = lua_gettop(L);

    // These tables and keys already exist. Raw deletion only replaces an
    // existing slot with nil, so delete callbacks do not allocate or run Lua.
    lua_rawgetp(L, LUA_REGISTRYINDEX, &kRecordMapRegistryKey);
    if (lua_istable(L, -1)) {
        lua_pushlightuserdata(L, obj);
        lua_rawget(L, -2);
        bool matches = lua_touserdata(L, -1) == record;
        lua_pop(L, 1);
        if (matches) {
            lua_pushlightuserdata(L, obj);
            lua_pushnil(L);
            lua_rawset(L, -3);
        }
    }
    lua_pop(L, 1);

    lua_rawgetp(L, LUA_REGISTRYINDEX, &kUserdataMapRegistryKey);
    if (lua_istable(L, -1)) {
        lua_pushlightuserdata(L, obj);
        lua_rawget(L, -2);
        BadgeObj *ud = static_cast<BadgeObj *>(lua_touserdata(L, -1));
        bool matches = ud && ud->record == record;
        lua_pop(L, 1);
        if (matches) {
            lua_pushlightuserdata(L, obj);
            lua_pushnil(L);
            lua_rawset(L, -3);
        }
    }

    lua_settop(L, top);
}

void free_aux(BadgeObjRecord *record)
{
    if (!record || !record->aux) return;
    void *aux = record->aux;
    BadgeObjAuxDeleter deleter = record->aux_deleter;
    record->aux = nullptr;
    record->aux_deleter = nullptr;
    if (deleter) deleter(aux);
}

void invalidate_record(BadgeObjRecord *record, lv_obj_t *obj)
{
    if (!record || record->obj != obj) return;

    LuaCtx *ctx = record->ctx;
    record->obj = nullptr;
    free_aux(record);

    if (ctx && (record->flags & BADGE_OBJ_ROOT)) {
        if (ctx->root == obj) ctx->root = nullptr;
        if (ctx->root_record == record) ctx->root_record = nullptr;
    }
    if (ctx && (record->flags & BADGE_OBJ_COUNTED)) {
        record->flags &= ~BADGE_OBJ_COUNTED;
        if (ctx->live_object_count > 0) --ctx->live_object_count;
    }

    remove_pointer_entries(ctx, obj, record);
    record_release(record);  // Drop the widget's reference.
}

void obj_delete_event(lv_event_t *event)
{
    BadgeObjRecord *record = static_cast<BadgeObjRecord *>(lv_event_get_user_data(event));
    lv_obj_t *obj = lv_event_get_current_target_obj(event);
    invalidate_record(record, obj);
}

BadgeObjRecord *record_from_map(lua_State *L, LuaCtx *ctx, lv_obj_t *obj)
{
    BadgeObjRecord *record = nullptr;
    lua_rawgetp(L, LUA_REGISTRYINDEX, &kRecordMapRegistryKey);
    if (lua_istable(L, -1)) {
        lua_pushlightuserdata(L, obj);
        lua_rawget(L, -2);
        record = static_cast<BadgeObjRecord *>(lua_touserdata(L, -1));
        lua_pop(L, 1);
    }
    lua_pop(L, 1);

    if (record && record->ctx == ctx && record->obj == obj) return record;

    // An old weak userdata can outlive its widget. Never associate a recycled
    // LVGL address with that stale record; the new mapping overwrites it.
    if (record) remove_pointer_entries(ctx, obj, record);
    return nullptr;
}

BadgeObjRecord *record_create(lua_State *L, LuaCtx *ctx, lv_obj_t *obj,
                              std::uint32_t flags)
{
    if (flags & BADGE_OBJ_ROOT) flags &= ~BADGE_OBJ_COUNTED;

    BadgeObjRecord *record = static_cast<BadgeObjRecord *>(
        std::calloc(1, sizeof(BadgeObjRecord)));
    if (!record) return nullptr;

    record->obj = obj;
    record->ctx = ctx;
    record->flags = flags;
    record->refcount = 1;  // Held until LV_EVENT_DELETE (or root detach).
    ++ctx->record_count;

    if (!lv_obj_add_event_cb(obj, obj_delete_event, LV_EVENT_DELETE, record)) {
        record_release(record);
        return nullptr;
    }

    if (flags & BADGE_OBJ_COUNTED) ++ctx->live_object_count;
    if (flags & BADGE_OBJ_ROOT) ctx->root_record = record;

    lua_rawgetp(L, LUA_REGISTRYINDEX, &kRecordMapRegistryKey);
    lua_pushlightuserdata(L, obj);
    lua_pushlightuserdata(L, record);
    lua_rawset(L, -3);
    lua_pop(L, 1);
    return record;
}

bool push_existing_userdata(lua_State *L, lv_obj_t *obj, BadgeObjRecord *record)
{
    lua_rawgetp(L, LUA_REGISTRYINDEX, &kUserdataMapRegistryKey);
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        return false;
    }

    lua_pushlightuserdata(L, obj);
    lua_rawget(L, -2);
    BadgeObj *ud = static_cast<BadgeObj *>(lua_touserdata(L, -1));
    if (ud && ud->record == record) {
        lua_remove(L, -2);
        return true;
    }

    lua_pop(L, 2);
    return false;
}

void store_userdata(lua_State *L, lv_obj_t *obj, int userdata_index)
{
    userdata_index = lua_absindex(L, userdata_index);
    lua_rawgetp(L, LUA_REGISTRYINDEX, &kUserdataMapRegistryKey);
    lua_pushlightuserdata(L, obj);
    lua_pushvalue(L, userdata_index);
    lua_rawset(L, -3);
    lua_pop(L, 1);
}

BadgeObjRecord *check_record(lua_State *L, int index)
{
    BadgeObj *ud = static_cast<BadgeObj *>(luaL_checkudata(L, index, kObjMetatable));
    if (!ud->record || !ud->record->obj) {
        luaL_error(L, "widget has been deleted");
        return nullptr;
    }

    LuaCtx *ctx = badge_lua_get_ctx(L);
    if (!ctx || ud->record->ctx != ctx) {
        luaL_error(L, "widget belongs to another app");
        return nullptr;
    }
    return ud->record;
}

BadgeObjRecord *check_label_record(lua_State *L, int index)
{
    BadgeObjRecord *record = badge_lua_obj_check_mutable_record(L, index);
    if (!lv_obj_has_class(record->obj, &lv_label_class)) {
        luaL_argerror(L, index, "expected label widget");
        return nullptr;
    }
    return record;
}

bool is_text_widget(lv_obj_t *obj)
{
    return lv_obj_has_class(obj, &lv_label_class)
        || lv_obj_has_class(obj, &lv_textarea_class)
        || lv_obj_has_class(obj, &lv_checkbox_class);
}

bool is_value_widget(lv_obj_t *obj)
{
    return lv_obj_has_class(obj, &lv_bar_class)
        || lv_obj_has_class(obj, &lv_arc_class)
        || lv_obj_has_class(obj, &lv_slider_class);
}

bool is_checked_widget(lv_obj_t *obj)
{
    return lv_obj_has_class(obj, &lv_switch_class)
        || lv_obj_has_class(obj, &lv_checkbox_class);
}

const char *check_bounded_string(lua_State *L, int index, const char *what,
                                 std::size_t *length)
{
    if (lua_type(L, index) != LUA_TSTRING) {
        luaL_error(L, "%s must be a string", what);
    }
    const char *value = lua_tolstring(L, index, length);
    if (*length > 1024) {
        luaL_error(L, "%s must be at most 1024 bytes", what);
    }
    if (std::memchr(value, '\0', *length) != nullptr) {
        luaL_error(L, "%s must not contain NUL bytes", what);
    }
    return value;
}

int32_t check_int32(lua_State *L, int index, const char *what)
{
    const lua_Integer value = luaL_checkinteger(L, index);
    if (value < INT32_MIN || value > INT32_MAX) {
        luaL_error(L, "%s must fit in a signed 32-bit integer", what);
    }
    return static_cast<int32_t>(value);
}

int32_t check_coordinate(lua_State *L, int index, const char *what)
{
    const lua_Integer value = luaL_checkinteger(L, index);
    if (value < LV_COORD_MIN || value > LV_COORD_MAX) {
        luaL_error(L, "%s is outside the LVGL coordinate range", what);
    }
    return static_cast<int32_t>(value);
}

int32_t check_dimension(lua_State *L, int index, const char *what)
{
    const lua_Integer value = luaL_checkinteger(L, index);
    if (value < 0 || value > LV_COORD_MAX) {
        luaL_error(L, "%s must be between 0 and %d", what, LV_COORD_MAX);
    }
    return static_cast<int32_t>(value);
}

bool parse_align(const char *name, lv_align_t *align)
{
    if (!std::strcmp(name, "center"))           *align = LV_ALIGN_CENTER;
    else if (!std::strcmp(name, "top_left"))    *align = LV_ALIGN_TOP_LEFT;
    else if (!std::strcmp(name, "top_mid"))     *align = LV_ALIGN_TOP_MID;
    else if (!std::strcmp(name, "top_right"))   *align = LV_ALIGN_TOP_RIGHT;
    else if (!std::strcmp(name, "bottom_left")) *align = LV_ALIGN_BOTTOM_LEFT;
    else if (!std::strcmp(name, "bottom_mid"))  *align = LV_ALIGN_BOTTOM_MID;
    else if (!std::strcmp(name, "bottom_right")) *align = LV_ALIGN_BOTTOM_RIGHT;
    else if (!std::strcmp(name, "left_mid"))    *align = LV_ALIGN_LEFT_MID;
    else if (!std::strcmp(name, "right_mid"))   *align = LV_ALIGN_RIGHT_MID;
    else if (!std::strcmp(name, "out_top_left")) *align = LV_ALIGN_OUT_TOP_LEFT;
    else if (!std::strcmp(name, "out_top_mid")) *align = LV_ALIGN_OUT_TOP_MID;
    else if (!std::strcmp(name, "out_top_right")) *align = LV_ALIGN_OUT_TOP_RIGHT;
    else if (!std::strcmp(name, "out_bottom_left")) *align = LV_ALIGN_OUT_BOTTOM_LEFT;
    else if (!std::strcmp(name, "out_bottom_mid")) *align = LV_ALIGN_OUT_BOTTOM_MID;
    else if (!std::strcmp(name, "out_bottom_right")) *align = LV_ALIGN_OUT_BOTTOM_RIGHT;
    else if (!std::strcmp(name, "out_left_top")) *align = LV_ALIGN_OUT_LEFT_TOP;
    else if (!std::strcmp(name, "out_left_mid")) *align = LV_ALIGN_OUT_LEFT_MID;
    else if (!std::strcmp(name, "out_left_bottom")) *align = LV_ALIGN_OUT_LEFT_BOTTOM;
    else if (!std::strcmp(name, "out_right_top")) *align = LV_ALIGN_OUT_RIGHT_TOP;
    else if (!std::strcmp(name, "out_right_mid")) *align = LV_ALIGN_OUT_RIGHT_MID;
    else if (!std::strcmp(name, "out_right_bottom")) *align = LV_ALIGN_OUT_RIGHT_BOTTOM;
    else return false;
    return true;
}

const lv_font_t *parse_font(const char *name)
{
    if (!name) return &lv_font_montserrat_16;
    if (!std::strcmp(name, "small")) return &lv_font_montserrat_14;
    if (!std::strcmp(name, "large")) return &lv_font_montserrat_20;
    return &lv_font_montserrat_16;
}

int obj_gc(lua_State *L)
{
    BadgeObj *ud = static_cast<BadgeObj *>(lua_touserdata(L, 1));
    if (ud && ud->record) {
        BadgeObjRecord *record = ud->record;
        ud->record = nullptr;
        record_release(record);
    }
    return 0;
}

int obj_eq(lua_State *L)
{
    BadgeObj *lhs = static_cast<BadgeObj *>(luaL_testudata(L, 1, kObjMetatable));
    BadgeObj *rhs = static_cast<BadgeObj *>(luaL_testudata(L, 2, kObjMetatable));
    lua_pushboolean(L, lhs && rhs && lhs->record && lhs->record == rhs->record);
    return 1;
}

int obj_tostring(lua_State *L)
{
    BadgeObj *ud = static_cast<BadgeObj *>(luaL_checkudata(L, 1, kObjMetatable));
    if (!ud->record || !ud->record->obj) {
        lua_pushliteral(L, "badge.obj (deleted)");
        return 1;
    }
    lua_pushfstring(L, "badge.%s: %p", badge_lua_obj_type_name(ud->record->obj),
                    static_cast<void *>(ud->record->obj));
    return 1;
}

int obj_type(lua_State *L)
{
    BadgeObjRecord *record = check_record(L, 1);
    lua_pushstring(L, badge_lua_obj_type_name(record->obj));
    return 1;
}

int obj_delete(lua_State *L)
{
    BadgeObjRecord *record = badge_lua_obj_check_mutable_record(L, 1);
    if (record->flags & BADGE_OBJ_FOREIGN) {
        return luaL_error(L, "cannot delete an internal widget");
    }
    lv_obj_t *obj = record->obj;
    lv_obj_delete(obj);
    return 0;
}

int obj_set_pos(lua_State *L)
{
    BadgeObjRecord *record = badge_lua_obj_check_mutable_record(L, 1);
    int x = static_cast<int>(check_coordinate(L, 2, "x"));
    int y = static_cast<int>(check_coordinate(L, 3, "y"));
    lv_obj_set_pos(record->obj, x, y);
    return 0;
}

int obj_set_size(lua_State *L)
{
    BadgeObjRecord *record = badge_lua_obj_check_mutable_record(L, 1);
    int width = static_cast<int>(check_dimension(L, 2, "width"));
    int height = static_cast<int>(check_dimension(L, 3, "height"));
    lv_obj_set_size(record->obj, width, height);
    return 0;
}

int obj_set_border(lua_State *L)
{
    BadgeObjRecord *record = badge_lua_obj_check_mutable_record(L, 1);
    std::uint32_t rgb = static_cast<std::uint32_t>(luaL_checkinteger(L, 2));
    int width = static_cast<int>(luaL_optinteger(L, 3, 1));
    lv_obj_set_style_border_color(record->obj, lv_color_hex(rgb), 0);
    lv_obj_set_style_border_width(record->obj, width, 0);
    return 0;
}

int obj_align(lua_State *L)
{
    BadgeObjRecord *record = badge_lua_obj_check_mutable_record(L, 1);
    const char *name = luaL_checkstring(L, 2);
    int x = static_cast<int>(luaL_optinteger(L, 3, 0));
    int y = static_cast<int>(luaL_optinteger(L, 4, 0));
    lv_align_t align;
    if (!badge_lua_obj_parse_align(name, &align)) {
        return luaL_error(L, "unknown alignment '%s'", name);
    }
    if (x < LV_COORD_MIN || x > LV_COORD_MAX || y < LV_COORD_MIN || y > LV_COORD_MAX) {
        return luaL_error(L, "alignment offsets are outside the LVGL coordinate range");
    }
    lv_obj_align(record->obj, align, x, y);
    return 0;
}

int obj_set_color(lua_State *L)
{
    BadgeObjRecord *record = badge_lua_obj_check_mutable_record(L, 1);
    std::uint32_t rgb = static_cast<std::uint32_t>(luaL_checkinteger(L, 2));
    badge_lua_style_apply_color(record->obj, rgb,
                                static_cast<lv_style_selector_t>(LV_PART_MAIN));
    return 0;
}

int obj_set_text(lua_State *L)
{
    BadgeObjRecord *record = badge_lua_obj_check_mutable_record(L, 1);
    badge_lua_obj_set_text(L, record, 2);
    return 0;
}

int obj_set_font_size(lua_State *L)
{
    BadgeObjRecord *record = check_label_record(L, 1);
    const char *name = luaL_checkstring(L, 2);
    lv_obj_set_style_text_font(record->obj, parse_font(name), 0);
    return 0;
}

int obj_set_value(lua_State *L)
{
    BadgeObjRecord *record = badge_lua_obj_check_mutable_record(L, 1);
    badge_lua_obj_set_value(L, record, 2);
    return 0;
}

int obj_set_range(lua_State *L)
{
    BadgeObjRecord *record = badge_lua_obj_check_mutable_record(L, 1);
    badge_lua_obj_set_range(L, record, 2, 3);
    return 0;
}

int obj_set_src(lua_State *L)
{
    BadgeObjRecord *record = badge_lua_obj_check_mutable_record(L, 1);
    badge_lua_obj_set_src(L, record, 2);
    return 0;
}

int obj_set_points(lua_State *L)
{
    BadgeObjRecord *record = badge_lua_obj_check_mutable_record(L, 1);
    if (!lv_obj_has_class(record->obj, &lv_line_class)) {
        return luaL_error(L, "set_points expects a line, got %s",
                          badge_lua_obj_type_name(record->obj));
    }
    std::uint32_t count = 0;
    lv_point_precise_t *points = badge_lua_obj_copy_points(L, 2, &count);
    badge_lua_obj_set_native_points(record, points, count);
    return 0;
}

int obj_set_checked(lua_State *L)
{
    BadgeObjRecord *record = badge_lua_obj_check_mutable_record(L, 1);
    badge_lua_obj_set_checked(L, record, 2);
    return 0;
}

int obj_get_checked(lua_State *L)
{
    BadgeObjRecord *record = check_record(L, 1);
    if (!is_checked_widget(record->obj)) {
        return luaL_error(L, "get_checked expects a switch or checkbox, got %s",
                          badge_lua_obj_type_name(record->obj));
    }
    lua_pushboolean(L, lv_obj_has_state(record->obj, LV_STATE_CHECKED));
    return 1;
}

int obj_set_options(lua_State *L)
{
    BadgeObjRecord *record = badge_lua_obj_check_mutable_record(L, 1);
    badge_lua_obj_set_options(L, record, 2);
    return 0;
}

int obj_get_selected(lua_State *L)
{
    BadgeObjRecord *record = check_record(L, 1);
    if (!lv_obj_has_class(record->obj, &lv_roller_class)) {
        return luaL_error(L, "get_selected expects a roller, got %s",
                          badge_lua_obj_type_name(record->obj));
    }
    lua_pushinteger(L, static_cast<lua_Integer>(lv_roller_get_selected(record->obj)));
    return 1;
}

int obj_parent(lua_State *L)
{
    BadgeObjRecord *record = check_record(L, 1);
    LuaCtx *ctx = record->ctx;
    if (record->obj == ctx->root) {
        lua_pushnil(L);
        return 1;
    }

    lv_obj_t *parent = lv_obj_get_parent(record->obj);
    if (!parent || parent != ctx->root) {
        // A valid app widget must lead to the app root. Do not expose screen
        // internals if the hierarchy no longer does.
        lv_obj_t *cursor = parent;
        while (cursor && cursor != ctx->root) cursor = lv_obj_get_parent(cursor);
        if (cursor != ctx->root) {
            lua_pushnil(L);
            return 1;
        }
    }
    if (!badge_lua_obj_push(L, parent, BADGE_OBJ_FOREIGN)) {
        return luaL_error(L, "failed to allocate parent handle");
    }
    return 1;
}

int obj_child(lua_State *L)
{
    BadgeObjRecord *record = check_record(L, 1);
    const lua_Integer requested = luaL_checkinteger(L, 2);
    if (requested < 1 || requested > INT32_MAX) {
        return luaL_error(L, "child index must be a positive integer");
    }
    lv_obj_t *child = lv_obj_get_child(record->obj, static_cast<int32_t>(requested - 1));
    if (!child) {
        lua_pushnil(L);
        return 1;
    }
    if (!badge_lua_obj_push(L, child, BADGE_OBJ_FOREIGN)) {
        return luaL_error(L, "failed to allocate child handle");
    }
    return 1;
}

int obj_child_count(lua_State *L)
{
    BadgeObjRecord *record = check_record(L, 1);
    lua_pushinteger(L, static_cast<lua_Integer>(lv_obj_get_child_count(record->obj)));
    return 1;
}

int obj_hidden(lua_State *L)
{
    BadgeObjRecord *record = badge_lua_obj_check_mutable_record(L, 1);
    if (lua_isnoneornil(L, 2)) {
        lua_pushboolean(L, lv_obj_has_flag(record->obj, LV_OBJ_FLAG_HIDDEN));
        return 1;
    }
    luaL_checktype(L, 2, LUA_TBOOLEAN);
    if (lua_toboolean(L, 2)) lv_obj_add_flag(record->obj, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_remove_flag(record->obj, LV_OBJ_FLAG_HIDDEN);
    return 0;
}

int obj_clickable(lua_State *L)
{
    BadgeObjRecord *record = badge_lua_obj_check_mutable_record(L, 1);
    if (lua_isnoneornil(L, 2)) {
        lua_pushboolean(L, lv_obj_has_flag(record->obj, LV_OBJ_FLAG_CLICKABLE));
        return 1;
    }
    luaL_checktype(L, 2, LUA_TBOOLEAN);
    if (lua_toboolean(L, 2)) lv_obj_add_flag(record->obj, LV_OBJ_FLAG_CLICKABLE);
    else lv_obj_remove_flag(record->obj, LV_OBJ_FLAG_CLICKABLE);
    return 0;
}

int obj_bring_to_front(lua_State *L)
{
    BadgeObjRecord *record = badge_lua_obj_check_mutable_record(L, 1);
    lv_obj_move_to_index(record->obj, -1);
    return 0;
}

} // namespace

void badge_lua_store_ctx(lua_State *L, LuaCtx *ctx)
{
    lua_pushlightuserdata(L, ctx);
    lua_rawsetp(L, LUA_REGISTRYINDEX, &kCtxRegistryKey);
}

LuaCtx *badge_lua_get_ctx(lua_State *L)
{
    lua_rawgetp(L, LUA_REGISTRYINDEX, &kCtxRegistryKey);
    LuaCtx *ctx = static_cast<LuaCtx *>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    if (!ctx || ctx->L != L || !ctx->state_live) return nullptr;
    return ctx;
}

BadgeObjRecord *badge_lua_obj_check_record(lua_State *L, int index)
{
    return check_record(L, index);
}

BadgeObjRecord *badge_lua_obj_check_mutable_record(lua_State *L, int index)
{
    BadgeObjRecord *record = check_record(L, index);
    if (record->flags & BADGE_OBJ_ROOT) {
        luaL_error(L, "root widget is read-only");
        return nullptr;
    }
    return record;
}

void badge_lua_obj_check_object_budget(lua_State *L, LuaCtx *ctx)
{
    if (!ctx || ctx->live_object_count >= kMaxLiveObjects) {
        luaL_error(L, "too many widgets (512); delete some or reuse them");
    }
}

const char *badge_lua_obj_type_name(lv_obj_t *obj)
{
    if (!obj) return "deleted";
    if (lv_obj_has_class(obj, &lv_slider_class)) return "slider";
    if (lv_obj_has_class(obj, &lv_bar_class)) return "bar";
    if (lv_obj_has_class(obj, &lv_arc_class)) return "arc";
    if (lv_obj_has_class(obj, &lv_button_class)) return "button";
    if (lv_obj_has_class(obj, &lv_switch_class)) return "switch";
    if (lv_obj_has_class(obj, &lv_checkbox_class)) return "checkbox";
    if (lv_obj_has_class(obj, &lv_roller_class)) return "roller";
    if (lv_obj_has_class(obj, &lv_textarea_class)) return "textarea";
    if (lv_obj_has_class(obj, &lv_image_class)) return "image";
    if (lv_obj_has_class(obj, &lv_line_class)) return "line";
    if (lv_obj_has_class(obj, &lv_label_class)) return "label";
    return "obj";
}

bool badge_lua_obj_parse_align(const char *name, lv_align_t *align)
{
    return name && align && parse_align(name, align);
}

void badge_lua_obj_set_text(lua_State *L, BadgeObjRecord *record, int value_index)
{
    if (!is_text_widget(record->obj)) {
        luaL_error(L, "set_text expects a label, textarea, or checkbox, got %s",
                   badge_lua_obj_type_name(record->obj));
    }
    std::size_t length = 0;
    const char *text = check_bounded_string(L, value_index, "text", &length);
    if (lv_obj_has_class(record->obj, &lv_label_class)) {
        lv_label_set_text(record->obj, text);
    } else if (lv_obj_has_class(record->obj, &lv_textarea_class)) {
        lv_textarea_set_text(record->obj, text);
    } else {
        lv_checkbox_set_text(record->obj, text);
    }
}

void badge_lua_obj_set_value(lua_State *L, BadgeObjRecord *record, int value_index)
{
    if (!is_value_widget(record->obj)) {
        luaL_error(L, "set_value expects a bar, arc, or slider, got %s",
                   badge_lua_obj_type_name(record->obj));
    }
    const int32_t value = check_int32(L, value_index, "value");
    if (lv_obj_has_class(record->obj, &lv_slider_class)) {
        lv_slider_set_value(record->obj, value, LV_ANIM_OFF);
    } else if (lv_obj_has_class(record->obj, &lv_bar_class)) {
        lv_bar_set_value(record->obj, value, LV_ANIM_OFF);
    } else {
        lv_arc_set_value(record->obj, value);
    }
}

void badge_lua_obj_set_range(lua_State *L, BadgeObjRecord *record, int min_index,
                             int max_index)
{
    if (!is_value_widget(record->obj)) {
        luaL_error(L, "set_range expects a bar, arc, or slider, got %s",
                   badge_lua_obj_type_name(record->obj));
    }
    const int32_t minimum = check_int32(L, min_index, "minimum");
    const int32_t maximum = check_int32(L, max_index, "maximum");
    if (minimum > maximum) luaL_error(L, "minimum must not exceed maximum");
    if (lv_obj_has_class(record->obj, &lv_slider_class)) {
        lv_slider_set_range(record->obj, minimum, maximum);
    } else if (lv_obj_has_class(record->obj, &lv_bar_class)) {
        lv_bar_set_range(record->obj, minimum, maximum);
    } else {
        lv_arc_set_range(record->obj, minimum, maximum);
    }
}

namespace {

constexpr std::size_t kImagePathBytes = 512;

void build_image_path(lua_State *L, LuaCtx *ctx, int value_index,
                      char *path, std::size_t path_size)
{
    if (!ctx || !ctx->dir) luaL_error(L, "Lua app context is unavailable");

    std::size_t length = 0;
    const char *source = check_bounded_string(L, value_index, "src", &length);
    if (length == 0 || length > 255
        || length < 4 || std::memcmp(source + length - 4, ".bin", 4) != 0) {
        luaL_error(L, "src must name a .bin file in this app");
    }
    if (source[0] == '/' || std::strchr(source, '\\') || std::strchr(source, ':')) {
        luaL_error(L, "src must be a relative path in this app");
    }
    std::size_t segment_start = 0;
    for (std::size_t i = 0; i <= length; ++i) {
        if (i != length && source[i] != '/') continue;
        const std::size_t segment_length = i - segment_start;
        if (segment_length == 0
            || (segment_length == 1 && source[segment_start] == '.')
            || (segment_length == 2 && source[segment_start] == '.'
                && source[segment_start + 1] == '.')) {
            luaL_error(L, "src must not contain a path traversal");
        }
        segment_start = i + 1;
    }

    const int written = std::snprintf(path, path_size, "%s/%s", ctx->dir, source);
    if (written < 0 || static_cast<std::size_t>(written) >= path_size) {
        luaL_error(L, "src path is too long");
    }
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
        luaL_error(L, "src must name an installed regular .bin file");
    }
}

void copy_point_coordinate(lua_State *L, int point_index, const char *name,
                           int positional_index, lv_value_precise_t *out)
{
    point_index = lua_absindex(L, point_index);
    lua_getfield(L, point_index, name);
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        lua_rawgeti(L, point_index, positional_index);
    }
    *out = static_cast<lv_value_precise_t>(check_coordinate(L, -1, name));
    lua_pop(L, 1);
}

} // namespace

void badge_lua_obj_validate_image_src(lua_State *L, LuaCtx *ctx, int value_index)
{
    char path[kImagePathBytes];
    build_image_path(L, ctx, value_index, path, sizeof(path));
}

void badge_lua_obj_set_src(lua_State *L, BadgeObjRecord *record, int value_index)
{
    if (!lv_obj_has_class(record->obj, &lv_image_class)) {
        luaL_error(L, "set_src expects an image, got %s",
                   badge_lua_obj_type_name(record->obj));
    }
    char path[kImagePathBytes];
    build_image_path(L, record->ctx, value_index, path, sizeof(path));
    char source[kImagePathBytes + 3];
    const int written = std::snprintf(source, sizeof(source), "A:%s", path);
    if (written < 0 || static_cast<std::size_t>(written) >= sizeof(source)) {
        luaL_error(L, "src path is too long");
    }
    lv_image_set_src(record->obj, source);
}

lv_point_precise_t *badge_lua_obj_copy_points(lua_State *L, int value_index,
                                              std::uint32_t *point_count)
{
    constexpr std::uint32_t kMaxPoints = 128;
    luaL_checktype(L, value_index, LUA_TTABLE);
    const std::size_t count = lua_rawlen(L, value_index);
    if (count > kMaxPoints) luaL_error(L, "points may contain at most 128 points");

    lv_point_precise_t staged[kMaxPoints];
    for (std::size_t i = 0; i < count; ++i) {
        lua_rawgeti(L, value_index, static_cast<lua_Integer>(i + 1));
        if (!lua_istable(L, -1)) {
            luaL_error(L, "points[%d] must be a table", static_cast<int>(i + 1));
        }
        copy_point_coordinate(L, -1, "x", 1, &staged[i].x);
        copy_point_coordinate(L, -1, "y", 2, &staged[i].y);
        lua_pop(L, 1);
    }

    *point_count = static_cast<std::uint32_t>(count);
    if (count == 0) return nullptr;
    lv_point_precise_t *points = static_cast<lv_point_precise_t *>(
        std::malloc(count * sizeof(lv_point_precise_t)));
    if (!points) {
        luaL_error(L, "failed to allocate line points");
    }
    std::memcpy(points, staged, count * sizeof(lv_point_precise_t));
    return points;
}

void badge_lua_obj_set_native_points(BadgeObjRecord *record,
                                     lv_point_precise_t *points,
                                     std::uint32_t point_count)
{
    lv_line_set_points(record->obj, points, point_count);
    free_aux(record);
    record->aux = points;
    record->aux_deleter = std::free;
}

void badge_lua_obj_set_checked(lua_State *L, BadgeObjRecord *record, int value_index)
{
    if (!is_checked_widget(record->obj)) {
        luaL_error(L, "set_checked expects a switch or checkbox, got %s",
                   badge_lua_obj_type_name(record->obj));
    }
    luaL_checktype(L, value_index, LUA_TBOOLEAN);
    if (lua_toboolean(L, value_index)) lv_obj_add_state(record->obj, LV_STATE_CHECKED);
    else lv_obj_remove_state(record->obj, LV_STATE_CHECKED);
}

void badge_lua_obj_set_options(lua_State *L, BadgeObjRecord *record, int value_index)
{
    if (!lv_obj_has_class(record->obj, &lv_roller_class)) {
        luaL_error(L, "set_options expects a roller, got %s",
                   badge_lua_obj_type_name(record->obj));
    }
    std::size_t length = 0;
    const char *options = check_bounded_string(L, value_index, "options", &length);
    lv_roller_set_options(record->obj, options, LV_ROLLER_MODE_NORMAL);
}

void badge_lua_obj_install(lua_State *L)
{
    lua_newtable(L);
    lua_rawsetp(L, LUA_REGISTRYINDEX, &kRecordMapRegistryKey);

    lua_newtable(L);
    lua_newtable(L);
    lua_pushliteral(L, "v");
    lua_setfield(L, -2, "__mode");
    lua_setmetatable(L, -2);
    lua_rawsetp(L, LUA_REGISTRYINDEX, &kUserdataMapRegistryKey);

    static const luaL_Reg methods[] = {
        { "type",           obj_type           },
        { "set_pos",        obj_set_pos        },
        { "set_size",       obj_set_size       },
        { "align",          obj_align          },
        { "parent",         obj_parent         },
        { "child",          obj_child          },
        { "child_count",    obj_child_count    },
        { "hidden",         obj_hidden         },
        { "clickable",      obj_clickable      },
        { "bring_to_front", obj_bring_to_front },
        { "set_color",      obj_set_color      },
        { "set_border",     obj_set_border     },
        { "set_text",       obj_set_text       },
        { "set_value",      obj_set_value      },
        { "set_range",      obj_set_range      },
        { "set_src",        obj_set_src        },
        { "set_points",     obj_set_points     },
        { "set_checked",    obj_set_checked    },
        { "get_checked",    obj_get_checked    },
        { "set_options",    obj_set_options    },
        { "get_selected",   obj_get_selected   },
        { "set_font_size",  obj_set_font_size  },
        { "delete",         obj_delete         },
        { nullptr, nullptr },
    };

    luaL_newmetatable(L, kObjMetatable);
    lua_pushcfunction(L, obj_gc);
    lua_setfield(L, -2, "__gc");
    lua_pushcfunction(L, obj_eq);
    lua_setfield(L, -2, "__eq");
    lua_pushcfunction(L, obj_tostring);
    lua_setfield(L, -2, "__tostring");
    lua_pushliteral(L, "badge.obj");
    lua_setfield(L, -2, "__metatable");
    lua_newtable(L);
    luaL_setfuncs(L, methods, 0);
    badge_lua_style_add_object_methods(L, -1);
    lua_setfield(L, -2, "__index");
    lua_pop(L, 1);
}

bool badge_lua_obj_push(lua_State *L, lv_obj_t *obj, std::uint32_t flags)
{
    LuaCtx *ctx = badge_lua_get_ctx(L);
    if (!ctx || !obj) return false;

    BadgeObjRecord *record = record_from_map(L, ctx, obj);
    if (!record) {
        record = record_create(L, ctx, obj, flags);
        if (!record) return false;
    }

    if (push_existing_userdata(L, obj, record)) return true;

    BadgeObj *ud = static_cast<BadgeObj *>(lua_newuserdatauv(L, sizeof(BadgeObj), 0));
    ud->record = record;
    ++record->refcount;
    luaL_setmetatable(L, kObjMetatable);
    store_userdata(L, obj, -1);
    return true;
}

void badge_lua_obj_detach_root(LuaCtx *ctx)
{
    if (!ctx) return;

    BadgeObjRecord *record = ctx->root_record;
    if (!record) {
        ctx->root = nullptr;
        return;
    }

    lv_obj_t *obj = record->obj;
    if (!obj) {
        ctx->root_record = nullptr;
        ctx->root = nullptr;
        return;
    }

    // The display root is borrowed, so detach our callback instead of deleting
    // it. Invalidation then follows the same ref/map accounting as deletion.
    lv_obj_remove_event_cb_with_user_data(obj, obj_delete_event, record);
    invalidate_record(record, obj);
}
