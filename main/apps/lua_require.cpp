#include "lua_internal.h"

extern "C" {
#include "lauxlib.h"
}

#include <cstdio>
#include <cstring>
#include <string>

namespace {

constexpr std::size_t kMaxModuleBytes = 48;
constexpr std::uint8_t kMaxDepth = 8;
constexpr std::uint8_t kMaxModules = 16;

// One charset rule kills /, \, .., absolute paths and NUL truncation
// outright: [A-Za-z0-9_] segments separated by '.', so there is no path
// normalization to get wrong.
bool valid_module_name(const char *name, std::size_t len)
{
    if (len == 0 || len > kMaxModuleBytes) return false;
    if (memchr(name, '\0', len) != nullptr) return false;
    bool need_segment = true;
    for (std::size_t i = 0; i < len; ++i) {
        const char c = name[i];
        if (c == '.') {
            if (need_segment) return false;
            need_segment = true;
            continue;
        }
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
            || (c >= '0' && c <= '9') || c == '_';
        if (!ok) return false;
        need_segment = false;
    }
    return !need_segment;
}

int lua_require(lua_State *L)
{
    LuaCtx *ctx = badge_lua_get_ctx(L);
    if (!ctx) return luaL_error(L, "Lua app context is unavailable");
    std::size_t len = 0;
    const char *name = luaL_checklstring(L, 1, &len);
    if (lua_gettop(L) != 1) return luaL_error(L, "require expects 1 argument");
    if (!valid_module_name(name, len)) {
        return luaL_error(L, "invalid module name (want [A-Za-z0-9_], '.' separated)");
    }
    // Private module table: invisible to every other app and to main.lua's
    // globals, so a module cannot leak across scripts.
    lua_rawgeti(L, LUA_REGISTRYINDEX, ctx->require_cache_ref);
    lua_pushlstring(L, name, len);
    lua_rawget(L, -2);
    if (!lua_isnil(L, -1)) {
        // A lightuserdata sentinel marks in-progress loads; anything else
        // is a finished module (true included, for modules that return nil).
        if (lua_islightuserdata(L, -1)) {
            return luaL_error(L, "cyclic require of '%s'", name);
        }
        lua_remove(L, -2);  // module table, leaving the cached value on top
        return 1;
    }
    lua_pop(L, 1);
    // Caps apply to actual loads; cached modules return without touching them.
    if (ctx->require_depth >= kMaxDepth) {
        return luaL_error(L, "require depth exceeded (%d)", (int)kMaxDepth);
    }
    if (ctx->require_module_count >= kMaxModules) {
        return luaL_error(L, "too many modules (%d)", (int)kMaxModules);
    }

    std::string rel(name, len);
    for (char &c : rel) {
        if (c == '.') c = '/';
    }
    std::string path(ctx->dir);
    path += "/";
    path += rel;
    path += ".lua";

    if (luaL_loadfilex(L, path.c_str(), "t") != LUA_OK) {
        return lua_error(L);
    }
    // Sentinel before the call: a require cycle sees lightuserdata, not a
    // half-built module.
    static int sentinel = 0;
    lua_pushlstring(L, name, len);
    lua_pushlightuserdata(L, &sentinel);
    lua_rawset(L, -4);

    ++ctx->require_depth;
    const int call_status = lua_pcall(L, 0, 1, 0);
    --ctx->require_depth;
    if (call_status != LUA_OK) {
        // Clear the sentinel so a retry after a failed load re-reads the file.
        lua_pushlstring(L, name, len);
        lua_pushnil(L);
        lua_rawset(L, -4);
        lua_remove(L, -2);  // module table, leaving the error on top
        return lua_error(L);
    }
    if (lua_isnoneornil(L, -1)) {
        lua_pop(L, 1);
        lua_pushboolean(L, 1);
    }
    lua_pushlstring(L, name, len);
    lua_pushvalue(L, -2);
    lua_rawset(L, -4);
    lua_remove(L, -2);  // module table, leaving the value on top
    ++ctx->require_module_count;
    return 1;
}

} // namespace

void badge_lua_require_install(lua_State *L, LuaCtx *ctx)
{
    lua_newtable(L);
    ctx->require_cache_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    ctx->require_depth = 0;
    ctx->require_module_count = 0;
    lua_pushcfunction(L, lua_require);
    lua_setglobal(L, "require");
}
