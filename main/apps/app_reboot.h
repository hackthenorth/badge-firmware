#pragma once

#include <cstddef>

class App;

// Reboot-into-app plumbing: lets an app trade its fragmented heap for a fresh
// boot and land straight back where it was, without the boot card in between.
//
// Two flows share one persisted intent (/littlefs/config/reboot.cfg):
//   OOM / NFC-enter ... app_reboot_into_app() reboots and boots straight into
//                       the app with the splash dismissed immediately.
//   radio/NFC exit .... app_reboot_to_launcher() reboots and boots to the
//                       launcher grid with the cursor on the exited app.
//
// Brownout hardening: the intent is consumed (file deleted) before it is
// honored, and honored only when the reset reason is ESP_RST_SW — i.e. our
// own esp_restart(). A brownout, power cycle, panic, or watchdog on the way
// in or out clears the intent and boots normally to the launcher. Each
// request therefore fires at most once; there is no reboot loop.
struct AppRebootPending {
    char target[64]; // slug to boot into; empty means the launcher
    char focus[64];  // launcher slug to focus when target is empty
    bool has_target = false;
    bool has_focus = false;
};

// Persist the intent and restart. Never return on success; return false only
// when the intent could not be persisted (caller falls back to a direct
// switch or an error card). Safe to call with the LVGL lock held.
bool app_reboot_into_slug(const char *slug);
bool app_reboot_into_app(App *app);
bool app_reboot_to_launcher(const char *focus_slug);

// Boot path (main.cpp, after hal_fs_init()/hal_config_init()): consume the
// persisted intent exactly once into *out. Returns true when a reboot boot
// was requested (target and/or focus set by our own SW restart).
bool app_reboot_consume_pending(AppRebootPending *out);

// Launcher path: one-shot restore of the focus slug stashed by the consume
// above. Returns true and clears the stash; false when there is nothing.
bool app_reboot_take_launcher_focus(char *slug_out, size_t slug_sz);
