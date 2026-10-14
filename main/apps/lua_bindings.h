#pragma once

extern "C" {
#include "lua.h"
}

#include "lua_internal.h"

// Installs the `badge` global table and the single non-owning `badge.obj`
// metatable. The context is stored in the registry and must outlive `L`.
void badge_lua_install(lua_State *L, LuaCtx *ctx);

// Push the borrowed app root through the same record/dedup path as all other
// objects. Returns false if native record allocation failed.
bool badge_lua_push_root(lua_State *L);

// Remove the root delete callback, invalidate its record, and drop its widget
// reference. Script-created children must be deleted before this call.
void badge_lua_detach_root(LuaCtx *ctx);
