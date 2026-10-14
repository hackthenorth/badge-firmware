#include "lua_internal.h"

extern "C" {
#include "lauxlib.h"
}

#include "hal_nfc.h"

#include <cstdio>
#include <cstring>

namespace {

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

int nfc_enable(lua_State *L)
{
    check_arg_count(L, 0, "badge.nfc.enable");
    LuaCtx *ctx = check_ctx(L);
    if (hal_nfc_enable() != ESP_OK) {
        lua_pushboolean(L, 0);
        return 1;
    }
    ctx->nfc_enabled_by_lua = true;
    lua_pushboolean(L, 1);
    return 1;
}

int nfc_disable(lua_State *L)
{
    check_arg_count(L, 0, "badge.nfc.disable");
    // Deferred like radio.disable: the registry force-disables NFC on every
    // app switch anyway, and teardown from inside a callback risks blocking
    // the LVGL task on the reader driver.
    check_ctx(L)->pending_nfc_disable = true;
    return 0;
}

int nfc_card(lua_State *L)
{
    check_arg_count(L, 0, "badge.nfc.card");
    NfcCard card = {};
    if (!hal_nfc_last_card(&card)) {
        lua_pushnil(L);
        return 1;
    }
    char uid_hex[21];
    std::size_t n = card.uid_len > sizeof(card.uid) ? sizeof(card.uid) : card.uid_len;
    for (std::size_t i = 0; i < n; ++i) {
        std::snprintf(uid_hex + 2 * i, sizeof(uid_hex) - 2 * i, "%02X",
                      card.uid[i]);
    }
    uid_hex[2 * n] = '\0';
    lua_newtable(L);
    lua_pushstring(L, uid_hex);
    lua_setfield(L, -2, "uid");
    lua_pushinteger(L, card.sak);
    lua_setfield(L, -2, "sak");
    lua_pushinteger(L, card.atqa);
    lua_setfield(L, -2, "atqa");
    return 1;
}

int nfc_read_text(lua_State *L)
{
    check_arg_count(L, 0, "badge.nfc.read_text");
    char out[256];
    if (hal_nfc_read_text(out, sizeof(out)) != ESP_OK) {
        lua_pushnil(L);
        lua_pushstring(L, "no text record");
        return 2;
    }
    lua_pushstring(L, out);
    return 1;
}

int nfc_clear(lua_State *L)
{
    check_arg_count(L, 0, "badge.nfc.clear");
    hal_nfc_clear_last_card();
    return 0;
}

} // namespace

void badge_lua_nfc_install(lua_State *L, int badge_index)
{
    badge_index = lua_absindex(L, badge_index);
    static const luaL_Reg nfc_functions[] = {
        { "enable",    nfc_enable    },
        { "disable",   nfc_disable   },
        { "card",      nfc_card      },
        { "read_text", nfc_read_text },
        { "clear",     nfc_clear     },
        { nullptr, nullptr },
    };
    lua_newtable(L);
    luaL_setfuncs(L, nfc_functions, 0);
    lua_setfield(L, badge_index, "nfc");
}
