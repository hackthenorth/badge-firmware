#include "lua_internal.h"

extern "C" {
#include "lauxlib.h"
}

#include "hal_radio.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <cstdio>
#include <cstring>

namespace {

// Restricted channel: every Lua frame carries this prefix and RX is filtered
// to it, so scripts can neither emit nor observe BBX1/BMP2/sync frames.
constexpr char kLuaMagic[4] = { 'L', 'U', 'A', '1' };
constexpr std::size_t kMaxPayloadBytes = 44;  // 48-byte ring slot minus magic
constexpr std::size_t kMaxFrameBytes = 48;

// Guards the ring head/tail between the NimBLE producer and the LVGL
// consumer. File-static (not in LuaCtx) so it never depends on ctx
// zero-init: portMUX_INITIALIZER_UNLOCKED is the only correct initial state.
portMUX_TYPE s_radio_ring_lock = portMUX_INITIALIZER_UNLOCKED;

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

// Runs on the NimBLE host task: touch the ring only, never Lua state.
void radio_recv_producer(LuaCtx *ctx, const uint8_t mac[6], int8_t rssi,
                         const uint8_t *data, size_t len)
{
    if (len < sizeof(kLuaMagic) + 1 || len > kMaxFrameBytes) return;
    if (std::memcmp(data, kLuaMagic, sizeof(kLuaMagic)) != 0) return;

    portENTER_CRITICAL(&s_radio_ring_lock);
    const std::uint8_t next =
        static_cast<std::uint8_t>((ctx->radio_head + 1) % LuaCtx::kLuaRadioRingSize);
    if (next == ctx->radio_tail) {
        if (ctx->radio_dropped != 0xffffffffu) ++ctx->radio_dropped;
        portEXIT_CRITICAL(&s_radio_ring_lock);
        return;
    }
    LuaCtx::LuaRadioFrame *slot = &ctx->radio_ring[ctx->radio_head];
    std::memcpy(slot->mac, mac, sizeof(slot->mac));
    slot->rssi = rssi;
    slot->len = static_cast<std::uint8_t>(len);
    std::memcpy(slot->data, data, len);
    ctx->radio_head = next;
    portEXIT_CRITICAL(&s_radio_ring_lock);
}

int radio_enable(lua_State *L)
{
    check_arg_count(L, 0, "badge.radio.enable");
    LuaCtx *ctx = check_ctx(L);
    if (hal_radio_enable() != ESP_OK) {
        lua_pushboolean(L, 0);
        return 1;
    }
    // The callback captures the LuaCtx address, which is a ScriptApp member
    // and therefore stable for the app's lifetime. Cleanup clears it before
    // the state closes, so a late frame can never reach freed Lua state.
    hal_radio_set_recv_cb([ctx](const uint8_t mac[6], int8_t rssi,
                                const uint8_t *data, size_t len) {
        radio_recv_producer(ctx, mac, rssi, data, len);
    });
    ctx->radio_enabled_by_lua = true;
    lua_pushboolean(L, 1);
    return 1;
}

int radio_disable(lua_State *L)
{
    check_arg_count(L, 0, "badge.radio.disable");
    // Deferred: keep BLE stack work off the Lua callback and let the tick
    // perform it outside the script call, on the LVGL task as usual.
    check_ctx(L)->pending_radio_disable = true;
    return 0;
}

int radio_send(lua_State *L)
{
    check_arg_count(L, 1, "badge.radio.send");
    LuaCtx *ctx = check_ctx(L);
    if (!ctx->radio_enabled_by_lua) {
        return luaL_error(L, "radio is not enabled (call badge.radio.enable first)");
    }
    std::size_t len = 0;
    const char *payload = luaL_checklstring(L, 1, &len);
    if (len == 0 || len > kMaxPayloadBytes) {
        return luaL_error(L, "payload must be 1-%d bytes", (int)kMaxPayloadBytes);
    }
    std::uint8_t frame[kMaxFrameBytes];
    std::memcpy(frame, kLuaMagic, sizeof(kLuaMagic));
    std::memcpy(frame + sizeof(kLuaMagic), payload, len);
    if (hal_radio_send(frame, sizeof(kLuaMagic) + len) != ESP_OK) {
        lua_pushboolean(L, 0);
        return 1;
    }
    lua_pushboolean(L, 1);
    return 1;
}

int radio_on_recv(lua_State *L)
{
    check_arg_count(L, 1, "badge.radio.on_recv");
    LuaCtx *ctx = check_ctx(L);
    if (!lua_isfunction(L, 1) && !lua_isnil(L, 1)) {
        return luaL_error(L, "handler must be a function or nil");
    }
    if (ctx->radio_recv_ref != LUA_NOREF) {
        luaL_unref(L, LUA_REGISTRYINDEX, ctx->radio_recv_ref);
        ctx->radio_recv_ref = LUA_NOREF;
    }
    if (lua_isfunction(L, 1)) {
        lua_pushvalue(L, 1);
        ctx->radio_recv_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    }
    return 0;
}

int radio_mac(lua_State *L)
{
    check_arg_count(L, 0, "badge.radio.mac");
    std::uint8_t mac[6] = {};
    hal_radio_local_mac(mac);
    char text[18];
    std::snprintf(text, sizeof(text), "%02X:%02X:%02X:%02X:%02X:%02X",
                  mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    lua_pushstring(L, text);
    return 1;
}

int radio_dropped(lua_State *L)
{
    check_arg_count(L, 0, "badge.radio.dropped");
    lua_pushinteger(L, static_cast<lua_Integer>(check_ctx(L)->radio_dropped));
    return 1;
}

} // namespace

void badge_lua_radio_install(lua_State *L, LuaCtx *ctx, int badge_index)
{
    badge_index = lua_absindex(L, badge_index);
    ctx->radio_head = 0;
    ctx->radio_tail = 0;
    ctx->radio_dropped = 0;
    ctx->radio_recv_ref = LUA_NOREF;
    ctx->radio_enabled_by_lua = false;

    static const luaL_Reg radio_functions[] = {
        { "enable",   radio_enable   },
        { "disable",  radio_disable  },
        { "send",     radio_send     },
        { "on_recv",  radio_on_recv  },
        { "mac",      radio_mac      },
        { "dropped",  radio_dropped  },
        { nullptr, nullptr },
    };
    lua_newtable(L);
    luaL_setfuncs(L, radio_functions, 0);
    lua_setfield(L, badge_index, "radio");
}

void badge_lua_radio_cleanup(LuaCtx *ctx)
{
    if (!ctx) return;
    // Clearing the callback first guarantees no NimBLE-task write races the
    // disable below or a later state close.
    hal_radio_set_recv_cb(nullptr);
    if (ctx->radio_enabled_by_lua) {
        ctx->radio_enabled_by_lua = false;
        hal_radio_disable();
    }
    if (ctx->L && ctx->radio_recv_ref != LUA_NOREF) {
        luaL_unref(ctx->L, LUA_REGISTRYINDEX, ctx->radio_recv_ref);
    }
    ctx->radio_recv_ref = LUA_NOREF;
    ctx->radio_head = 0;
    ctx->radio_tail = 0;
}

bool badge_lua_radio_has_frames(LuaCtx *ctx)
{
    if (!ctx || ctx->radio_recv_ref == LUA_NOREF) return false;
    std::uint8_t head = 0, tail = 0;
    portENTER_CRITICAL(&s_radio_ring_lock);
    head = ctx->radio_head;
    tail = ctx->radio_tail;
    portEXIT_CRITICAL(&s_radio_ring_lock);
    return head != tail;
}

int badge_lua_radio_deliver(lua_State *L, LuaCtx *ctx, void *)
{
    // Pop at most kLuaRadioDeliverMax frames under the lock, then invoke Lua
    // with the lock released: a chatty channel degrades to drops, never to
    // a stalled LVGL task.
    LuaCtx::LuaRadioFrame frames[LuaCtx::kLuaRadioDeliverMax];
    std::size_t count = 0;
    portENTER_CRITICAL(&s_radio_ring_lock);
    while (ctx->radio_tail != ctx->radio_head
           && count < LuaCtx::kLuaRadioDeliverMax) {
        frames[count++] = ctx->radio_ring[ctx->radio_tail];
        ctx->radio_tail = static_cast<std::uint8_t>(
            (ctx->radio_tail + 1) % LuaCtx::kLuaRadioRingSize);
    }
    portEXIT_CRITICAL(&s_radio_ring_lock);

    for (std::size_t i = 0; i < count; ++i) {
        lua_rawgeti(L, LUA_REGISTRYINDEX, ctx->radio_recv_ref);
        if (!lua_isfunction(L, -1)) {
            return luaL_error(L, "radio receive handler is invalid");
        }
        char mac_text[18];
        std::snprintf(mac_text, sizeof(mac_text), "%02X:%02X:%02X:%02X:%02X:%02X",
                      frames[i].mac[0], frames[i].mac[1], frames[i].mac[2],
                      frames[i].mac[3], frames[i].mac[4], frames[i].mac[5]);
        lua_pushstring(L, mac_text);
        lua_pushinteger(L, frames[i].rssi);
        lua_pushlstring(L, reinterpret_cast<const char *>(frames[i].data)
                                  + sizeof(kLuaMagic),
                        frames[i].len - sizeof(kLuaMagic));
        lua_call(L, 3, 0);
    }
    return 0;
}
