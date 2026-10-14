#include "lua_internal.h"

extern "C" {
#include "lauxlib.h"
}

#include "contacts_store.h"
#include "hal_config.h"
#include "hal_identity.h"
#include "hal_sleep.h"

#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"

#include <cstdio>
#include <cstring>

namespace {

constexpr std::size_t kMaxKeyBytes = 24;
constexpr std::size_t kMaxStrValueBytes = 128;
constexpr std::size_t kMaxStoreKeys = 32;

LuaCtx *check_ctx(lua_State *L)
{
    LuaCtx *ctx = badge_lua_get_ctx(L);
    if (!ctx) luaL_error(L, "Lua app context is unavailable");
    return ctx;
}

void check_arg_count(lua_State *L, int expected, const char *name)
{
    if (lua_gettop(L) != expected) {
        luaL_error(L, "%s expects %d argument%s", name, expected,
                   expected == 1 ? "" : "s");
    }
}

const char *check_key(lua_State *L, int index)
{
    const char *key = luaL_checkstring(L, index);
    const std::size_t len = std::strlen(key);
    if (len == 0 || len > kMaxKeyBytes) {
        luaL_error(L, "key must be 1-%d bytes", (int)kMaxKeyBytes);
    }
    for (std::size_t i = 0; i < len; ++i) {
        const char c = key[i];
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
            || (c >= '0' && c <= '9') || c == '_';
        if (!ok) luaL_error(L, "key must match [A-Za-z0-9_]+");
    }
    return key;
}

const char *check_str_value(lua_State *L, int index)
{
    const char *value = luaL_checkstring(L, index);
    if (std::strlen(value) > kMaxStrValueBytes) {
        luaL_error(L, "string value must be at most %d bytes",
                   (int)kMaxStrValueBytes);
    }
    if (std::strchr(value, '\n') || std::strchr(value, '\r')) {
        luaL_error(L, "string value must not contain line breaks");
    }
    return value;
}

// Slugs are manifest-limited to 32 bytes, but the compiler can't see that;
// taking the size as a runtime parameter keeps -Wformat-truncation quiet,
// the same shape as hal_config's path_for.
void store_path(char *out, std::size_t out_sz, const char *slug)
{
    std::snprintf(out, out_sz, "/littlefs/config/%s.cfg", slug);
}

// Counts keys in this app's own config file. The file is line-oriented
// key=value, so a key is any non-empty line containing '='.
std::size_t store_key_count(const char *slug)
{
    char path[64];
    store_path(path, sizeof(path), slug);
    FILE *f = std::fopen(path, "r");
    if (!f) return 0;
    std::size_t count = 0;
    char line[256];
    while (std::fgets(line, sizeof(line), f)) {
        if (line[0] != '\0' && line[0] != '\n' && std::strchr(line, '=')) ++count;
    }
    std::fclose(f);
    return count;
}

bool store_has_key(const char *slug, const char *key)
{
    char path[64];
    store_path(path, sizeof(path), slug);
    FILE *f = std::fopen(path, "r");
    if (!f) return false;
    const std::size_t klen = std::strlen(key);
    bool found = false;
    char line[256];
    while (std::fgets(line, sizeof(line), f)) {
        if (std::strncmp(line, key, klen) == 0 && line[klen] == '=') {
            found = true;
            break;
        }
    }
    std::fclose(f);
    return found;
}

void store_enforce_quota(lua_State *L, LuaCtx *ctx, const char *key)
{
    if (store_has_key(ctx->slug, key)) return;
    if (store_key_count(ctx->slug) >= kMaxStoreKeys) {
        luaL_error(L, "store quota exceeded (%d keys); remove one first",
                   (int)kMaxStoreKeys);
    }
}

// ---- badge.sys ----

int sys_ms(lua_State *L)
{
    check_arg_count(L, 0, "badge.sys.ms");
    lua_pushinteger(L, static_cast<lua_Integer>(esp_timer_get_time() / 1000));
    return 1;
}

int sys_uptime(lua_State *L)
{
    check_arg_count(L, 0, "badge.sys.uptime");
    lua_pushinteger(L, static_cast<lua_Integer>(esp_timer_get_time() / 1000000));
    return 1;
}

int sys_log(lua_State *L)
{
    check_arg_count(L, 1, "badge.sys.log");
    LuaCtx *ctx = check_ctx(L);
    ESP_LOGI("lua", "[%s] %s", ctx->slug, luaL_checkstring(L, 1));
    return 0;
}

int sys_random(lua_State *L)
{
    const int tops = lua_gettop(L);
    if (tops > 1) luaL_error(L, "badge.sys.random expects 0 or 1 arguments");
    std::uint32_t random = esp_random();
    if (tops == 1) {
        const lua_Integer max = luaL_checkinteger(L, 1);
        if (max <= 0) luaL_error(L, "max must be positive");
        lua_pushinteger(L, static_cast<lua_Integer>(
                                 random % static_cast<std::uint32_t>(max)));
    } else {
        lua_pushinteger(L, static_cast<lua_Integer>(random));
    }
    return 1;
}

int sys_heap(lua_State *L)
{
    check_arg_count(L, 0, "badge.sys.heap");
    LuaCtx *ctx = check_ctx(L);
    lua_pushinteger(L, static_cast<lua_Integer>(ctx->heap_current));
    return 1;
}

int sys_gc_step(lua_State *L)
{
    check_arg_count(L, 0, "badge.sys.gc_step");
    lua_gc(L, LUA_GCSTEP);
    return 0;
}

int sys_version(lua_State *L)
{
    check_arg_count(L, 0, "badge.sys.version");
    lua_pushstring(L, esp_app_get_description()->version);
    return 1;
}

int sys_wake_lock(lua_State *L)
{
    check_arg_count(L, 1, "badge.sys.wake_lock");
    luaL_checktype(L, 1, LUA_TBOOLEAN);
    hal_sleep_wake_lock(lua_toboolean(L, 1) != 0);
    return 0;
}

int sys_stats(lua_State *L)
{
    check_arg_count(L, 0, "badge.sys.stats");
    LuaCtx *ctx = check_ctx(L);
    lua_newtable(L);
    lua_pushinteger(L, static_cast<lua_Integer>(ctx->heap_current));
    lua_setfield(L, -2, "lua_used");
    lua_pushinteger(L, static_cast<lua_Integer>(ctx->heap_peak));
    lua_setfield(L, -2, "lua_peak");
    lua_pushinteger(L, static_cast<lua_Integer>(ctx->heap_limit));
    lua_setfield(L, -2, "lua_limit");
    lua_pushinteger(L, static_cast<lua_Integer>(ctx->live_object_count));
    lua_setfield(L, -2, "widgets");
    lua_pushinteger(L, static_cast<lua_Integer>(esp_timer_get_time() / 1000));
    lua_setfield(L, -2, "uptime_ms");
    lua_pushinteger(L, static_cast<lua_Integer>(esp_get_free_heap_size()));
    lua_setfield(L, -2, "free_heap");
    return 1;
}

// ---- badge.store ----

int store_get_int(lua_State *L)
{
    check_arg_count(L, 2, "badge.store.get_int");
    LuaCtx *ctx = check_ctx(L);
    const char *key = check_key(L, 1);
    const int def = static_cast<int>(luaL_checkinteger(L, 2));
    lua_pushinteger(L, hal_config_get_int(ctx->slug, key, def));
    return 1;
}

int store_set_int(lua_State *L)
{
    check_arg_count(L, 2, "badge.store.set_int");
    LuaCtx *ctx = check_ctx(L);
    const char *key = check_key(L, 1);
    const int value = static_cast<int>(luaL_checkinteger(L, 2));
    store_enforce_quota(L, ctx, key);
    hal_config_set_int(ctx->slug, key, value);
    return 0;
}

int store_get_str(lua_State *L)
{
    check_arg_count(L, 2, "badge.store.get_str");
    LuaCtx *ctx = check_ctx(L);
    const char *key = check_key(L, 1);
    const char *def = luaL_checkstring(L, 2);
    char out[kMaxStrValueBytes + 1];
    if (hal_config_get_string(ctx->slug, key, out, sizeof(out), nullptr)) {
        lua_pushstring(L, out);
    } else {
        lua_pushstring(L, def);
    }
    return 1;
}

int store_set_str(lua_State *L)
{
    check_arg_count(L, 2, "badge.store.set_str");
    LuaCtx *ctx = check_ctx(L);
    const char *key = check_key(L, 1);
    const char *value = check_str_value(L, 2);
    store_enforce_quota(L, ctx, key);
    hal_config_set_string(ctx->slug, key, value);
    return 0;
}

int store_get(lua_State *L)
{
    if (lua_gettop(L) != 2) luaL_error(L, "badge.store.get expects 2 arguments");
    LuaCtx *ctx = check_ctx(L);
    const char *key = check_key(L, 1);
    if (lua_type(L, 2) == LUA_TNUMBER) {
        const int def = static_cast<int>(luaL_checkinteger(L, 2));
        lua_pushinteger(L, hal_config_get_int(ctx->slug, key, def));
    } else if (lua_type(L, 2) == LUA_TSTRING) {
        const char *def = lua_tostring(L, 2);
        char out[kMaxStrValueBytes + 1];
        if (hal_config_get_string(ctx->slug, key, out, sizeof(out), nullptr)) {
            lua_pushstring(L, out);
        } else {
            lua_pushstring(L, def);
        }
    } else {
        luaL_error(L, "default must be an integer or a string");
    }
    return 1;
}

int store_set(lua_State *L)
{
    if (lua_gettop(L) != 2) luaL_error(L, "badge.store.set expects 2 arguments");
    LuaCtx *ctx = check_ctx(L);
    const char *key = check_key(L, 1);
    store_enforce_quota(L, ctx, key);
    if (lua_type(L, 2) == LUA_TNUMBER) {
        hal_config_set_int(ctx->slug, key, static_cast<int>(luaL_checkinteger(L, 2)));
    } else if (lua_type(L, 2) == LUA_TSTRING) {
        hal_config_set_string(ctx->slug, key, check_str_value(L, 2));
    } else {
        luaL_error(L, "value must be an integer or a string");
    }
    return 0;
}

// ---- badge.me ----

int me_name(lua_State *L)
{
    check_arg_count(L, 0, "badge.me.name");
    const BadgeIdentity *id = hal_identity_get();
    if (id->provisioned) lua_pushstring(L, id->display_name);
    else lua_pushstring(L, "unprovisioned");
    return 1;
}

int me_role(lua_State *L)
{
    check_arg_count(L, 0, "badge.me.role");
    lua_pushinteger(L, hal_identity_get()->role);
    return 1;
}

int me_role_name(lua_State *L)
{
    check_arg_count(L, 0, "badge.me.role_name");
    lua_pushstring(L, hal_identity_role_name(hal_identity_get()->role));
    return 1;
}

int me_color(lua_State *L)
{
    check_arg_count(L, 0, "badge.me.color");
    const BadgeIdentity *id = hal_identity_get();
    lua_pushinteger(L, id->role_color[0]);
    lua_pushinteger(L, id->role_color[1]);
    lua_pushinteger(L, id->role_color[2]);
    return 3;
}

int me_badge_id(lua_State *L)
{
    check_arg_count(L, 0, "badge.me.badge_id");
    const BadgeIdentity *id = hal_identity_get();
    if (id->provisioned) lua_pushstring(L, id->badge_id);
    else lua_pushnil(L);
    return 1;
}

int me_provisioned(lua_State *L)
{
    check_arg_count(L, 0, "badge.me.provisioned");
    lua_pushboolean(L, hal_identity_is_provisioned() ? 1 : 0);
    return 1;
}

// ---- badge.contacts ----

int contacts_count(lua_State *L)
{
    check_arg_count(L, 0, "badge.contacts.count");
    lua_pushinteger(L, static_cast<lua_Integer>(contacts_store_count()));
    return 1;
}

int contacts_get(lua_State *L)
{
    check_arg_count(L, 1, "badge.contacts.get");
    const lua_Integer index = luaL_checkinteger(L, 1);
    if (index < 1) luaL_error(L, "index must be at least 1");
    ContactRecord record = {};
    if (!contacts_store_get(static_cast<std::size_t>(index - 1), &record)) {
        lua_pushnil(L);
        lua_pushstring(L, "no contact at that index");
        return 2;
    }
    lua_newtable(L);
    lua_pushstring(L, record.name);
    lua_setfield(L, -2, "name");
    lua_pushinteger(L, record.role);
    lua_setfield(L, -2, "role");
    lua_pushstring(L, record.badge_id);
    lua_setfield(L, -2, "badge_id");
    lua_pushinteger(L, static_cast<lua_Integer>(record.received_unix));
    lua_setfield(L, -2, "received_unix");
    return 1;
}

// ---- badge.app ----

int app_slug(lua_State *L)
{
    check_arg_count(L, 0, "badge.app.slug");
    lua_pushstring(L, check_ctx(L)->slug);
    return 1;
}

int app_name(lua_State *L)
{
    check_arg_count(L, 0, "badge.app.name");
    lua_pushstring(L, check_ctx(L)->app_name);
    return 1;
}

int app_exit(lua_State *L)
{
    check_arg_count(L, 0, "badge.app.exit");
    // Deferred: closing the state here would free the lua_pcall still on
    // the C stack. ScriptApp returns to the launcher after the call lands.
    check_ctx(L)->pending_exit = true;
    return 0;
}

} // namespace

void badge_lua_sys_install(lua_State *L, int badge_index)
{
    // See badge_lua_hw_install: a relative badge_index would retarget every
    // lua_setfield below at the freshly pushed table instead of badge.
    badge_index = lua_absindex(L, badge_index);
    static const luaL_Reg sys_functions[] = {
        { "ms",        sys_ms        },
        { "uptime",    sys_uptime    },
        { "log",       sys_log       },
        { "random",    sys_random    },
        { "heap",      sys_heap      },
        { "gc_step",   sys_gc_step   },
        { "version",   sys_version   },
        { "wake_lock", sys_wake_lock },
        { "stats",     sys_stats     },
        { nullptr, nullptr },
    };
    lua_newtable(L);
    luaL_setfuncs(L, sys_functions, 0);
    lua_setfield(L, badge_index, "sys");

    static const luaL_Reg store_functions[] = {
        { "get",     store_get     },
        { "set",     store_set     },
        { "get_int", store_get_int },
        { "set_int", store_set_int },
        { "get_str", store_get_str },
        { "set_str", store_set_str },
        { nullptr, nullptr },
    };
    lua_newtable(L);
    luaL_setfuncs(L, store_functions, 0);
    lua_setfield(L, badge_index, "store");

    static const luaL_Reg me_functions[] = {
        { "name",        me_name        },
        { "role",        me_role        },
        { "role_name",   me_role_name   },
        { "color",       me_color       },
        { "badge_id",    me_badge_id    },
        { "provisioned", me_provisioned },
        { nullptr, nullptr },
    };
    lua_newtable(L);
    luaL_setfuncs(L, me_functions, 0);
    lua_setfield(L, badge_index, "me");

    static const luaL_Reg contacts_functions[] = {
        { "count", contacts_count },
        { "get",   contacts_get   },
        { nullptr, nullptr },
    };
    lua_newtable(L);
    luaL_setfuncs(L, contacts_functions, 0);
    lua_setfield(L, badge_index, "contacts");

    static const luaL_Reg app_functions[] = {
        { "slug", app_slug },
        { "name", app_name },
        { "exit", app_exit },
        { nullptr, nullptr },
    };
    lua_newtable(L);
    luaL_setfuncs(L, app_functions, 0);
    lua_setfield(L, badge_index, "app");
}
