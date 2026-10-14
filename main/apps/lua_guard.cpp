#include "lua_internal.h"

extern "C" {
#include "lauxlib.h"
}

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_timer.h"

#include <cstdlib>
#include <limits>

namespace {

constexpr std::size_t kTracebackReserveBytes = 4 * 1024;
constexpr int kHookInstructionCount = 2000;
constexpr UBaseType_t kMinimumFreeStackBytes = 4096;
constexpr const char *TAG = "lua_guard";

void disarm_deadline(lua_State *L, LuaCtx *ctx)
{
    lua_sethook(L, nullptr, 0, 0);
    if (ctx) ctx->deadline_armed = false;
}

void execution_hook(lua_State *L, lua_Debug *)
{
    LuaCtx *ctx = badge_lua_guard_ctx(L);
    if (!ctx || !ctx->deadline_armed) return;

    if (uxTaskGetStackHighWaterMark(nullptr) < kMinimumFreeStackBytes) {
        ctx->stack_guard_tripped = true;
        disarm_deadline(L, ctx);
        luaL_error(L, "Lua stack safety limit reached");
        return;
    }

    if (esp_timer_get_time() >= ctx->deadline_us) {
        ctx->timed_out = true;
        disarm_deadline(L, ctx);
        luaL_error(L, "Lua execution deadline exceeded");
    }
}

int panic_handler(lua_State *L)
{
    LuaCtx *ctx = badge_lua_guard_ctx(L);
    ESP_LOGE(TAG, "[%s] unprotected Lua panic",
             ctx && ctx->slug ? ctx->slug : "unknown");
    return 0;
}

void warning_handler(void *userdata, const char *message, int to_continue)
{
    LuaCtx *ctx = static_cast<LuaCtx *>(userdata);
    ESP_LOGW(TAG, "[%s] Lua warning%s: %s",
             ctx && ctx->slug ? ctx->slug : "unknown",
             to_continue ? " (continued)" : "", message ? message : "");
}

int traceback_handler(lua_State *L)
{
    LuaCtx *ctx = badge_lua_guard_ctx(L);
    if (ctx) {
        disarm_deadline(L, ctx);
        ctx->emergency_reserve_enabled = true;
        ctx->traceback_active = true;
    }

    const char *message = lua_type(L, 1) == LUA_TSTRING
        ? lua_tolstring(L, 1, nullptr)
        : "Lua error object is not a string";
    luaL_traceback(L, L, message, 1);

    if (ctx) ctx->traceback_active = false;
    return 1;
}

int protected_host_entry(lua_State *L)
{
    LuaCtx *ctx = badge_lua_guard_ctx(L);
    if (!ctx || !ctx->protected_function) {
        return luaL_error(L, "Lua protected host entry is unavailable");
    }
    return ctx->protected_function(L, ctx, ctx->protected_userdata);
}

int protected_dispatcher(lua_State *L)
{
    LuaCtx *ctx = badge_lua_guard_ctx(L);
    if (!ctx) return luaL_error(L, "Lua app context is unavailable");

    lua_pushcfunction(L, traceback_handler);
    lua_pushcfunction(L, protected_host_entry);
    int status = lua_pcall(L, 0, 0, 1);

    if (ctx->traceback_active) {
        ctx->traceback_active = false;
        ctx->traceback_failed = true;
    }
    ctx->emergency_reserve_enabled = false;
    ctx->protected_status = status;

    if (status != LUA_OK) {
        lua_remove(L, 1);
        return lua_error(L);
    }

    lua_pop(L, 1);
    return 0;
}

} // namespace

void *badge_lua_alloc(void *userdata, void *ptr, std::size_t osize,
                      std::size_t nsize)
{
    LuaCtx *ctx = static_cast<LuaCtx *>(userdata);
    if (!ctx) return nullptr;

    if (nsize == 0) {
        if (ptr) {
            // Lua supplies the true previous size for an existing block. Keep
            // accounting saturated if a broken allocator caller ever doesn't.
            ctx->heap_current = osize > ctx->heap_current
                ? 0 : ctx->heap_current - osize;
            std::free(ptr);
        }
        return nullptr;
    }

    const std::size_t old_size = ptr ? osize : 0;
    if (nsize > old_size) {
        const std::size_t growth = nsize - old_size;
        std::size_t limit = ctx->heap_limit;
        if (ctx->emergency_reserve_enabled) {
            limit = limit > std::numeric_limits<std::size_t>::max() - kTracebackReserveBytes
                ? std::numeric_limits<std::size_t>::max()
                : limit + kTracebackReserveBytes;
        }
        if (ctx->heap_current > limit || growth > limit - ctx->heap_current) {
            if (ctx->traceback_active) ctx->traceback_allocation_failed = true;
            return nullptr;
        }
    }

    void *new_ptr = std::realloc(ptr, nsize);
    if (!new_ptr) {
        if (ctx->traceback_active) ctx->traceback_allocation_failed = true;
        return nullptr;
    }

    if (nsize >= old_size) {
        ctx->heap_current += nsize - old_size;
        if (ctx->heap_current > ctx->heap_peak) {
            ctx->heap_peak = ctx->heap_current;
        }
    } else {
        const std::size_t shrink = old_size - nsize;
        ctx->heap_current = shrink > ctx->heap_current
            ? 0 : ctx->heap_current - shrink;
    }
    return new_ptr;
}

void badge_lua_guard_setup(lua_State *L, LuaCtx *ctx)
{
    static_assert(LUA_EXTRASPACE >= sizeof(LuaCtx *),
                  "Lua extraspace must hold the app context pointer");
    ctx->L = L;
    *static_cast<LuaCtx **>(lua_getextraspace(L)) = ctx;
    lua_atpanic(L, panic_handler);
    lua_setwarnf(L, warning_handler, ctx);
}

LuaCtx *badge_lua_guard_ctx(lua_State *L)
{
    if (!L) return nullptr;
    LuaCtx *ctx = *static_cast<LuaCtx **>(lua_getextraspace(L));
    return ctx && ctx->L == L ? ctx : nullptr;
}

int badge_lua_guard_run(lua_State *L, BadgeLuaProtectedFunction function,
                        void *userdata, std::int64_t budget_us)
{
    LuaCtx *ctx = badge_lua_guard_ctx(L);
    if (!ctx || !function) return LUA_ERRRUN;

    ctx->protected_function = function;
    ctx->protected_userdata = userdata;
    ctx->protected_status = LUA_OK;
    ctx->timed_out = false;
    ctx->stack_guard_tripped = false;
    ctx->traceback_active = false;
    ctx->traceback_failed = false;
    ctx->traceback_allocation_failed = false;
    ctx->emergency_reserve_enabled = false;
    ctx->in_callback = true;

    const std::int64_t now = esp_timer_get_time();
    ctx->deadline_us = budget_us > std::numeric_limits<std::int64_t>::max() - now
        ? std::numeric_limits<std::int64_t>::max()
        : now + budget_us;
    ctx->deadline_armed = true;

    // A zero-upvalue C function is a light C function and needs no Lua allocation.
    lua_pushcfunction(L, protected_dispatcher);
    lua_sethook(L, execution_hook, LUA_MASKCOUNT, kHookInstructionCount);
    int outer_status = lua_pcall(L, 0, 0, 0);

    disarm_deadline(L, ctx);
    if (ctx->traceback_active) {
        ctx->traceback_active = false;
        ctx->traceback_failed = true;
    }
    ctx->emergency_reserve_enabled = false;
    ctx->in_callback = false;
    ctx->protected_function = nullptr;
    ctx->protected_userdata = nullptr;

    return outer_status == LUA_OK || ctx->protected_status == LUA_OK
        ? outer_status
        : ctx->protected_status;
}
