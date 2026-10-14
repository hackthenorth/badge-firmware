#pragma once

#include <cstddef>
#include <cstdint>

enum class LauncherAppMode : uint8_t {
    DefaultsOnly = 0,
    PublicApps = 1,
    PublicAndInternal = 2,
};

// Ordered list of slugs that appear in the launcher. Persisted in
// /littlefs/config/system.cfg under key `launcher_order` (CSV).
//
// The list is the single source of truth for launcher ordering. Default apps
// are always members and cannot be removed; unlockable apps become members
// when installed. NFC scans append, while launcher actions move or drop
// eligible entries.
//
// Also applies versioned defaults for general apps that should be unlocked
// after a normal firmware flash, including on badges with existing config.

void   launcher_slugs_load();

// Apply and persist one-time default app migrations. Call after every app is
// registered so the always-present apps can be seeded into the order.
void   launcher_slugs_apply_defaults();

bool   launcher_slugs_contains(const char *slug);
bool   launcher_slugs_append(const char *slug);          // false if already present
bool   launcher_slugs_remove(const char *slug);          // false if not present
bool   launcher_slugs_move(const char *slug, size_t new_idx);

size_t      launcher_slugs_count();
const char *launcher_slugs_at(size_t i);                 // nullptr if out of range

// Drops any slug for which keep(slug) returns false. Persists once at end.
// Used at boot to evict slugs whose backing app is no longer registered
// (e.g. a built-in removed from the firmware, a Lua app whose dir is gone).
void   launcher_slugs_prune(bool (*keep)(const char *slug));

// Batch testing helpers. Public and internal apps are distinct tiers: public
// Public unlock paths can never reveal internal tools.
size_t launcher_slugs_unlock_public_apps();
size_t launcher_slugs_unlock_internal_apps();
void   launcher_slugs_set_mode(LauncherAppMode mode);
