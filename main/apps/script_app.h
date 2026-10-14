#pragma once

#include "app.h"
#include "lua_internal.h"

#include <cstddef>
#include <cstdint>
#include <string>

extern "C" {
#include "lua.h"
#include "lauxlib.h"
}

// Wraps a `/littlefs/apps/<slug>/` directory as an App by loading its
// manifest.cfg + main.lua and forwarding lifecycle hooks into Lua callbacks.
//
// Each on_enter creates a fresh lua_State so a script that wedges itself
// can be recovered by exiting and re-entering. on_exit closes the state.
class ScriptApp : public App {
public:
    static ScriptApp *load(const char *dir);
    void refresh_launcher_metadata(const ScriptApp &fresh);

    const char *name() const override { return name_.c_str(); }
    const char *icon() const override { return icon_.c_str(); }
    const char *slug() const override { return slug_.c_str(); }
    const char *icon_image_path() const override {
        return icon_path_.empty() ? nullptr : icon_path_.c_str();
    }
    bool wants_wake_lock() const override { return wake_lock_; }
    bool confirm_home_exit() const override {
        return confirm_home_ && !failed_ && !tick_suspended_;
    }
    bool handles_home_button() const override {
        return home_button_ && !failed_ && !tick_suspended_;
    }

    bool is_user_installed() const override { return true; }
    void on_uninstall() override;

    void on_enter(lv_obj_t *root) override;
    void on_exit() override;
    void on_tick() override;
    void on_button(ButtonEvent ev) override;

private:
    static constexpr std::size_t kErrorMessageBytes = 801;

    ScriptApp(std::string dir,
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
              std::string author);

    bool protected_call(BadgeLuaProtectedFunction function, void *userdata,
                        std::int64_t budget_us, const char *what,
                        const char *hint);
    void report_lua_error(const char *what, int stack_index, int status,
                          const char *hint);
    void report_error(const char *what, const char *message, const char *hint);
    // Runs deferred Lua requests (nfc/radio disable, app exit) after the
    // protected call that raised them has returned.
    void handle_pending_actions();
    void clear_error();
    void close_lua(bool call_on_exit);

    std::string dir_;
    std::string main_path_;
    std::string slug_;
    std::string name_;
    std::string icon_;
    std::string icon_path_;
    std::uint8_t api_ = 1;
    std::uint8_t heap_kb_ = 48;
    bool wake_lock_ = false;
    bool home_button_ = false;
    bool confirm_home_ = false;
    std::string version_;
    std::string author_;

    LuaCtx lua_ctx_ = {};
    int on_enter_ref_  = LUA_NOREF;
    int on_tick_ref_   = LUA_NOREF;
    int on_button_ref_ = LUA_NOREF;
    int on_exit_ref_   = LUA_NOREF;

    char error_message_[kErrorMessageBytes] = {};
    std::uint32_t error_hash_ = 0;
    std::uint32_t error_repeat_count_ = 0;
    std::uint32_t last_logged_hash_ = 0;
    std::int64_t last_logged_us_ = 0;
    std::uint8_t consecutive_tick_errors_ = 0;
    bool has_error_ = false;
    bool has_logged_error_ = false;
    bool tick_suspended_ = false;
    bool failed_ = false;
};

// Walks /littlefs/apps/, calls ScriptApp::load on each subdir, and adds
// successful loads to the app registry. Safe to call when the directory
// is missing.
void script_app_register_all(const char *apps_dir);

// Last-run Lua heap snapshot: the C++ twin of badge.sys.stats(). A script's
// own state is closed by the time diagnostics runs, so close_lua stashes
// these counters for it. Returns false if no Lua app has run yet.
struct ScriptAppLuaStats {
    char slug[33];
    std::size_t used;
    std::size_t peak;
    std::size_t limit;
    std::uint32_t widgets;
    bool valid;
};
bool script_app_last_lua_stats(ScriptAppLuaStats *out);
