#pragma once

#include <cstddef>

class App;

void   app_registry_add(App *app);
size_t app_registry_count();
App   *app_registry_get(size_t i);
App   *app_registry_find_by_slug(const char *slug);

// Removes app from the registry and deletes it. Only call on heap-allocated
// apps (e.g. ScriptApp); built-in singleton apps must never be passed here.
void   app_registry_remove(App *app);

void   app_registry_launch(App *a);
void   app_registry_return_to_launcher();

// Installs the button dispatcher + tick timer, then launches `first` (or the
// launcher if `first` is null). Call once after all HAL inits are done.
//
// `launcher` is where the global HOME button lands, which is not necessarily
// what boots: normal boot opens the badge ID page with the grid behind it,
// and that page reveals the grid explicitly with Home or Start.
//
// `reboot_bypass` skips the reboot-enter rule for that first launch only: the
// boot path already rebooted into a pending target, so re-applying the rule
// would restart right back out of it.
void   app_registry_start(App *launcher, App *first = nullptr, bool reboot_bypass = false);

// Swap what the HOME button returns to. Used by onboarding, which makes
// itself the launcher so the registry's existing "don't intercept HOME
// inside the launcher" rule turns it into a modal flow for free, then hands
// over to the real launcher on completion.
void   app_registry_set_launcher(App *launcher);
