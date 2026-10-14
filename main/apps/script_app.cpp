#include "script_app.h"
#include "app_registry.h"
#include "launcher_slugs.h"
#include "lua_bindings.h"
#include "ui_error_card.h"
#include "hal_nfc.h"

extern "C" {
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
}

#include "esp_log.h"
#include "esp_timer.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

static const char *TAG = "script_app";

// Last-run Lua heap snapshot, for the diagnostics app's `lua:` line — the
// script's own state is closed by the time diagnostics runs, so sys.stats()
// would have nothing to read. Written on every close_lua that had a state.
static ScriptAppLuaStats s_last_lua_stats = {};

namespace {

constexpr std::size_t kPathBytes = 512;
constexpr std::size_t kLoadPathBytes = 128;
constexpr std::size_t kManifestLineBytes = 256;
constexpr std::size_t kMaxNameBytes = 48;
constexpr std::size_t kMaxMetadataBytes = 48;
constexpr std::size_t kMaxIconBytes = 12;
constexpr off_t kMaxMainLuaBytes = 64 * 1024;
constexpr std::int64_t kLogRepeatIntervalUs = 1000 * 1000;
// These are runaway-code cutoffs, not frame-time targets. Prefer allowing a
// slow app to finish a redraw or flash write over stopping otherwise valid
// code. Tick work remains more bounded because it can run continuously.
constexpr std::int64_t kBootstrapBudgetUs = 3000 * 1000;
constexpr std::int64_t kOnEnterBudgetUs = 3000 * 1000;
constexpr std::int64_t kOnTickBudgetUs = 250 * 1000;
constexpr std::int64_t kOnButtonBudgetUs = 1000 * 1000;
constexpr std::int64_t kOnExitBudgetUs = 1000 * 1000;

constexpr const char *kExitHint = "HOME: exit";
constexpr const char *kRetryHint = "A: retry  HOME: exit";

constexpr std::uint16_t button_mask(Button button)
{
    return static_cast<std::uint16_t>(1u << static_cast<std::uint8_t>(button));
}

enum ManifestField : unsigned {
    FIELD_SLUG,
    FIELD_NAME,
    FIELD_ICON,
    FIELD_API,
    FIELD_HEAP_KB,
    FIELD_WAKE_LOCK,
    FIELD_HOME_BUTTON,
    FIELD_CONFIRM_HOME,
    FIELD_BACK_BUTTON, // deprecated alias for FIELD_HOME_BUTTON
    FIELD_VERSION,
    FIELD_AUTHOR,
    FIELD_COUNT,
};

struct Manifest {
    std::string slug;
    std::string name;
    std::string icon = "?";
    std::uint8_t api = 1;
    std::uint8_t heap_kb = 48;
    bool wake_lock = false;
    bool home_button = false;
    bool confirm_home = false;
    std::string version;
    std::string author;
};

void set_reason(char *why, std::size_t why_size, const char *message)
{
    if (!why || why_size == 0) return;
    std::snprintf(why, why_size, "%s", message);
}

char *trim_ascii_space(char *text)
{
    while (*text == ' ' || *text == '\t') ++text;
    char *end = text + std::strlen(text);
    while (end > text && (end[-1] == ' ' || end[-1] == '\t')) --end;
    *end = '\0';
    return text;
}

bool join_path(char *out, std::size_t out_size, const char *dir, const char *name)
{
    int written = std::snprintf(out, out_size, "%s/%s", dir, name);
    return written >= 0 && (std::size_t)written < out_size;
}

bool valid_slug(const char *slug)
{
    std::size_t len = std::strlen(slug);
    if (len == 0 || len > 32) return false;
    if (!((slug[0] >= 'a' && slug[0] <= 'z')
          || (slug[0] >= '0' && slug[0] <= '9'))) return false;

    for (std::size_t i = 1; i < len; ++i) {
        char c = slug[i];
        if ((c >= 'a' && c <= 'z')
            || (c >= '0' && c <= '9')
            || c == '_' || c == '-') continue;
        return false;
    }
    return true;
}

bool read_manifest(const char *path, Manifest *manifest, char *why, std::size_t why_size)
{
    FILE *f = std::fopen(path, "r");
    if (!f) {
        set_reason(why, why_size, "cannot open manifest.cfg");
        return false;
    }

    bool seen[FIELD_COUNT] = {};
    char line[kManifestLineBytes];
    unsigned line_number = 0;
    bool ok = true;

    while (std::fgets(line, sizeof(line), f)) {
        ++line_number;
        std::size_t len = std::strlen(line);
        if (len > 0 && line[len - 1] == '\n') {
            line[--len] = '\0';
            if (len > 0 && line[len - 1] == '\r') line[--len] = '\0';
        } else if (!std::feof(f)) {
            std::snprintf(why, why_size, "line %u is too long", line_number);
            ok = false;
            break;
        }

        if (std::strchr(line, '\r')) {
            std::snprintf(why, why_size, "line %u has a malformed line ending", line_number);
            ok = false;
            break;
        }

        char *entry = trim_ascii_space(line);
        if (*entry == '\0' || *entry == '#') continue;
        if (std::strchr(entry, '#')) {
            std::snprintf(why, why_size, "line %u contains an inline comment", line_number);
            ok = false;
            break;
        }

        char *eq = std::strchr(entry, '=');
        if (!eq || std::strchr(eq + 1, '=')) {
            std::snprintf(why, why_size, "line %u is malformed", line_number);
            ok = false;
            break;
        }
        *eq = '\0';
        char *key = trim_ascii_space(entry);
        char *value = trim_ascii_space(eq + 1);
        if (*key == '\0') {
            std::snprintf(why, why_size, "line %u has an empty key", line_number);
            ok = false;
            break;
        }

        ManifestField field;
        if (!std::strcmp(key, "slug")) field = FIELD_SLUG;
        else if (!std::strcmp(key, "name")) field = FIELD_NAME;
        else if (!std::strcmp(key, "icon")) field = FIELD_ICON;
        else if (!std::strcmp(key, "api")) field = FIELD_API;
        else if (!std::strcmp(key, "heap_kb")) field = FIELD_HEAP_KB;
        else if (!std::strcmp(key, "wake_lock")) field = FIELD_WAKE_LOCK;
        else if (!std::strcmp(key, "home_button")) field = FIELD_HOME_BUTTON;
        else if (!std::strcmp(key, "confirm_home")) field = FIELD_CONFIRM_HOME;
        else if (!std::strcmp(key, "back_button")) field = FIELD_BACK_BUTTON;
        else if (!std::strcmp(key, "version")) field = FIELD_VERSION;
        else if (!std::strcmp(key, "author")) field = FIELD_AUTHOR;
        else {
            std::snprintf(why, why_size, "line %u has unknown key '%s'", line_number, key);
            ok = false;
            break;
        }

        if (seen[field]) {
            std::snprintf(why, why_size, "line %u duplicates key '%s'", line_number, key);
            ok = false;
            break;
        }
        seen[field] = true;

        switch (field) {
        case FIELD_SLUG:
            manifest->slug = value;
            break;
        case FIELD_NAME:
            manifest->name = value;
            break;
        case FIELD_ICON:
            if (*value == '\0' || std::strlen(value) > kMaxIconBytes) {
                set_reason(why, why_size, "icon must be 1-12 bytes");
                ok = false;
            } else {
                manifest->icon = value;
            }
            break;
        case FIELD_API:
            if (!std::strcmp(value, "1")) manifest->api = 1;
            else if (!std::strcmp(value, "2")) manifest->api = 2;
            else {
                set_reason(why, why_size, "api must be 1 or 2");
                ok = false;
            }
            break;
        case FIELD_HEAP_KB:
            if (!std::strcmp(value, "48")) manifest->heap_kb = 48;
            else if (!std::strcmp(value, "96")) manifest->heap_kb = 96;
            else {
                set_reason(why, why_size, "heap_kb must be 48 or 96");
                ok = false;
            }
            break;
        case FIELD_WAKE_LOCK:
            if (!std::strcmp(value, "0")) manifest->wake_lock = false;
            else if (!std::strcmp(value, "1")) manifest->wake_lock = true;
            else {
                set_reason(why, why_size, "wake_lock must be 0 or 1");
                ok = false;
            }
            break;
        case FIELD_HOME_BUTTON:
            if (!std::strcmp(value, "0")) manifest->home_button = false;
            else if (!std::strcmp(value, "1")) manifest->home_button = true;
            else {
                set_reason(why, why_size, "home_button must be 0 or 1");
                ok = false;
            }
            break;
        case FIELD_CONFIRM_HOME:
            if (!std::strcmp(value, "0")) manifest->confirm_home = false;
            else if (!std::strcmp(value, "1")) manifest->confirm_home = true;
            else {
                set_reason(why, why_size, "confirm_home must be 0 or 1");
                ok = false;
            }
            break;
        case FIELD_BACK_BUTTON:
            // Deprecated alias for home_button=. home_button= wins when both
            // are present regardless of order; back_button= applies only when
            // home_button= is absent. Either key may appear at most once.
            if (!std::strcmp(value, "0")) {
                if (!seen[FIELD_HOME_BUTTON]) manifest->home_button = false;
            } else if (!std::strcmp(value, "1")) {
                if (!seen[FIELD_HOME_BUTTON]) manifest->home_button = true;
            } else {
                set_reason(why, why_size, "back_button must be 0 or 1");
                ok = false;
            }
            break;
        case FIELD_VERSION:
            if (std::strlen(value) > kMaxMetadataBytes) {
                set_reason(why, why_size, "version must be at most 48 bytes");
                ok = false;
            } else {
                manifest->version = value;
            }
            break;
        case FIELD_AUTHOR:
            if (std::strlen(value) > kMaxMetadataBytes) {
                set_reason(why, why_size, "author must be at most 48 bytes");
                ok = false;
            } else {
                manifest->author = value;
            }
            break;
        case FIELD_COUNT:
            break;
        }

        if (!ok) break;
    }

    if (ok && std::ferror(f)) {
        set_reason(why, why_size, "error reading manifest.cfg");
        ok = false;
    }
    std::fclose(f);

    if (!ok) return false;
    if (!seen[FIELD_SLUG] || !seen[FIELD_NAME]) {
        set_reason(why, why_size, "slug and name are required");
        return false;
    }
    if (!valid_slug(manifest->slug.c_str())) {
        set_reason(why, why_size, "slug must match [a-z0-9][a-z0-9_-]{0,31}");
        return false;
    }
    if (manifest->confirm_home && manifest->home_button) {
        set_reason(why, why_size, "confirm_home and home_button cannot both be 1");
        return false;
    }
    if (manifest->name.empty() || manifest->name.size() > kMaxNameBytes) {
        set_reason(why, why_size, "name must be 1-48 bytes");
        return false;
    }
    return true;
}

void strip_global(lua_State *L, const char *name)
{
    lua_pushnil(L);
    lua_setglobal(L, name);
}

int bootstrap_lua(lua_State *L, LuaCtx *ctx, void *)
{
    // Selectively load standard libs; skip io, os, package, debug, coroutine.
    luaL_requiref(L, LUA_GNAME,       luaopen_base,   1); lua_pop(L, 1);
    luaL_requiref(L, LUA_TABLIBNAME,  luaopen_table,  1); lua_pop(L, 1);
    luaL_requiref(L, LUA_STRLIBNAME,  luaopen_string, 1); lua_pop(L, 1);
    luaL_requiref(L, LUA_MATHLIBNAME, luaopen_math,   1); lua_pop(L, 1);
    luaL_requiref(L, LUA_UTF8LIBNAME, luaopen_utf8,   1); lua_pop(L, 1);

    static const char *const stripped_globals[] = {
        "dofile", "loadfile", "load", "require", "pcall", "xpcall",
        "setmetatable",
    };
    for (const char *name : stripped_globals) strip_global(L, name);

    badge_lua_install(L, ctx);
    return 0;
}

struct MainChunk {
    const char *path;
};

int run_main_chunk(lua_State *L, LuaCtx *, void *userdata)
{
    const MainChunk *chunk = static_cast<const MainChunk *>(userdata);
    if (luaL_loadfilex(L, chunk->path, "t") != LUA_OK) return lua_error(L);
    lua_call(L, 0, 0);
    return 0;
}

struct CallbackRefs {
    int *on_enter;
    int *on_tick;
    int *on_button;
    int *on_exit;
};

void capture_callback(lua_State *L, const char *global, int *ref)
{
    lua_pushglobaltable(L);
    lua_pushstring(L, global);
    lua_rawget(L, -2);
    lua_remove(L, -2);
    if (lua_isfunction(L, -1)) {
        *ref = luaL_ref(L, LUA_REGISTRYINDEX);
    } else {
        lua_pop(L, 1);
        *ref = LUA_NOREF;
    }
}

int capture_callbacks(lua_State *L, LuaCtx *, void *userdata)
{
    CallbackRefs *refs = static_cast<CallbackRefs *>(userdata);
    capture_callback(L, "on_enter", refs->on_enter);
    capture_callback(L, "on_tick", refs->on_tick);
    capture_callback(L, "on_button", refs->on_button);
    capture_callback(L, "on_exit", refs->on_exit);
    return 0;
}

struct CallbackInvocation {
    int ref;
    bool push_root;
    const ButtonEvent *button_event;
};

int invoke_callback(lua_State *L, LuaCtx *, void *userdata)
{
    const CallbackInvocation *invocation =
        static_cast<const CallbackInvocation *>(userdata);
    lua_rawgeti(L, LUA_REGISTRYINDEX, invocation->ref);
    if (!lua_isfunction(L, -1)) {
        return luaL_error(L, "Lua callback reference is invalid");
    }

    int nargs = 0;
    if (invocation->push_root) {
        if (!badge_lua_push_root(L)) {
            return luaL_error(L, "root handle allocation failed");
        }
        nargs = 1;
    } else if (invocation->button_event) {
        lua_pushinteger(L, static_cast<lua_Integer>(invocation->button_event->button));
        lua_pushinteger(L, static_cast<lua_Integer>(invocation->button_event->kind));
        nargs = 2;
    }

    lua_call(L, nargs, 0);
    return 0;
}

// One protected call per tick: drain up to 4 queued radio frames, then run
// on_tick, all inside the same tick budget. Capping the radio batch prevents
// a chatty channel from draining the queue forever before on_tick can run.
struct TickState {
    int tick_ref;
};

int dispatch_tick(lua_State *L, LuaCtx *ctx, void *userdata)
{
    const TickState *state = static_cast<const TickState *>(userdata);
    if (ctx->radio_recv_ref != LUA_NOREF
        && badge_lua_radio_has_frames(ctx)) {
        badge_lua_radio_deliver(L, ctx, nullptr);
    }
    if (state->tick_ref != LUA_NOREF) {
        CallbackInvocation invocation = { state->tick_ref, false, nullptr };
        return invoke_callback(L, ctx, &invocation);
    }
    return 0;
}

std::uint32_t fnv1a_append(std::uint32_t hash, const char *text)
{
    const unsigned char *p = reinterpret_cast<const unsigned char *>(text);
    while (*p) {
        hash ^= *p++;
        hash *= 16777619u;
    }
    return hash;
}

void copy_bounded(char *dst, std::size_t dst_size, const char *src)
{
    if (!src) src = "Lua error without a message";
    std::size_t len = std::strlen(src);
    if (len >= dst_size) len = dst_size - 1;
    std::memcpy(dst, src, len);
    dst[len] = '\0';
}

void format_memory_error(char *out, std::size_t out_size, const LuaCtx &ctx)
{
    std::snprintf(out, out_size,
                  "Lua memory limit exceeded (used %zu / limit %zu, peak %zu)",
                  ctx.heap_current, ctx.heap_limit, ctx.heap_peak);
}

} // namespace

ScriptApp::ScriptApp(std::string dir,
                     std::string main_path,
                     std::string slug,
                     std::string name,
                     std::string icon,
                     std::string icon_path,
                     std::uint8_t api,
                     std::uint8_t heap_kb,
                     bool wake_lock,
                     bool home_button,
                     bool confirm_home,
                     std::string version,
                     std::string author)
    : dir_(std::move(dir)),
      main_path_(std::move(main_path)),
      slug_(std::move(slug)),
      name_(std::move(name)),
      icon_(std::move(icon)),
      icon_path_(std::move(icon_path)),
      api_(api),
      heap_kb_(heap_kb),
      wake_lock_(wake_lock),
      home_button_(home_button),
      confirm_home_(confirm_home),
      version_(std::move(version)),
      author_(std::move(author))
{
}

ScriptApp *ScriptApp::load(const char *dir)
{
    // Stack budget: this runs on the 3584-byte main task at boot (and on the
    // console task via reload). The old three 512-byte path buffers plus
    // newlib vfprintf overflowed it the first time real app dirs existed.
    // All load paths fit in 128 bytes (dir <= 47, longest leaf 12 + NUL).
    if (!dir || !*dir) return nullptr;

    struct stat st;
    if (stat(dir, &st) != 0 || !S_ISDIR(st.st_mode)) return nullptr;

    char path[kLoadPathBytes];
    if (!join_path(path, sizeof(path), dir, "manifest.cfg")) {
        ESP_LOGW(TAG, "skipping script directory: manifest path is too long");
        return nullptr;
    }
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
        ESP_LOGW(TAG, "skipping %s: manifest.cfg is not a regular file", dir);
        return nullptr;
    }

    Manifest manifest;
    char why[128] = {};
    if (!read_manifest(path, &manifest, why, sizeof(why))) {
        ESP_LOGW(TAG, "skipping %s: %s", dir, why);
        return nullptr;
    }

    const char *basename = std::strrchr(dir, '/');
    basename = basename ? basename + 1 : dir;
    if (!*basename || manifest.slug != basename) {
        ESP_LOGW(TAG, "skipping %s: folder basename must equal slug '%s'",
                 dir, manifest.slug.c_str());
        return nullptr;
    }

    if (!join_path(path, sizeof(path), dir, "main.lua")) {
        ESP_LOGW(TAG, "skipping %s: main.lua path is too long", dir);
        return nullptr;
    }
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
        ESP_LOGW(TAG, "skipping %s: main.lua is not a regular file", dir);
        return nullptr;
    }
    if (st.st_size < 0 || st.st_size > kMaxMainLuaBytes) {
        ESP_LOGW(TAG, "skipping %s: main.lua exceeds 64 KiB", dir);
        return nullptr;
    }
    const std::string main_path(path);

    std::string icon_path;
    if (!join_path(path, sizeof(path), dir, "icon.bin")) {
        ESP_LOGW(TAG, "skipping %s: icon.bin path is too long", dir);
        return nullptr;
    }
    if (stat(path, &st) == 0 && S_ISREG(st.st_mode)) {
        icon_path = "A:";
        icon_path += path;
    }

    ESP_LOGI(TAG, "loaded lua app: %s (%s)", manifest.name.c_str(), manifest.slug.c_str());
    return new ScriptApp(dir,
                         main_path,
                         std::move(manifest.slug),
                         std::move(manifest.name),
                         std::move(manifest.icon),
                         std::move(icon_path),
                         manifest.api,
                         manifest.heap_kb,
                         manifest.wake_lock,
                         manifest.home_button,
                         manifest.confirm_home,
                         std::move(manifest.version),
                         std::move(manifest.author));
}

bool ScriptApp::protected_call(BadgeLuaProtectedFunction function, void *userdata,
                               std::int64_t budget_us, const char *what,
                               const char *hint)
{
    lua_State *L = lua_ctx_.L;
    int status = badge_lua_guard_run(L, function, userdata, budget_us);
    if (status != LUA_OK) {
        report_lua_error(what, -1, status, hint);
        lua_pop(L, 1);
        return false;
    }
    return true;
}

void ScriptApp::report_lua_error(const char *what, int stack_index, int status,
                                 const char *hint)
{
    const bool has_message = lua_type(lua_ctx_.L, stack_index) == LUA_TSTRING;
    const bool has_traceback = has_message && !lua_ctx_.traceback_failed
        && status != LUA_ERRMEM;
    const char *message = nullptr;
    char fallback[160];

    if (lua_ctx_.timed_out && !has_traceback) {
        message = "Lua execution deadline exceeded";
    } else if (lua_ctx_.stack_guard_tripped && !has_traceback) {
        message = "Lua stack safety limit reached";
    } else if (status == LUA_ERRMEM
               || (lua_ctx_.traceback_failed
                   && lua_ctx_.traceback_allocation_failed)) {
        format_memory_error(fallback, sizeof(fallback), lua_ctx_);
        message = fallback;
    } else {
        message = has_message ? lua_tostring(lua_ctx_.L, stack_index)
                              : "Lua error object is not a string";
    }
    report_error(what, message, hint);
}

void ScriptApp::report_error(const char *what, const char *message, const char *hint)
{
    // Own the text before the Lua stack is cleaned up or the state is closed.
    copy_bounded(error_message_, sizeof(error_message_), message);

    std::uint32_t hash = fnv1a_append(2166136261u, what);
    hash ^= 0xffu;
    hash *= 16777619u;
    hash = fnv1a_append(hash, error_message_);

    if (has_error_ && hash == error_hash_) {
        if (error_repeat_count_ != 0xffffffffu) ++error_repeat_count_;
    } else {
        has_error_ = true;
        error_hash_ = hash;
        error_repeat_count_ = 1;
    }

    std::int64_t now = esp_timer_get_time();
    if (!has_logged_error_
        || hash != last_logged_hash_
        || now - last_logged_us_ >= kLogRepeatIntervalUs) {
        ESP_LOGE(TAG, "[%s] %s: %s (hash %08lx, occurrence %lu)",
                 slug_.c_str(), what, error_message_, (unsigned long)hash,
                 (unsigned long)error_repeat_count_);
        has_logged_error_ = true;
        last_logged_hash_ = hash;
        last_logged_us_ = now;
    }

    ui_error_card_show(what, error_message_, error_repeat_count_, hint);
}

void ScriptApp::handle_pending_actions()
{
    if (!lua_ctx_.L) return;
    if (lua_ctx_.pending_nfc_disable) {
        lua_ctx_.pending_nfc_disable = false;
        lua_ctx_.nfc_enabled_by_lua = false;
        hal_nfc_disable();
    }
    if (lua_ctx_.pending_radio_disable) {
        lua_ctx_.pending_radio_disable = false;
        badge_lua_radio_cleanup(&lua_ctx_);
    }
    if (lua_ctx_.pending_exit) {
        lua_ctx_.pending_exit = false;
        // Same call C++ apps make from on_button/on_tick; the registry
        // re-locks and runs our on_exit on the way out.
        app_registry_return_to_launcher();
    }
}

void ScriptApp::clear_error()
{
    error_message_[0] = '\0';
    error_hash_ = 0;
    error_repeat_count_ = 0;
    has_error_ = false;
    ui_error_card_hide();
}

void ScriptApp::close_lua(bool call_on_exit)
{
    if (!lua_ctx_.L) {
        badge_lua_hw_cleanup_no_context();
        lua_ctx_.held_mask = 0;
        return;
    }

    // on_exit still has a fully live object model. Once it returns, no further
    // lifecycle dispatch is allowed and every LVGL delete callback runs before
    // lua_close can invalidate the registry maps it updates.
    if (call_on_exit && on_exit_ref_ != LUA_NOREF) {
        CallbackInvocation invocation = { on_exit_ref_, false, nullptr };
        protected_call(invoke_callback, &invocation, kOnExitBudgetUs,
                       "on_exit", "");
    }

    // Neither invokes Lua, so both stay reliable after a broken on_exit.
    // Radio first: it clears the NimBLE receive callback before teardown.
    badge_lua_radio_cleanup(&lua_ctx_);
    badge_lua_hw_cleanup(&lua_ctx_);
    lua_ctx_.dispatch_enabled = false;
    lua_ctx_.tearing_down = true;

    if (lua_ctx_.root) lv_obj_clean(lua_ctx_.root);
    badge_lua_detach_root(&lua_ctx_);

    lua_ctx_.state_live = false;
    s_last_lua_stats.valid = true;
    copy_bounded(s_last_lua_stats.slug, sizeof(s_last_lua_stats.slug),
                 slug_.c_str());
    s_last_lua_stats.used = lua_ctx_.heap_current;
    s_last_lua_stats.peak = lua_ctx_.heap_peak;
    s_last_lua_stats.limit = lua_ctx_.heap_limit;
    s_last_lua_stats.widgets = lua_ctx_.live_object_count;
    lua_close(lua_ctx_.L);

    if (lua_ctx_.live_object_count != 0 || lua_ctx_.record_count != 0) {
        ESP_LOGE(TAG, "[%s] Lua teardown leaked %lu objects / %lu records",
                 slug_.c_str(), (unsigned long)lua_ctx_.live_object_count,
                 (unsigned long)lua_ctx_.record_count);
    }

    lua_ctx_ = {};
    lua_ctx_.held_mask = 0;
    on_enter_ref_ = on_tick_ref_ = on_button_ref_ = on_exit_ref_ = LUA_NOREF;
}

void ScriptApp::on_enter(lv_obj_t *root)
{
    ui_error_card_prepare();
    ui_error_card_hide();
    error_message_[0] = '\0';
    error_hash_ = 0;
    error_repeat_count_ = 0;
    last_logged_hash_ = 0;
    last_logged_us_ = 0;
    consecutive_tick_errors_ = 0;
    has_error_ = false;
    has_logged_error_ = false;
    tick_suspended_ = false;
    failed_ = false;

    lua_ctx_ = {};
    lua_ctx_.held_mask = 0;
    // luaL_ref can hand out 0-adjacent values, so registry refs must start
    // at LUA_NOREF, not the zero fill. Cleanup unrefs whatever is here on
    // every close path, including a bootstrap that failed mid-install.
    lua_ctx_.radio_recv_ref = LUA_NOREF;
    lua_ctx_.require_cache_ref = LUA_NOREF;
    lua_ctx_.root = root;
    lua_ctx_.slug = slug_.c_str();
    lua_ctx_.dir = dir_.c_str();
    lua_ctx_.app_name = name_.c_str();
    lua_ctx_.api = api_;
    lua_ctx_.heap_kb = heap_kb_;
    lua_ctx_.heap_limit = static_cast<std::size_t>(heap_kb_) * 1024;

    lua_State *L = lua_newstate(badge_lua_alloc, &lua_ctx_, luaL_makeseed(nullptr));
    if (!L) {
        badge_lua_hw_cleanup_no_context();
        char message[160];
        format_memory_error(message, sizeof(message), lua_ctx_);
        report_error("load", message, kExitHint);
        failed_ = true;
        lua_ctx_ = {};
        return;
    }

    badge_lua_guard_setup(L, &lua_ctx_);
    lua_ctx_.state_live = true;

    if (!protected_call(bootstrap_lua, nullptr, kBootstrapBudgetUs,
                        "setup", kExitHint)) {
        failed_ = true;
        close_lua(false);
        return;
    }

    MainChunk main_chunk = { main_path_.c_str() };
    if (!protected_call(run_main_chunk, &main_chunk, kBootstrapBudgetUs,
                        "main.lua", kExitHint)) {
        failed_ = true;
        close_lua(false);
        return;
    }

    CallbackRefs refs = {
        &on_enter_ref_, &on_tick_ref_, &on_button_ref_, &on_exit_ref_,
    };
    if (!protected_call(capture_callbacks, &refs, kBootstrapBudgetUs,
                        "callbacks", kExitHint)) {
        failed_ = true;
        close_lua(false);
        return;
    }
    lua_ctx_.dispatch_enabled = true;

    if (on_enter_ref_ != LUA_NOREF) {
        CallbackInvocation invocation = { on_enter_ref_, true, nullptr };
        if (!protected_call(invoke_callback, &invocation, kOnEnterBudgetUs,
                            "on_enter", kExitHint)) {
            failed_ = true;
            close_lua(false);
        } else {
            handle_pending_actions();
        }
    }
}

void ScriptApp::on_exit()
{
    close_lua(true);
    ui_error_card_hide();
}

void ScriptApp::on_tick()
{
    if (!lua_ctx_.L || !lua_ctx_.dispatch_enabled || failed_ || tick_suspended_) return;
    const bool radio_pending = badge_lua_radio_has_frames(&lua_ctx_);
    if (on_tick_ref_ == LUA_NOREF && !radio_pending) {
        if (has_error_) clear_error();
        consecutive_tick_errors_ = 0;
        return;
    }

    const char *hint = consecutive_tick_errors_ >= 2 ? kRetryHint : kExitHint;
    const char *what = on_tick_ref_ != LUA_NOREF ? "on_tick" : "on_recv";
    TickState state = { on_tick_ref_ };
    if (protected_call(dispatch_tick, &state, kOnTickBudgetUs, what, hint)) {
        if (has_error_) clear_error();
        consecutive_tick_errors_ = 0;
        handle_pending_actions();
        return;
    }

    if (consecutive_tick_errors_ != 0xffu) ++consecutive_tick_errors_;
    if (consecutive_tick_errors_ >= 3) tick_suspended_ = true;
}

void ScriptApp::on_button(ButtonEvent ev)
{
    const std::uint8_t button = static_cast<std::uint8_t>(ev.button);
    if (button < static_cast<std::uint8_t>(Button::COUNT)) {
        const std::uint16_t mask = button_mask(ev.button);
        if (ev.button == Button::Home) {
            // HOME presses can be intercepted by the registry, but its release
            // still reaches the app. Keep HOME out of the script held state.
            lua_ctx_.held_mask &= static_cast<std::uint16_t>(~mask);
        } else if (ev.kind == ButtonEventKind::Pressed) {
            lua_ctx_.held_mask |= mask;
        } else {
            lua_ctx_.held_mask &= static_cast<std::uint16_t>(~mask);
        }
    }

    if (!lua_ctx_.L || !lua_ctx_.dispatch_enabled || failed_) return;
    if (tick_suspended_) {
        if (ev.kind == ButtonEventKind::Pressed && ev.button == Button::A) {
            tick_suspended_ = false;
            consecutive_tick_errors_ = 0;
        }
        return;
    }
    if (on_button_ref_ == LUA_NOREF) return;

    CallbackInvocation invocation = { on_button_ref_, false, &ev };
    if (!protected_call(invoke_callback, &invocation, kOnButtonBudgetUs,
                        "on_button", kRetryHint)) {
        consecutive_tick_errors_ = 0;
        tick_suspended_ = true;
    } else {
        handle_pending_actions();
    }
}

static void rmdir_recursive(const char *path)
{
    DIR *d = opendir(path);
    if (!d) {
        unlink(path);
        return;
    }
    struct dirent *de;
    while ((de = readdir(d)) != nullptr) {
        if (de->d_name[0] == '.' &&
            (de->d_name[1] == '\0' ||
             (de->d_name[1] == '.' && de->d_name[2] == '\0'))) continue;
        char child[kPathBytes];
        if (!join_path(child, sizeof(child), path, de->d_name)) {
            ESP_LOGW(TAG, "not removing overlong child path under %s", path);
            continue;
        }
        struct stat st;
        if (stat(child, &st) == 0 && S_ISDIR(st.st_mode)) {
            rmdir_recursive(child);
        } else {
            unlink(child);
        }
    }
    closedir(d);
    rmdir(path);
}

bool script_app_last_lua_stats(ScriptAppLuaStats *out)
{
    if (!out || !s_last_lua_stats.valid) return false;
    *out = s_last_lua_stats;
    return true;
}

void ScriptApp::on_uninstall()
{
    if (dir_.empty()) return;
    ESP_LOGI(TAG, "uninstalling %s (%s)", slug_.c_str(), dir_.c_str());
    rmdir_recursive(dir_.c_str());
}

void ScriptApp::refresh_launcher_metadata(const ScriptApp &fresh)
{
    name_ = fresh.name_;
    icon_ = fresh.icon_;
    icon_path_ = fresh.icon_path_;
}

void script_app_register_all(const char *apps_dir)
{
    if (!apps_dir || !*apps_dir) return;
    DIR *d = opendir(apps_dir);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d)) != nullptr) {
        if (de->d_name[0] == '.') continue;
        // The bundled countdown timer is disabled for this release, just like
        // Pomodoro in main.cpp. Keep its files so re-enabling is reversible.
        if (std::strcmp(de->d_name, "lua_countdown") == 0) continue;
        char child[kPathBytes];
        if (!join_path(child, sizeof(child), apps_dir, de->d_name)) {
            ESP_LOGW(TAG, "skipping overlong script directory path");
            continue;
        }
        struct stat st;
        if (stat(child, &st) != 0 || !S_ISDIR(st.st_mode)) continue;
        if (ScriptApp *app = ScriptApp::load(child)) {
            if (App *existing = app_registry_find_by_slug(app->slug())) {
                if (existing->is_user_installed()) {
                    static_cast<ScriptApp *>(existing)->refresh_launcher_metadata(*app);
                } else {
                    ESP_LOGW(TAG, "skipping %s: slug '%s' is already registered",
                             child, app->slug());
                }
                delete app;
                continue;
            }
            app_registry_add(app);
            // A directory under /littlefs/apps implies install; no NFC scan is
            // required, unlike built-ins that remain gated by their card.
            launcher_slugs_append(app->slug());
        }
    }
    closedir(d);
}
