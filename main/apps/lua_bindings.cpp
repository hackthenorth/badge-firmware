#include "lua_bindings.h"
#include "lua_internal.h"

extern "C" {
#include "lua.h"
#include "lauxlib.h"
}

#include "hal_buttons.h"
#include "hal_config.h"

#include "esp_random.h"

#include <cstdint>
#include <cstring>

namespace {

LuaCtx *check_ctx(lua_State *L)
{
    LuaCtx *ctx = badge_lua_get_ctx(L);
    if (!ctx) {
        luaL_error(L, "Lua app context is unavailable");
        return nullptr;
    }
    return ctx;
}

int badge_config_get(lua_State *L)
{
    LuaCtx *ctx = check_ctx(L);
    const char *slug = luaL_checkstring(L, 1);
    const char *key = luaL_checkstring(L, 2);
    int def = static_cast<int>(luaL_optinteger(L, 3, 0));
    if (std::strcmp(slug, ctx->slug) != 0) {
        return luaL_error(L, "config slug must match app slug '%s'", ctx->slug);
    }
    lua_pushinteger(L, hal_config_get_int(slug, key, def));
    return 1;
}

int badge_config_set(lua_State *L)
{
    LuaCtx *ctx = check_ctx(L);
    const char *slug = luaL_checkstring(L, 1);
    const char *key = luaL_checkstring(L, 2);
    int value = static_cast<int>(luaL_checkinteger(L, 3));
    if (std::strcmp(slug, ctx->slug) != 0) {
        return luaL_error(L, "config slug must match app slug '%s'", ctx->slug);
    }
    hal_config_set_int(slug, key, value);
    return 0;
}

int badge_random(lua_State *L)
{
    lua_Integer max = luaL_optinteger(L, 1, 0);
    std::uint32_t random = esp_random();
    if (max > 0) {
        lua_pushinteger(L, static_cast<lua_Integer>(random % static_cast<std::uint32_t>(max)));
    } else {
        lua_pushinteger(L, static_cast<lua_Integer>(random));
    }
    return 1;
}

int badge_gesture_shake(lua_State *L)
{
    return badge_lua_hw_sensor_shake(L);
}

int badge_gesture_tap(lua_State *L)
{
    return badge_lua_hw_sensor_tap(L);
}

void register_button_consts(lua_State *L)
{
    lua_newtable(L);
    lua_pushinteger(L, static_cast<lua_Integer>(Button::A));     lua_setfield(L, -2, "A");
    lua_pushinteger(L, static_cast<lua_Integer>(Button::B));     lua_setfield(L, -2, "B");
    lua_pushinteger(L, static_cast<lua_Integer>(Button::Home));  lua_setfield(L, -2, "HOME");
    lua_pushinteger(L, static_cast<lua_Integer>(Button::Down));  lua_setfield(L, -2, "DOWN");
    lua_pushinteger(L, static_cast<lua_Integer>(Button::Left));  lua_setfield(L, -2, "LEFT");
    lua_pushinteger(L, static_cast<lua_Integer>(Button::Right)); lua_setfield(L, -2, "RIGHT");
    lua_pushinteger(L, static_cast<lua_Integer>(Button::Up));    lua_setfield(L, -2, "UP");
    lua_pushinteger(L, static_cast<lua_Integer>(Button::Aux1));  lua_setfield(L, -2, "AUX1");
    lua_pushinteger(L, static_cast<lua_Integer>(Button::Start)); lua_setfield(L, -2, "START");
    lua_setfield(L, -2, "button");

    lua_newtable(L);
    lua_pushinteger(L, static_cast<lua_Integer>(ButtonEventKind::Pressed));
    lua_setfield(L, -2, "PRESSED");
    lua_pushinteger(L, static_cast<lua_Integer>(ButtonEventKind::Released));
    lua_setfield(L, -2, "RELEASED");
    lua_setfield(L, -2, "kind");
}

} // namespace

void badge_lua_install(lua_State *L, LuaCtx *ctx)
{
    badge_lua_store_ctx(L, ctx);
    badge_lua_obj_install(L);

    static const luaL_Reg functions[] = {
        { "config_get",    badge_config_get    },
        { "config_set",    badge_config_set    },
        { "random",        badge_random        },
        { "gesture_shake", badge_gesture_shake },
        { "gesture_tap",   badge_gesture_tap   },
        { nullptr, nullptr },
    };

    lua_newtable(L);
    luaL_setfuncs(L, functions, 0);
    register_button_consts(L);
    badge_lua_style_install_theme(L, -1);
    badge_lua_ui_install(L, -1);
    if (ctx->api == 1) badge_lua_ui_install_legacy(L, -1);
    badge_lua_hw_install(L, ctx, -1);
    badge_lua_sys_install(L, -1);
    badge_lua_fs_install(L, ctx, -1);
    badge_lua_require_install(L, ctx);
    badge_lua_nfc_install(L, -1);
    badge_lua_radio_install(L, ctx, -1);
    lua_setglobal(L, "badge");
}

bool badge_lua_push_root(lua_State *L)
{
    LuaCtx *ctx = badge_lua_get_ctx(L);
    if (!ctx || !ctx->root) return false;
    return badge_lua_obj_push(L, ctx->root, BADGE_OBJ_ROOT | BADGE_OBJ_FOREIGN);
}

void badge_lua_detach_root(LuaCtx *ctx)
{
    badge_lua_obj_detach_root(ctx);
}
