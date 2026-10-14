#include "hal_display.h"
#include "hal_i2c.h"
#include "hal_buttons.h"
#include "hal_accel.h"
#include "hal_lights.h"
#include "hal_fs.h"
#include "hal_config.h"
#include "hal_identity.h"
#include "hal_sleep.h"
#include "solana_wallet.h"
#include "esp_log.h"
#include "esp_system.h"
#include "hal_nfc.h"
#include "hal_console.h"

#include <cstdio>
#include "app_registry.h"
#include "app_launcher.h"
#include "app_reboot.h"
#include "script_app.h"
#include "app_registry.h"
#include "launcher_slugs.h"
#include "boot_splash.h"

// Alternative boot flow for factory hardware testing (main/test.cpp). Always
// compiled; only entered when the factory.enabled config flag is set.
void factory_test_run();

// Engineer/factory firmware toggle. Build with BADGE_FACTORY_TEST=1 in the
// environment (see main/CMakeLists.txt) to boot straight into the factory
// hardware test instead of the launcher. Defaults off for normal builds.
#ifndef FACTORY_TEST_BUILD
#define FACTORY_TEST_BUILD 0
#endif

extern "C" void app_main(void)
{
    // Log why we booted before anything can clear it: distinguishes a
    // brownout reset (power) from panics/watchdogs (software) after an NFC
    // repro on batteries. Numeric enum from esp_system.h; 9 = BROWNOUT.
    const esp_reset_reason_t boot_reason = esp_reset_reason();
    ESP_LOGI("boot", "reset reason=%d%s", (int)boot_reason,
             boot_reason == ESP_RST_BROWNOUT ? " (BROWNOUT)" : "");
    // Journaled below into the NFC journal once LittleFS is mounted (see
    // hal_fs_init() above); the numeric log line above fires first so the
    // reason is captured even if the mount fails.
    // Take control of the LCD first. Warm resets can otherwise leave its old
    // frame visible while filesystem, identity, and app state initialize.
    // (hal_display_init blanks the panel and keeps it dark until
    // hal_display_show(); the splash card is only what first lights it.)
    hal_display_init();

    // Filesystem before the first frame: the reboot intent (OOM / NFC-enter /
    // radio-NFC-exit) is consumed here so a reboot boot never paints the
    // splash card at all — it powers the panel on straight into the
    // destination below.
    hal_fs_init();
    hal_config_init();
    AppRebootPending reboot_pending{};
    const bool have_reboot = app_reboot_consume_pending(&reboot_pending);
    const bool skip_splash_boot =
        have_reboot && (reboot_pending.has_target || reboot_pending.has_focus);
    if (!FACTORY_TEST_BUILD && !skip_splash_boot) boot_splash_show();
    char boot_stage[32];
    snprintf(boot_stage, sizeof(boot_stage), "boot rr=%d", (int)boot_reason);
    hal_nfc_journal(boot_stage);
    hal_identity_init();   // reads /littlefs/identity.json; unprovisioned is OK
    solana_wallet_init();  // keypair generation is deferred: it needs the radio up
    launcher_slugs_load();
    hal_i2c_init();
    hal_buttons_init();
    hal_accel_init();
    hal_lights_init();
    hal_sleep_init();
    // NFC and Radio HALs are enabled on demand by apps (see hal_nfc.h /
    // hal_radio.h). The registry force-disables both on every app switch.

    // Factory line build: a runtime flag hands the whole device to the factory
    // hardware test instead of the normal launcher. Operators set
    // factory.enabled=1 via the config console command, reboot, run the test.
    // if (hal_config_get_int("factory", "enabled", 0)) {
    if (FACTORY_TEST_BUILD) {
        factory_test_run();
        hal_display_show();
        hal_console_start();
        return;
    }

    // Default launcher order starts with My Badge (QR), Connect, Scanner,
    // Sync, Share, then the hunt and the remaining event tools.
    // My Badge is also the boot target below. Saved custom orders take priority.
    app_registry_add(app_badge_id_instance());
    app_registry_add(app_connect_instance());
    app_registry_add(app_scanner_instance());
    app_registry_add(app_sync_instance());
    app_registry_add(app_share_instance());
    app_registry_add(app_blindbox_instance());
    app_registry_add(app_sponsors_instance());
    app_registry_add(app_workshops_instance());
    app_registry_add(app_mood_instance());
    app_registry_add(app_socials_instance());
    app_registry_add(app_schedule_instance());
    app_registry_add(app_rules_instance());
    app_registry_add(app_museum_instance());
    app_registry_add(app_progress_instance());
    app_registry_add(app_snake_instance());
    app_registry_add(app_dice_instance());

    // Public apps, unlocked individually through event interactions.
    // app_registry_add(app_pomodoro_instance());
    app_registry_add(app_67_instance());
    app_registry_add(app_level_instance());
    app_registry_add(app_eightball_instance());
    app_registry_add(app_conversation_instance());
    app_registry_add(app_lightshow_instance());
    app_registry_add(app_tetris_instance());
    app_registry_add(app_2048_instance());
    app_registry_add(app_breakout_instance());
    app_registry_add(app_tictactoe_instance());
    app_registry_add(app_chess_instance());
    app_registry_add(app_maze_instance());
    app_registry_add(app_flappy_instance());
    app_registry_add(app_bubble_wrap_instance());
    app_registry_add(app_ktnh_instance());
    app_registry_add(app_hardware_physics_instance());
    app_registry_add(app_teardown_instance());
    app_registry_add(app_flashlight_instance());
    app_registry_add(app_blackjack_instance());

    // Internal tools. These are registered but hidden unless an engineer
    // selects the diagnostic flasher mode or uses the provisioning console.
    app_registry_add(app_accel_instance());
    app_registry_add(app_test_instance());
    app_registry_add(app_lanyards_instance());
    app_registry_add(app_directory_instance());
    app_registry_add(app_files_instance());
    app_registry_add(app_diagnostics_instance());
    app_registry_add(app_meminfo_instance());
    // v3 batch: reaction, party games, toys, dance (all Public/unlockable).
    app_registry_add(app_reaction_instance());
    app_registry_add(app_whack_instance());
    app_registry_add(app_pong_instance());
    app_registry_add(app_slots_instance());
    app_registry_add(app_hangman_instance());
    app_registry_add(app_mines_instance());
    app_registry_add(app_fortune_instance());
    app_registry_add(app_etch_instance());
    app_registry_add(app_kaleido_instance());
    app_registry_add(app_lava_instance());
    app_registry_add(app_dance_instance());
    app_registry_add(app_sokoban_instance());
    app_registry_add(app_hopper_instance());
    app_registry_add(app_jetpack_instance());
    app_registry_add(app_invaders_instance());
    app_registry_add(app_blocks_instance());
    app_registry_add(app_outrun_instance());
    // app_registry_add(app_power_instance());
    // app_registry_add(app_buttons_instance());
    // app_registry_add(app_nfc_instance());
    // app_registry_add(app_lights_instance());
    // app_registry_add(app_kitchen_instance());
    // app_registry_add(app_simon_instance());

    // User-installed Lua apps (each subdir of /littlefs/apps with a manifest).
    script_app_register_all("/littlefs/apps");

    // Seed or migrate the launcher order only after every built-in and script
    // app has registered, so the always-present apps can participate in
    // the same persisted ordering as unlocked apps.
    launcher_slugs_apply_defaults();

    // Drop slugs that no longer resolve to a registered app — e.g. a built-in
    // removed from the firmware, or a Lua app whose dir was deleted.
    launcher_slugs_prune([](const char *s) {
        return app_registry_find_by_slug(s) != nullptr;
    });

    // A reboot intent only redirects a provisioned, onboarded badge: a pending
    // target resolves against the registry after pruning, so a slug whose app
    // is gone falls back to the normal boot instead of stranding the badge.
    App *reboot_target = nullptr;
    if (have_reboot && reboot_pending.has_target) {
        reboot_target = app_registry_find_by_slug(reboot_pending.target);
        if (!reboot_target) {
            ESP_LOGW("boot", "reboot target '%s' gone; normal boot",
                     reboot_pending.target);
        }
    }
    const bool reboot_to_launcher =
        have_reboot && !reboot_target && reboot_pending.has_focus;

    // Registration is the first gate. An unprovisioned badge boots to its red
    // status card; START can open the hardware self-test, but no attendee apps
    // or terms flow are reachable yet. Only a provisioned badge may onboard.
    bool skip_splash = false;
    if (!hal_identity_is_provisioned()) {
        app_registry_start(app_launcher_instance(), app_badge_id_instance());
    } else if (badge_onboarding_required()) {
        // Onboarding runs AS the shell, which makes it modal: the registry only
        // lets HOME out of an app when that app isn't the shell. Onboarding hands
        // over to the real launcher when it completes.
        app_registry_start(app_onboarding_instance());
    } else if (reboot_target) {
        // OOM / NFC-enter reboot: straight into the app on a fresh heap. The
        // bypass keeps the registry's reboot-enter rule from restarting out.
        ESP_LOGI("boot", "reboot into %s", reboot_pending.target);
        app_registry_start(app_launcher_instance(), reboot_target, true);
        skip_splash = true;
    } else if (reboot_to_launcher) {
        // Seamless reboot-exit: the grid itself (not the badge ID page), with
        // the cursor restored onto the exited radio/NFC app by the launcher.
        ESP_LOGI("boot", "reboot to launcher focus=%s", reboot_pending.focus);
        app_registry_start(app_launcher_instance(), app_launcher_instance());
        skip_splash = true;
    } else {
        // The launcher is where the HOME button lands; the ID page is what
        // the user actually sees on boot.
        app_registry_start(app_launcher_instance(), app_badge_id_instance());
    }

    // Normal boots: the destination above is fully rendered underneath the
    // partnership card. Release it once ready, but keep it up until its
    // one-second minimum has elapsed so there is never an intermediate boot
    // frame. Reboot boots never painted the card (panel still dark), so
    // reveal the destination directly: paint it and power the panel on.
    if (skip_splash) hal_display_show();
    else boot_splash_finish();

    hal_console_start();
}
