#include "lua_internal.h"

extern "C" {
#include "lauxlib.h"
}

#include "hal_accel.h"
#include "hal_buttons.h"
#include "hal_lights.h"

#include <cmath>
#include <cstddef>
#include <cstdint>

namespace {

constexpr float kShakeThresholdMg = 1200.0f;
constexpr float kTapThresholdMg = 1800.0f;
constexpr std::uint8_t kShakeRefractorySamples = 8;
constexpr std::uint8_t kTapRefractorySamples = 4;

enum SensorString : std::uint8_t {
    SensorUnknown,
    SensorFlatUp,
    SensorFlatDown,
    SensorLeftEdge,
    SensorRightEdge,
    SensorTopEdge,
    SensorBottomEdge,
    SensorStringCount,
};

constexpr const char *kSensorNames[SensorStringCount] = {
    "unknown",
    "flat_up",
    "flat_down",
    "left_edge",
    "right_edge",
    "top_edge",
    "bottom_edge",
};

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

lua_Integer check_integer(lua_State *L, int index, const char *name)
{
    if (!lua_isinteger(L, index)) luaL_error(L, "%s must be an integer", name);
    return lua_tointeger(L, index);
}

std::uint8_t check_component(lua_State *L, int index, const char *name)
{
    const lua_Integer value = check_integer(L, index, name);
    if (value < 0 || value > 255) {
        luaL_error(L, "%s must be between 0 and 255", name);
    }
    return static_cast<std::uint8_t>(value);
}

std::size_t check_led_index(lua_State *L)
{
    const lua_Integer value = check_integer(L, 1, "index");
    if (value < 1 || value > static_cast<lua_Integer>(LED_COUNT)) {
        luaL_error(L, "index must be between 1 and %d", static_cast<int>(LED_COUNT));
    }
    return static_cast<std::size_t>(value - 1);
}

Button check_button(lua_State *L)
{
    const lua_Integer value = check_integer(L, 1, "button");
    if (value < 0 || value >= static_cast<lua_Integer>(Button::COUNT)) {
        luaL_error(L, "button is not a valid badge.input.BUTTON value");
    }
    return static_cast<Button>(value);
}

std::uint16_t button_mask(Button button)
{
    return static_cast<std::uint16_t>(1u << static_cast<std::uint8_t>(button));
}

int badge_led_set(lua_State *L)
{
    check_arg_count(L, 4, "badge.led.set");
    LuaCtx *ctx = check_ctx(L);
    const std::size_t index = check_led_index(L);
    const std::uint8_t r = check_component(L, 2, "red");
    const std::uint8_t g = check_component(L, 3, "green");
    const std::uint8_t b = check_component(L, 4, "blue");
    ctx->led_shadow[index][0] = r;
    ctx->led_shadow[index][1] = g;
    ctx->led_shadow[index][2] = b;
    hal_lights_set(index, r, g, b);
    return 0;
}

int badge_led_set_all(lua_State *L)
{
    check_arg_count(L, 3, "badge.led.set_all");
    LuaCtx *ctx = check_ctx(L);
    const std::uint8_t r = check_component(L, 1, "red");
    const std::uint8_t g = check_component(L, 2, "green");
    const std::uint8_t b = check_component(L, 3, "blue");
    for (std::size_t i = 0; i < LED_COUNT; ++i) {
        ctx->led_shadow[i][0] = r;
        ctx->led_shadow[i][1] = g;
        ctx->led_shadow[i][2] = b;
        hal_lights_set(i, r, g, b);
    }
    return 0;
}

int badge_led_clear(lua_State *L)
{
    check_arg_count(L, 0, "badge.led.clear");
    LuaCtx *ctx = check_ctx(L);
    for (std::size_t i = 0; i < LED_COUNT; ++i) {
        ctx->led_shadow[i][0] = 0;
        ctx->led_shadow[i][1] = 0;
        ctx->led_shadow[i][2] = 0;
        hal_lights_set(i, 0, 0, 0);
    }
    return 0;
}

int badge_led_show(lua_State *L)
{
    check_arg_count(L, 0, "badge.led.show");
    static_cast<void>(check_ctx(L));
    hal_lights_show();
    return 0;
}

int badge_led_count(lua_State *L)
{
    check_arg_count(L, 0, "badge.led.count");
    lua_pushinteger(L, static_cast<lua_Integer>(LED_COUNT));
    return 1;
}

bool sensor_sample(LuaCtx *ctx, AccelSample *sample)
{
    if (hal_accel_cached_read(sample) != ESP_OK) return false;

    const float magnitude = std::sqrt(sample->x_mg * sample->x_mg
                                    + sample->y_mg * sample->y_mg
                                    + sample->z_mg * sample->z_mg);
    if (!ctx->sensor_baseline_primed) {
        ctx->sensor_baseline_mg = magnitude;
        ctx->sensor_baseline_primed = true;
    }
    ctx->sensor_baseline_mg = ctx->sensor_baseline_mg * 0.92f + magnitude * 0.08f;
    return true;
}

int badge_sensor_accel(lua_State *L)
{
    check_arg_count(L, 0, "badge.sensor.accel");
    LuaCtx *ctx = check_ctx(L);
    AccelSample sample = {};
    if (hal_accel_cached_read(&sample) != ESP_OK) {
        lua_pushnil(L);
        lua_rawgeti(L, LUA_REGISTRYINDEX, ctx->sensor_unavailable_ref);
        return 2;
    }
    lua_pushnumber(L, sample.x_mg);
    lua_pushnumber(L, sample.y_mg);
    lua_pushnumber(L, sample.z_mg);
    return 3;
}

int badge_sensor_orientation(lua_State *L)
{
    check_arg_count(L, 0, "badge.sensor.orientation");
    LuaCtx *ctx = check_ctx(L);
    AccelSample sample = {};
    SensorString orientation = SensorUnknown;
    if (hal_accel_cached_read(&sample) == ESP_OK) {
        const float absx = std::fabs(sample.x_mg);
        const float absy = std::fabs(sample.y_mg);
        const float absz = std::fabs(sample.z_mg);
        if (absz > absx && absz > absy && absz > 700.0f) {
            orientation = sample.z_mg > 0 ? SensorFlatUp : SensorFlatDown;
        } else if (absx > absy && absx > absz && absx > 700.0f) {
            orientation = sample.x_mg > 0 ? SensorRightEdge : SensorLeftEdge;
        } else if (absy > absx && absy > absz && absy > 700.0f) {
            orientation = sample.y_mg > 0 ? SensorTopEdge : SensorBottomEdge;
        }
    }
    lua_rawgeti(L, LUA_REGISTRYINDEX, ctx->sensor_orientation_refs[orientation]);
    return 1;
}

int badge_input_is_down(lua_State *L)
{
    check_arg_count(L, 1, "badge.input.is_down");
    LuaCtx *ctx = check_ctx(L);
    const Button button = check_button(L);
    const bool is_down = button != Button::Home
        && (ctx->held_mask & button_mask(button)) != 0;
    lua_pushboolean(L, is_down ? 1 : 0);
    return 1;
}

int badge_input_held(lua_State *L)
{
    check_arg_count(L, 0, "badge.input.held");
    LuaCtx *ctx = check_ctx(L);
    const std::uint16_t held = ctx->held_mask
        & static_cast<std::uint16_t>(~button_mask(Button::Home));
    lua_pushinteger(L, static_cast<lua_Integer>(held));
    return 1;
}

void set_button_constants(lua_State *L)
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
}

void set_kind_constants(lua_State *L)
{
    lua_newtable(L);
    lua_pushinteger(L, static_cast<lua_Integer>(ButtonEventKind::Pressed));
    lua_setfield(L, -2, "PRESSED");
    lua_pushinteger(L, static_cast<lua_Integer>(ButtonEventKind::Released));
    lua_setfield(L, -2, "RELEASED");
}

void cache_sensor_strings(lua_State *L, LuaCtx *ctx)
{
    lua_pushliteral(L, "accelerometer cache unavailable");
    ctx->sensor_unavailable_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    for (std::uint8_t i = 0; i < SensorStringCount; ++i) {
        lua_pushstring(L, kSensorNames[i]);
        ctx->sensor_orientation_refs[i] = luaL_ref(L, LUA_REGISTRYINDEX);
    }
    ctx->sensor_strings_ready = true;
}

} // namespace

// Global scope (declared in lua_internal.h for the legacy gesture aliases in
// lua_bindings.cpp). Defined here, after the anonymous-namespace helpers.
int badge_lua_hw_sensor_shake(lua_State *L)
{
    check_arg_count(L, 0, "badge.sensor.shake");
    LuaCtx *ctx = check_ctx(L);
    AccelSample sample = {};
    if (!sensor_sample(ctx, &sample)) {
        lua_pushboolean(L, 0);
        return 1;
    }
    if (ctx->sensor_shake_refractory != 0) {
        --ctx->sensor_shake_refractory;
        lua_pushboolean(L, 0);
        return 1;
    }
    const float magnitude = std::sqrt(sample.x_mg * sample.x_mg
                                    + sample.y_mg * sample.y_mg
                                    + sample.z_mg * sample.z_mg);
    const float jerk = std::fabs(magnitude - ctx->sensor_baseline_mg);
    if (jerk > kShakeThresholdMg) {
        ctx->sensor_shake_refractory = kShakeRefractorySamples;
        lua_pushboolean(L, 1);
        return 1;
    }
    lua_pushboolean(L, 0);
    return 1;
}

int badge_lua_hw_sensor_tap(lua_State *L)
{
    check_arg_count(L, 0, "badge.sensor.tap");
    LuaCtx *ctx = check_ctx(L);
    AccelSample sample = {};
    if (!sensor_sample(ctx, &sample)) {
        lua_pushboolean(L, 0);
        return 1;
    }
    if (ctx->sensor_tap_refractory != 0) {
        --ctx->sensor_tap_refractory;
        lua_pushboolean(L, 0);
        return 1;
    }
    const float magnitude = std::sqrt(sample.x_mg * sample.x_mg
                                    + sample.y_mg * sample.y_mg
                                    + sample.z_mg * sample.z_mg);
    const float jerk = std::fabs(magnitude - ctx->sensor_baseline_mg);
    if (jerk > kTapThresholdMg) {
        ctx->sensor_tap_refractory = kTapRefractorySamples;
        lua_pushboolean(L, 1);
        return 1;
    }
    lua_pushboolean(L, 0);
    return 1;
}

void badge_lua_hw_install(lua_State *L, LuaCtx *ctx, int badge_index)
{
    // Pin the badge table: every lua_setfield below pushes first, which
    // would retarget a relative index at the fresh table instead of badge.
    badge_index = lua_absindex(L, badge_index);
    cache_sensor_strings(L, ctx);

    static const luaL_Reg led_functions[] = {
        { "set",     badge_led_set     },
        { "set_all", badge_led_set_all },
        { "clear",   badge_led_clear   },
        { "show",    badge_led_show    },
        { "count",   badge_led_count   },
        { nullptr,    nullptr           },
    };
    lua_newtable(L);
    luaL_setfuncs(L, led_functions, 0);
    lua_setfield(L, badge_index, "led");

    static const luaL_Reg sensor_functions[] = {
        { "accel",       badge_sensor_accel          },
        { "shake",       badge_lua_hw_sensor_shake   },
        { "tap",         badge_lua_hw_sensor_tap     },
        { "orientation", badge_sensor_orientation    },
        { nullptr,        nullptr                     },
    };
    lua_newtable(L);
    luaL_setfuncs(L, sensor_functions, 0);
    lua_setfield(L, badge_index, "sensor");

    static const luaL_Reg input_functions[] = {
        { "is_down", badge_input_is_down },
        { "held",    badge_input_held    },
        { nullptr,    nullptr             },
    };
    lua_newtable(L);
    luaL_setfuncs(L, input_functions, 0);
    set_button_constants(L);
    lua_setfield(L, -2, "BUTTON");
    set_kind_constants(L);
    lua_setfield(L, -2, "KIND");
    lua_setfield(L, badge_index, "input");
}

void badge_lua_hw_cleanup(LuaCtx *ctx)
{
    if (ctx) {
        for (std::size_t i = 0; i < LED_COUNT; ++i) {
            ctx->led_shadow[i][0] = 0;
            ctx->led_shadow[i][1] = 0;
            ctx->led_shadow[i][2] = 0;
        }
        ctx->held_mask = 0;
        ctx->sensor_shake_refractory = 0;
        ctx->sensor_tap_refractory = 0;
        ctx->sensor_baseline_mg = 0.0f;
        ctx->sensor_baseline_primed = false;
    }

    for (std::size_t i = 0; i < LED_COUNT; ++i) {
        hal_lights_set(i, 0, 0, 0);
    }
    hal_lights_show();
}

void badge_lua_hw_cleanup_no_context()
{
    badge_lua_hw_cleanup(nullptr);
}
