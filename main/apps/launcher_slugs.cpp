#include "launcher_slugs.h"
#include "app.h"
#include "app_registry.h"
#include "hal_config.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

static constexpr size_t kSerializedCap = 1024;
static constexpr int kDefaultAppsVersion = 14;

static std::vector<std::string> &slugs()
{
    static std::vector<std::string> v;
    return v;
}

static void persist()
{
    std::string joined;
    joined.reserve(kSerializedCap);
    for (size_t i = 0; i < slugs().size(); i++) {
        if (joined.size() + slugs()[i].size() + 1 > kSerializedCap) break;
        if (i) joined.push_back(',');
        joined.append(slugs()[i]);
    }
    hal_config_set_string("system", "launcher_order", joined.c_str());
}

static void parse_csv_into_list(char *buf)
{
    char *p = buf;
    while (*p) {
        char *comma = strchr(p, ',');
        if (comma) *comma = '\0';
        if (*p) slugs().emplace_back(p);
        if (!comma) break;
        p = comma + 1;
    }
}

void launcher_slugs_load()
{
    slugs().clear();

    char buf[kSerializedCap];
    if (hal_config_get_string("system", "launcher_order", buf, sizeof(buf), "")
        && buf[0]) {
        parse_csv_into_list(buf);
    }

}

static bool ensure_default_apps()
{
    bool changed = false;
    const bool fresh = slugs().empty();
    for (size_t i = 0; i < app_registry_count(); i++) {
        App *app = app_registry_get(i);
        if (!app || !app->is_default_app()
            || launcher_slugs_contains(app->slug())) continue;
        // New defaults join existing orders at the end, preserving the first
        // tile and custom moves. Fresh lists naturally use registry order.
        if (!fresh && strcmp(app->slug(), "share") == 0
            && hal_config_get_int("system", "default_apps_version", 0) < 9) {
            // Preserve the legacy Share insertion recognized by v8 migration.
            slugs().insert(slugs().begin(), app->slug());
        } else {
            slugs().emplace_back(app->slug());
        }
        changed = true;
    }
    return changed;
}

static bool reorder_untouched_defaults(int current_version)
{
    static const char *kPreviousOrders[][10] = {
        {"badgeid", "blindbox", "schedule", "museum", "sponsors",
         "workshops", "scanner", "connect", "mood", "socials"},
        {"badgeid", "blindbox", "scanner", "connect", "socials",
         "schedule", "sponsors", "mood", "workshops", "museum"},
    };
    static const char *kVersion8Order[] = {
        "badgeid", "scanner", "connect", "share", "blindbox", "sponsors",
        "workshops", "mood", "socials", "schedule", "museum", "rules",
    };
    static const char *kVersion9Order[] = {
        "badgeid", "connect", "scanner", "share", "blindbox", "sponsors",
        "workshops", "mood", "socials", "schedule", "museum", "rules",
    };
    // Adding Share to an existing v8 badge prepended it ahead of My Badge.
    // Recognize that automatic insertion too; unlocked apps and Rules may
    // follow this prefix in either order.
    static const char *kVersion8ShareFirst[] = {
        "share", "badgeid", "scanner", "connect", "blindbox", "sponsors",
        "workshops", "mood", "socials", "schedule", "museum",
    };
    // Match only the default sequence shipped for this saved version.
    const char *const *previous = current_version >= 9 ? kVersion9Order : current_version >= 8 ? kVersion8Order
        : kPreviousOrders[current_version < 7 ? 0 : 1];
    size_t count = current_version >= 8
        ? sizeof(kVersion8Order) / sizeof(kVersion8Order[0])
        : sizeof(kPreviousOrders[0]) / sizeof(kPreviousOrders[0][0]);
    const auto matches = [](const char *const *prefix, size_t length) {
        if (slugs().size() < length) return false;
        for (size_t i = 0; i < length; ++i) {
            if (slugs()[i] != prefix[i]) return false;
        }
        return true;
    };
    if (!matches(previous, count)) {
        if (current_version < 8 || current_version >= 9) return false;
        previous = kVersion8ShareFirst;
        count = sizeof(kVersion8ShareFirst) / sizeof(kVersion8ShareFirst[0]);
        if (!matches(previous, count)) return false;
    }

    // Registry order defines the fresh default bundle. Only replace the exact
    // old prefix, preserving unlocked apps and any customization after it.
    // Newly introduced defaults stay outside the matched prefix.
    std::vector<std::string> defaults;
    for (size_t i = 0; i < app_registry_count(); ++i) {
        App *app = app_registry_get(i);
        if (app && app->is_default_app()
            && std::any_of(previous, previous + count, [app](const char *slug) {
                return strcmp(app->slug(), slug) == 0;
            })) {
            defaults.emplace_back(app->slug());
        }
    }
    if (defaults.size() != count) return false;
    std::copy(defaults.begin(), defaults.end(), slugs().begin());
    // The legacy Share-first prefix did not include Rules. If Rules follows
    // that untouched prefix, bring it beside Museum along with the other defaults.
    auto museum = std::find(slugs().begin(), slugs().end(), "museum");
    auto rules = std::find(slugs().begin(), slugs().end(), "rules");
    if (museum != slugs().end() && rules == museum + 1) std::iter_swap(museum, rules);
    return true;
}

void launcher_slugs_apply_defaults()
{
    // One-time migration for badges that previously received Swag as a public
    // default or enabled the legacy all-visible debug bypass. Internal apps
    // must enter only through explicit diagnostic access.
    const int current_version =
        hal_config_get_int("system", "default_apps_version", 0);
    bool changed = false;
    if (current_version < 2) {
        static const char *kInternalSlugs[] = {
            "test", "swag", "files", "diagnostics", "meminfo",
        };
        for (const char *slug : kInternalSlugs) {
            auto it = std::find(slugs().begin(), slugs().end(), slug);
            if (it != slugs().end()) {
                slugs().erase(it);
                changed = true;
            }
        }
        hal_config_set_string("system", "debug_mode", "false");
    }

    // These prototypes used to be attendee-facing. Remove their persisted
    // entries once so existing badges hide them too; diagnostic access can
    // append them again from their Internal access tier.
    if (current_version < 4) {
        static const char *kNewInternalSlugs[] = {
            "accel", "anim", "progress",
        };
        for (const char *slug : kNewInternalSlugs) {
            const auto old_size = slugs().size();
            slugs().erase(std::remove(slugs().begin(), slugs().end(), slug),
                          slugs().end());
            if (slugs().size() != old_size) changed = true;
        }
    }

    // Default apps used to live outside the persisted slug list. Add any that
    // are missing in registry order, but leave an existing user-defined order
    // untouched on every subsequent boot.
    const bool fresh_order = slugs().empty();
    if (ensure_default_apps()) changed = true;

    // Preserve the legacy hunt promotion for existing older launchers. Fresh
    // badges already use the current registry order.
    if (current_version < 6 && !fresh_order) {
        App *hunt = app_registry_find_by_slug("blindbox");
        if (hunt && hunt->is_default_app()) {
            slugs().erase(std::remove(slugs().begin(), slugs().end(), hunt->slug()),
                          slugs().end());
            auto badge = std::find(slugs().begin(), slugs().end(), "badgeid");
            auto position = badge == slugs().end() ? slugs().begin() : badge + 1;
            slugs().insert(position, hunt->slug());
            changed = true;
        }
    }
    // Refresh untouched launchers once; do not move apps in a custom order.
    if (!fresh_order && current_version < kDefaultAppsVersion
        && reorder_untouched_defaults(current_version)) {
        changed = true;
    }
    // Version 11 placed Sync after Museum. Move it beside Scanner once on
    // upgrade; later user moves remain untouched.
    if (current_version < 12 && launcher_slugs_contains("scanner")) {
        slugs().erase(std::remove(slugs().begin(), slugs().end(), "sync"), slugs().end());
        auto scanner = std::find(slugs().begin(), slugs().end(), "scanner");
        slugs().insert(scanner + 1, "sync");
        changed = true;
    }
    // Blackjack landed after some badges had already been provisioned with a
    // snapshot of every Public app. Add it once to those public/all launchers;
    // default-mode badges still need the normal NFC unlock payload.
    if (current_version < 13 && !launcher_slugs_contains("blackjack")) {
        char mode[16];
        hal_config_get_string("system", "app_mode", mode, sizeof(mode),
                              "default");
        const bool seeded_public = strcmp(mode, "public") == 0
            || strcmp(mode, "all") == 0;
        App *blackjack = app_registry_find_by_slug("blackjack");
        if (seeded_public && blackjack
            && blackjack->access() == AppAccess::Public) {
            slugs().emplace_back(blackjack->slug());
            changed = true;
        }
    }
    // Flashlight is internal again. Remove old public unlocks once, while
    // preserving explicitly provisioned all-app diagnostic launchers.
    if (current_version < 14) {
        char mode[16];
        hal_config_get_string("system", "app_mode", mode, sizeof(mode), "default");
        if (strcmp(mode, "all") != 0) {
            const auto old_size = slugs().size();
            slugs().erase(std::remove(slugs().begin(), slugs().end(), "flashlight"),
                          slugs().end());
            if (slugs().size() != old_size) changed = true;
        }
    }
    if (changed) persist();
    if (current_version < kDefaultAppsVersion) {
        hal_config_set_int("system", "default_apps_version",
                           kDefaultAppsVersion);
    }
}

static size_t append_access_tier(AppAccess tier)
{
    size_t added = 0;
    for (size_t i = 0; i < app_registry_count(); i++) {
        App *app = app_registry_get(i);
        if (!app || app->access() != tier || launcher_slugs_contains(app->slug())) continue;
        slugs().emplace_back(app->slug());
        added++;
    }
    if (added) persist();
    return added;
}

size_t launcher_slugs_unlock_public_apps()
{
    return append_access_tier(AppAccess::Public);
}

size_t launcher_slugs_unlock_internal_apps()
{
    return append_access_tier(AppAccess::Internal);
}

void launcher_slugs_set_mode(LauncherAppMode mode)
{
    slugs().clear();
    append_access_tier(AppAccess::Default);
    if (mode >= LauncherAppMode::PublicApps) {
        append_access_tier(AppAccess::Public);
    }
    if (mode >= LauncherAppMode::PublicAndInternal) {
        append_access_tier(AppAccess::Internal);
    }
    // append_access_tier persists only when it adds an app. Keep the explicit
    // DefaultsOnly write as a safe fallback if a future build has no defaults.
    if (mode == LauncherAppMode::DefaultsOnly) persist();

    static const char *kModeNames[] = { "default", "public", "all" };
    hal_config_set_string("system", "app_mode",
                          kModeNames[static_cast<uint8_t>(mode)]);
}

bool launcher_slugs_contains(const char *slug)
{
    if (!slug) return false;
    for (const auto &s : slugs()) {
        if (s == slug) return true;
    }
    return false;
}

bool launcher_slugs_append(const char *slug)
{
    if (!slug || !*slug) return false;
    if (launcher_slugs_contains(slug)) return false;
    slugs().emplace_back(slug);
    persist();
    return true;
}

bool launcher_slugs_remove(const char *slug)
{
    if (!slug || !*slug) return false;
    App *app = app_registry_find_by_slug(slug);
    if (app && app->is_default_app()) return false;
    auto &v = slugs();
    auto it = std::find(v.begin(), v.end(), slug);
    if (it == v.end()) return false;
    v.erase(it);
    persist();
    return true;
}

bool launcher_slugs_move(const char *slug, size_t new_idx)
{
    if (!slug || !*slug) return false;
    auto &v = slugs();
    auto it = std::find(v.begin(), v.end(), slug);
    if (it == v.end()) return false;
    std::string s = std::move(*it);
    v.erase(it);
    if (new_idx > v.size()) new_idx = v.size();
    v.insert(v.begin() + new_idx, std::move(s));
    persist();
    return true;
}

size_t launcher_slugs_count()
{
    return slugs().size();
}

const char *launcher_slugs_at(size_t i)
{
    return i < slugs().size() ? slugs()[i].c_str() : nullptr;
}

void launcher_slugs_prune(bool (*keep)(const char *slug))
{
    if (!keep) return;
    auto &v = slugs();
    bool changed = false;
    for (auto it = v.begin(); it != v.end(); ) {
        if (!keep(it->c_str())) { it = v.erase(it); changed = true; }
        else ++it;
    }
    if (changed) persist();
}
