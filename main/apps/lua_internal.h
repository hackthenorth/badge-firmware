#pragma once

extern "C" {
#include "lua.h"
}

#include "lvgl.h"
#include "hal_lights.h"

#include <cstddef>
#include <cstdint>

struct BadgeObjRecord;
struct LuaCtx;

using BadgeLuaProtectedFunction = int (*)(lua_State *L, LuaCtx *ctx, void *userdata);

// Lua and every LVGL object referenced by it are confined to the LVGL task.
// App lifecycle hooks already run under the LVGL lock; later phases must keep
// asynchronous work from touching this context directly.
struct LuaCtx {
    lua_State *L;
    lv_obj_t *root;
    const char *slug;      // Stable storage owned by ScriptApp.
    const char *dir;       // Stable storage owned by ScriptApp.
    const char *app_name;  // Stable storage owned by ScriptApp.
    std::uint8_t api;
    std::uint8_t heap_kb;
    std::size_t heap_limit;
    std::size_t heap_current;
    std::size_t heap_peak;
    std::uint32_t live_object_count;
    std::uint32_t record_count;
    BadgeObjRecord *root_record;
    BadgeLuaProtectedFunction protected_function;
    void *protected_userdata;
    std::int64_t deadline_us;
    int protected_status;
    bool state_live;
    bool dispatch_enabled;
    bool tearing_down;
    bool in_callback;
    bool emergency_reserve_enabled;
    bool deadline_armed;
    bool timed_out;
    bool stack_guard_tripped;
    bool traceback_active;
    bool traceback_failed;
    bool traceback_allocation_failed;
    std::uint8_t led_shadow[LED_COUNT][3];
    std::uint8_t sensor_shake_refractory;
    std::uint8_t sensor_tap_refractory;
    std::uint16_t held_mask;
    float sensor_baseline_mg;
    bool sensor_baseline_primed;
    bool sensor_strings_ready;
    int sensor_unavailable_ref;
    int sensor_orientation_refs[7];
    // Deferred actions requested from inside Lua callbacks. Acting on them
    // inline would close the state (exit) or tear down the stack (radio/nfc
    // disable) while a lua_pcall is still on the C stack, so ScriptApp
    // drains these after the protected call returns.
    bool pending_exit;
    bool pending_nfc_disable;
    bool pending_radio_disable;
    bool nfc_enabled_by_lua;
    bool radio_enabled_by_lua;
    // Filesystem quota: bytes currently attributed to this app, scanned at
    // install and tracked by badge.fs writes/removes.
    std::size_t fs_quota_used;
    // Sandboxed require caps.
    std::uint8_t require_depth;
    std::uint8_t require_module_count;
    int require_cache_ref;
    // Restricted radio channel: single-producer (NimBLE host task) /
    // single-consumer (LVGL task) ring, guarded by lua_radio.cpp's
    // file-static spinlock. The producer only touches ring storage plus
    // head under that lock, never Lua state.
    struct LuaRadioFrame {
        std::uint8_t mac[6];
        std::int8_t rssi;
        std::uint8_t len;
        std::uint8_t data[48];
    };
    static constexpr std::size_t kLuaRadioRingSize = 8;
    static constexpr std::size_t kLuaRadioDeliverMax = 4;
    LuaRadioFrame radio_ring[kLuaRadioRingSize];
    std::uint8_t radio_head;
    std::uint8_t radio_tail;
    std::uint32_t radio_dropped;
    int radio_recv_ref;
};

using BadgeObjAuxDeleter = void (*)(void *);

enum BadgeObjFlag : std::uint32_t {
    BADGE_OBJ_ROOT    = 1u << 0,
    BADGE_OBJ_FOREIGN = 1u << 1,
    BADGE_OBJ_COUNTED = 1u << 2,
};

struct BadgeObjRecord {
    lv_obj_t *obj;
    LuaCtx *ctx;
    std::uint32_t flags;
    std::uint32_t refcount;
    void *aux;
    BadgeObjAuxDeleter aux_deleter;
};

struct BadgeObj {
    BadgeObjRecord *record;
};

// Context and object internals shared by module assembly and ScriptApp.
void *badge_lua_alloc(void *userdata, void *ptr, std::size_t osize,
                      std::size_t nsize);
void badge_lua_guard_setup(lua_State *L, LuaCtx *ctx);
LuaCtx *badge_lua_guard_ctx(lua_State *L);
int badge_lua_guard_run(lua_State *L, BadgeLuaProtectedFunction function,
                        void *userdata, std::int64_t budget_us);
void badge_lua_store_ctx(lua_State *L, LuaCtx *ctx);
LuaCtx *badge_lua_get_ctx(lua_State *L);
void badge_lua_obj_install(lua_State *L);
BadgeObjRecord *badge_lua_obj_check_record(lua_State *L, int index);
// Validates userdata ownership and liveness, and rejects the borrowed root.
// Binding modules must use this rather than accessing BadgeObjRecord directly.
BadgeObjRecord *badge_lua_obj_check_mutable_record(lua_State *L, int index);
bool badge_lua_obj_push(lua_State *L, lv_obj_t *obj, std::uint32_t flags);
void badge_lua_obj_detach_root(LuaCtx *ctx);
void badge_lua_obj_check_object_budget(lua_State *L, LuaCtx *ctx);
const char *badge_lua_obj_type_name(lv_obj_t *obj);
bool badge_lua_obj_parse_align(const char *name, lv_align_t *align);

// Type-specific setters are shared by badge.obj methods and badge.ui tables.
void badge_lua_obj_set_text(lua_State *L, BadgeObjRecord *record, int value_index);
void badge_lua_obj_set_value(lua_State *L, BadgeObjRecord *record, int value_index);
void badge_lua_obj_set_range(lua_State *L, BadgeObjRecord *record, int min_index,
                             int max_index);
void badge_lua_obj_validate_image_src(lua_State *L, LuaCtx *ctx, int value_index);
void badge_lua_obj_set_src(lua_State *L, BadgeObjRecord *record, int value_index);
lv_point_precise_t *badge_lua_obj_copy_points(lua_State *L, int value_index,
                                              std::uint32_t *point_count);
void badge_lua_obj_set_native_points(BadgeObjRecord *record,
                                     lv_point_precise_t *points,
                                     std::uint32_t point_count);
void badge_lua_obj_set_checked(lua_State *L, BadgeObjRecord *record, int value_index);
void badge_lua_obj_set_options(lua_State *L, BadgeObjRecord *record, int value_index);

void badge_lua_style_add_object_methods(lua_State *L, int methods_index);
void badge_lua_style_install_theme(lua_State *L, int badge_index);
void badge_lua_style_apply_color(lv_obj_t *obj, std::uint32_t rgb,
                                  lv_style_selector_t selector);
void badge_lua_style_validate_props(lua_State *L, int table_index);
void badge_lua_style_apply_props(lua_State *L, lv_obj_t *obj, int table_index,
                                 lv_style_selector_t selector);

void badge_lua_ui_install(lua_State *L, int badge_index);
void badge_lua_ui_install_legacy(lua_State *L, int badge_index);

// Lua hardware bindings have no asynchronous access to LuaCtx. Cleanup only
// touches the LED HAL and may therefore run after a Lua callback has failed.
void badge_lua_hw_install(lua_State *L, LuaCtx *ctx, int badge_index);
void badge_lua_hw_cleanup(LuaCtx *ctx);
void badge_lua_hw_cleanup_no_context();
int badge_lua_hw_sensor_shake(lua_State *L);
int badge_lua_hw_sensor_tap(lua_State *L);

// System, store, identity, contacts, and app-lifecycle bindings. Stateless
// with respect to LuaCtx apart from the deferred exit flag.
void badge_lua_sys_install(lua_State *L, int badge_index);

// Filesystem sandbox plus quota. Scans current usage at install.
void badge_lua_fs_install(lua_State *L, LuaCtx *ctx, int badge_index);
// Sandboxed require over the app's own directory. Replaces the stripped
// global after bootstrap.
void badge_lua_require_install(lua_State *L, LuaCtx *ctx);

// NFC reader. Enable is immediate; disable defers through LuaCtx::pending.
void badge_lua_nfc_install(lua_State *L, int badge_index);

// Restricted Lua-only radio channel (LUA1 framing + SPSC ring). Cleanup
// clears the NimBLE receive callback so a late frame can never touch a
// closed state.
void badge_lua_radio_install(lua_State *L, LuaCtx *ctx, int badge_index);
void badge_lua_radio_cleanup(LuaCtx *ctx);
bool badge_lua_radio_has_frames(LuaCtx *ctx);
int badge_lua_radio_deliver(lua_State *L, LuaCtx *ctx, void *userdata);
