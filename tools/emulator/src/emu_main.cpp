// Entry points the page calls: boot the firmware, then step it. One instance
// of this module is one boot of the badge; a restart discards the instance.

#include "emu.h"

#include "app.h"
#include "app_launcher.h"
#include "app_registry.h"
#include "hal_config.h"
#include "hal_sleep.h"
#include "json_util.h"
#include "launcher_slugs.h"
#include "sponsor_catalog.h"
#include "workshop_catalog.h"

#include "lvgl.h"

#include <emscripten.h>

#include <cstdio>
#include <string>

extern "C" void app_main(void);

int main()
{
    // Static constructors (every built-in app) have run; the page calls
    // emu_boot() once it has restored the flash image.
    return 0;
}

extern "C" EMSCRIPTEN_KEEPALIVE void emu_boot()
{
    app_main();
}

// Advance the badge by `elapsed_ms` of wall-clock time. LVGL timers fire at
// the virtual instants they are due, not once per browser frame, so a 20 ms
// app tick stays 50 Hz whether the page is drawing at 60 Hz or throttled.
extern "C" EMSCRIPTEN_KEEPALIVE void emu_step(double elapsed_ms)
{
    static double budget_ms = 0;
    budget_ms += elapsed_ms;
    // A stalled tab (debugger, laptop lid) resumes where it stopped rather
    // than fast-forwarding through the gap.
    if (budget_ms > 250.0) budget_ms = 250.0;

    while (budget_ms >= 1.0) {
        emu_sleep_poll();
        uint32_t wait_ms = 10;
        if (!emu_sleep_frozen()) {
            emu_radio_poll();
            wait_ms = lv_timer_handler();
        }
        // Wake at the next due timer, but at least every 10 ms so the radio
        // schedule and sleep wake sources keep their resolution.
        if (wait_ms < 1) wait_ms = 1;
        if (wait_ms > 10) wait_ms = 10;
        if ((double)wait_ms > budget_ms) wait_ms = (uint32_t)budget_ms;
        if (wait_ms < 1) wait_ms = 1;
        emu_advance_us((int64_t)wait_ms * 1000);
        budget_ms -= (double)wait_ms;
    }
}

// What the console's password-gated `appmode` does once authorised. The page
// is the badge's owner, so it sets the tier directly and then reboots.
extern "C" EMSCRIPTEN_KEEPALIVE void emu_set_app_mode(int mode)
{
    if (mode < 0 || mode > 2) return;
    launcher_slugs_set_mode(static_cast<LauncherAppMode>(mode));
}

extern "C" EMSCRIPTEN_KEEPALIVE int emu_onboarding_pending()
{
    return badge_onboarding_required();
}

// The page's "Skip tutorial": what OnboardingApp::finish() does after its last
// page, except that it lands on the app grid rather than the ID page, so a
// visitor is one press from playing. The emulator is built without
// BADGE_BUILD, so 0 is the build number that marks the tutorial complete.
extern "C" EMSCRIPTEN_KEEPALIVE int emu_skip_onboarding()
{
    if (!badge_onboarding_required()) return 0;
    hal_config_set_int("system", "onboard_build", 0);
    app_registry_set_launcher(app_launcher_instance());
    app_registry_return_to_launcher();
    return 1;
}

// Skip the five-minute idle wait when checking the sleep card.
extern "C" EMSCRIPTEN_KEEPALIVE void emu_sleep_now()
{
    hal_sleep_enter_now();
}

namespace {

void append_string(std::string &out, const char *key, const char *value, bool last = false)
{
    char escaped[512];
    json_escape(escaped, sizeof(escaped), value ? value : "");
    out += '"';
    out += key;
    out += "\":\"";
    out += escaped;
    out += last ? "\"" : "\",";
}

const char *access_name(AppAccess access)
{
    switch (access) {
    case AppAccess::Default:  return "default";
    case AppAccess::Public:   return "public";
    case AppAccess::Internal: return "internal";
    }
    return "public";
}

} // namespace

// What the page needs to offer meaningful NFC tags: the apps, sponsors and
// workshops this firmware build actually knows, straight from its catalogs.
extern "C" EMSCRIPTEN_KEEPALIVE const char *emu_catalog_json()
{
    static std::string json;
    json = "{\"apps\":[";
    for (size_t i = 0; i < app_registry_count(); ++i) {
        App *app = app_registry_get(i);
        if (!app) continue;
        if (json.back() == '}') json += ',';
        json += '{';
        append_string(json, "slug", app->slug());
        append_string(json, "name", app->name());
        append_string(json, "access", access_name(app->access()));
        json += "\"nfc\":";
        json += app->uses_nfc() ? "true" : "false";
        json += ",\"lua\":";
        json += app->is_user_installed() ? "true" : "false";
        json += ",\"installed\":";
        json += launcher_slugs_contains(app->slug()) ? "true" : "false";
        json += '}';
    }
    json += "],\"sponsors\":[";
    for (size_t i = 0; i < sponsor_catalog_count(); ++i) {
        const SponsorInfo *sponsor = sponsor_catalog_at(i);
        if (i) json += ',';
        json += '{';
        append_string(json, "slug", sponsor->slug);
        append_string(json, "name", sponsor->name, true);
        json += '}';
    }
    json += "],\"workshops\":[";
    for (size_t i = 0; i < workshop_catalog_count(); ++i) {
        const WorkshopInfo *workshop = workshop_catalog_at(i);
        if (i) json += ',';
        json += '{';
        append_string(json, "slug", workshop->slug);
        append_string(json, "code", workshop->code);
        append_string(json, "name", workshop->name, true);
        json += '}';
    }
    json += "]}";
    return json.c_str();
}
