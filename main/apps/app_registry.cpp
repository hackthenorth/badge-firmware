#include "app_registry.h"
#include "app.h"
#include "app_reboot.h"
#include "hal_display.h"
#include "hal_sleep.h"
#include "hal_identity.h"
#include "hal_lights.h"
#include "hal_buttons.h"
#include "hal_nfc.h"
#include "hal_radio.h"
#include "esp_lvgl_port.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

#include <cstring>
#include <vector>

static const char *TAG = "app_reg";

static std::vector<App *> &apps()
{
    static std::vector<App *> v; // construct-on-first-use
    return v;
}

static uint16_t s_pressed_buttons = 0;
static uint16_t s_suppress_until_release = 0;
static App *s_active = nullptr;
static App *s_launcher = nullptr;
// One-shot: the post-reboot boot target skips the reboot-enter rule once, so
// the boot that just rebooted into an NFC app does not restart out of it.
static bool s_reboot_bypass = false;
static lv_timer_t *s_tick_timer = nullptr;
static lv_obj_t *s_home_confirmation = nullptr;
static uint32_t s_home_confirmation_started_ms = 0;

static void close_home_confirmation()
{
    if (s_home_confirmation) lv_obj_delete(s_home_confirmation);
    s_home_confirmation = nullptr;
}

static void show_home_confirmation()
{
    lv_obj_t *root = hal_display_root();
    if (!root) return;
    s_home_confirmation_started_ms = lv_tick_get();
    s_home_confirmation = lv_obj_create(root);
    lv_obj_remove_style_all(s_home_confirmation);
    lv_obj_set_size(s_home_confirmation, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(s_home_confirmation, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_home_confirmation, LV_OPA_70, 0);
    lv_obj_clear_flag(s_home_confirmation, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_home_confirmation, [](lv_event_t *) {
        s_home_confirmation = nullptr;
    }, LV_EVENT_DELETE, nullptr);

    lv_obj_t *card = lv_obj_create(s_home_confirmation);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, 270, 116);
    lv_obj_center(card);
    lv_obj_set_style_radius(card, 12, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(BadgeTheme::BACKGROUND), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(card, 2, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(BadgeTheme::ACCENT_DETAIL), 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *text = lv_label_create(card);
    lv_obj_set_width(text, 246);
    lv_label_set_text(text, "Return to Apps?\nHome to confirm\nAny other button to resume");
    lv_obj_set_style_text_font(text, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(text, lv_color_hex(BadgeTheme::TEXT), 0);
    lv_obj_set_style_text_align(text, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(text);
}

void app_registry_add(App *app)
{
    if (!app) return;
    apps().push_back(app);
}

size_t app_registry_count()
{
    return apps().size();
}

App *app_registry_get(size_t i)
{
    return i < apps().size() ? apps()[i] : nullptr;
}

App *app_registry_find_by_slug(const char *slug)
{
    if (!slug) return nullptr;
    for (App *a : apps()) {
        if (a && a->slug() && strcmp(a->slug(), slug) == 0) return a;
    }
    return nullptr;
}

void app_registry_remove(App *app)
{
    if (!app) return;
    auto &v = apps();
    for (auto it = v.begin(); it != v.end(); ++it) {
        if (*it == app) { v.erase(it); break; }
    }
    delete app;
}

void app_registry_launch(App *a)
{
    if (!a) return;
    lv_obj_t *root = hal_display_root();
    if (!root) return;

    const bool bypass = s_reboot_bypass;
    s_reboot_bypass = false;
    // Reboot-enter: NFC and memory-heavy radio apps start on a clean heap
    // (and fresh NFC field). Scoped to
    // launcher exits only — app-to-app handoffs (scanner<->sponsors payload
    // relays) stay direct because their relay state lives in DRAM and would
    // not survive a restart.
    if (!bypass && s_launcher && a != s_launcher && a != s_active
        && s_active == s_launcher && a->needs_fresh_boot()) {
        ESP_LOGI(TAG, "reboot-enter %s", a->slug());
        if (app_reboot_into_app(a)) return; // restarts; no return on success
        ESP_LOGW(TAG, "reboot-enter persist failed; direct launch");
    }

    // Snapshot while the exiting app still owns the hardware: a radio/NFC
    // session exiting to the launcher reboots for a clean heap instead of
    // switching directly. Sampled before on_exit/disable below.
    App *exiting = s_active;
    const bool hw_session = exiting && exiting != s_launcher
        && (hal_nfc_is_enabled() || hal_radio_is_enabled());

    // Heap tripwire: per-app-switch free/largest, so a leak or spike is
    // attributed to the app that caused it instead of wherever it OOMs.
    auto heap_line = [](const char *what, const char *name) {
        ESP_LOGI(TAG, "heap %s %s: free=%u largest=%u", what, name ? name : "?",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL));
    };
    lvgl_port_lock(0);
    close_home_confirmation();
    if (s_active) {
        s_active->checkpoint_progress();
        s_active->on_exit();
    }
    heap_line("after exit", s_active ? s_active->name() : nullptr);
    // Force-disable power-hungry HALs between apps. The new app must call
    // hal_*_enable() in on_enter() if it needs them.
    hal_nfc_disable();
    hal_radio_disable();
    lv_obj_clean(root);
    // Reboot-exit: the exiting session ran radio/NFC, so hand the launcher a
    // fresh boot with the cursor restored onto the exited app. The exiting
    // app's on_exit/checkpoint above already ran, so this is seamless.
    if (!bypass && hw_session && s_launcher && a == s_launcher) {
        const char *slug = exiting->slug();
        ESP_LOGI(TAG, "reboot-exit %s", slug ? slug : "?");
        // Unlock around the persist + restart: the spill is one small
        // synchronous write, and the restart never returns. On persist
        // failure relock and fall through to a direct launch.
        lvgl_port_unlock();
        if (app_reboot_to_launcher(slug)) return; // restarts
        ESP_LOGW(TAG, "reboot-exit persist failed; direct launch");
        lvgl_port_lock(0);
    }
    s_suppress_until_release |= s_pressed_buttons;
    s_active = a;
    if (s_tick_timer) {
        uint32_t period_ms = s_active->tick_period_ms();
        if (period_ms == 0) period_ms = 1;
        lv_timer_set_period(s_tick_timer, period_ms);
    }
    s_active->on_enter(root);
    heap_line("after enter", s_active->name());
    lvgl_port_unlock();
    // An app that must not be interrupted mid-run (games, timers, light
    // shows) holds the idle timer open for as long as it is foregrounded.
    hal_sleep_wake_lock(a->wants_wake_lock());
    ESP_LOGI(TAG, "launched %s", a->name());
}

void app_registry_return_to_launcher()
{
    if (s_launcher) app_registry_launch(s_launcher);
}

static void dispatch_button(ButtonEvent ev)
{
    hal_sleep_note_activity();
    const uint16_t bit = uint16_t(1u << static_cast<uint8_t>(ev.button));
    if (ev.kind == ButtonEventKind::Pressed) s_pressed_buttons |= bit;
    else s_pressed_buttons &= ~bit;
    if (s_suppress_until_release & bit) {
        if (ev.kind == ButtonEventKind::Released) s_suppress_until_release &= ~bit;
        return;
    }

    if (s_active != s_launcher && s_active
        && ev.kind == ButtonEventKind::Pressed) {
        if (s_home_confirmation) {
            const uint32_t paused_ms = lv_tick_elaps(s_home_confirmation_started_ms);
            close_home_confirmation();
            if (ev.button != Button::Home) {
                s_active->on_home_confirmation_cancel(paused_ms);
                return;
            }
            // Confirmed: use the existing exit path, including Chess's BYE.
        } else if (ev.button == Button::Home && s_active->confirm_home_exit()) {
            s_active->checkpoint_progress();
            show_home_confirmation();
            return;
        }
    }

    // Outside the confirmation, HOME is the only launcher exit. B remains
    // an ordinary app button; custom HOME handlers can finish their cleanup.
    if (ev.kind == ButtonEventKind::Pressed
        && ev.button == Button::Home
        && s_active != s_launcher
        && (!s_active || !s_active->handles_home_button())) {
        app_registry_return_to_launcher();
        return;
    }
    if (s_active) s_active->on_button(ev);
}

static void tick_cb(lv_timer_t *)
{
    static uint32_t seen_sync_revision = 0;
    const uint32_t sync_revision = hal_identity_sync_revision();
    if (sync_revision != seen_sync_revision) {
        seen_sync_revision = sync_revision;
        hal_sleep_note_activity();
        hal_lights_flash_success();
    }
    if (s_active) {
        if (s_home_confirmation) s_active->on_home_confirmation_tick();
        else s_active->on_tick();
    }
    if (s_home_confirmation) lv_obj_move_foreground(s_home_confirmation);
    hal_lights_tick();
    // After the app, so a tick that just rendered something still counts as
    // the frame the user sees before the sleep card goes up.
    hal_sleep_tick();
}

void app_registry_set_launcher(App *launcher)
{
    if (launcher) s_launcher = launcher;
}

void app_registry_start(App *launcher, App *first, bool reboot_bypass)
{
    s_launcher = launcher;
    s_reboot_bypass = reboot_bypass;

    lvgl_port_lock(0);
    s_tick_timer = lv_timer_create(tick_cb, 20, nullptr);
    lvgl_port_unlock();

    hal_buttons_set_dispatcher(dispatch_button);
    app_registry_launch(first ? first : launcher);
}
