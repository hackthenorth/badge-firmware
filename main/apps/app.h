#pragma once

#include "lvgl.h"
#include "hal_buttons.h"
#include "ui_theme.h"

// The single source of truth for who may see an app. There is deliberately no
// second visibility flag: tier decides both whether an app can be unlocked and
// whether it needs unlocking at all.
enum class AppAccess : uint8_t {
    Default,   // Core attendee experience. Always present in the launcher
               // order and movable, but never dropped or gated behind unlocks.
    Public,    // Unlockable through event interactions or app-specific secrets.
    Internal,  // Staff/diagnostic tooling; never unlocked by public tags.
};

// App lifecycle hooks all run under the LVGL lock. If an app spawns its
// own FreeRTOS task, that task must lvgl_port_lock() before touching LVGL.
// Keep registered app objects small: allocate large working buffers in
// on_enter(), handle allocation failure, and release them in on_exit().
// The registry retains metadata objects for launcher lookup throughout boot.
class App {
public:
    virtual ~App() = default;
    virtual const char *name() const = 0;
    virtual const char *icon() const = 0; // 1-3 chars for grid cell
    virtual const char *slug() const = 0;

    // Optional embedded image icon. Takes precedence over icon().
    virtual const lv_image_dsc_t *icon_image() const { return nullptr; }
    // Monochrome PNG stencils use only alpha and render in the launcher text color.
    virtual bool icon_is_mask() const { return false; }
    // One-pixel white silhouette behind dark embedded launcher artwork.
    virtual bool icon_needs_outline() const { return false; }
    // Optional LVGL-FS path for an on-disk icon (e.g. "A:/littlefs/apps/foo/icon.bin").
    virtual const char *icon_image_path() const { return nullptr; }

    // Access tier. Apps are unlockable Public tools unless they say otherwise:
    // override to Default for the always-present boot bundle (My Badge,
    // Scanner, ...) or to Internal for staff and diagnostic tooling.
    virtual AppAccess access() const { return AppAccess::Public; }

    // Convenience predicate over access(); the launcher keeps these apps in
    // its persisted order but excludes them from Drop.
    bool is_default_app() const { return access() == AppAccess::Default; }

    // NFC apps need a clean heap and fresh field on entry from the launcher.
    virtual bool uses_nfc() const { return false; }

    // Reboot once before entering from the launcher. Radio-heavy apps can
    // opt in without claiming NFC use. Boot routing bypasses this once, and
    // direct app-to-app handoffs preserve their in-memory payloads.
    virtual bool needs_fresh_boot() const { return uses_nfc(); }

    // True if "uninstall" means a hard delete (Lua app, files removed);
    // false means "drop" — recoverable via re-scanning the NFC card.
    virtual bool is_user_installed() const { return false; }

    // Called when the user confirms uninstall. Default is no-op (built-in
    // apps just leave the launcher list). ScriptApp overrides to delete its
    // source directory.
    virtual void on_uninstall() {}

    // True while this app must not be interrupted by the sleep manager —
    // games, timers, light shows, anything the user is watching rather than
    // touching. Idle-timeout sleep is suppressed while the active app
    // returns true. Keep it honest: a permanent wake lock flattens a battery.
    virtual bool wants_wake_lock() const { return false; }

    // App update cadence. Most UI only needs 50 Hz; sensor-heavy apps can
    // request a shorter period without making every app run that quickly.
    virtual uint32_t tick_period_ms() const { return 20; }

    // B is reserved for actions within the app, including dismissing subpages;
    // it must never return to the launcher. HOME is the only exit control.
    // Guided hardware flows
    // can opt in to receiving HOME so they can verify the switch itself.
    virtual bool handles_home_button() const { return false; }

    // Persist any in-progress score or other lightweight progress. The
    // registry calls this before showing the Home pause and before on_exit().
    // Real-time games also call it when their own pause overlay opens.
    virtual void checkpoint_progress() {}

    // Games ask before leaving. The registry consumes the confirmation and
    // cancellation buttons, and pauses app ticks while the dialog is open.
    virtual bool confirm_home_exit() const { return false; }
    virtual void on_home_confirmation_cancel(uint32_t /*paused_ms*/) {}
    // Networked games may keep their session alive while asking to leave.
    virtual void on_home_confirmation_tick() {}

    virtual void on_enter(lv_obj_t *root) = 0;
    virtual void on_exit() {}
    virtual void on_tick() {}
    virtual void on_button(ButtonEvent /*ev*/) {}
};

// Use this base for games and play apps so new games share the Home contract.
class GameApp : public App {
public:
    bool confirm_home_exit() const override { return true; }
};
