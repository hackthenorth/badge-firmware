#include "pal_catalog.h"

#include <cstring>

namespace {

constexpr PalDefinition kPals[] = {
    { PAL_GINNY_LOCKED_IN, "1", "Ginny - Locked In" },
    { PAL_GINNY_SPARKLE,   "2", "Ginny - Sparkle" },
    { PAL_PATCH_SNOOZE,    "3", "Patch - Snooze" },
    { PAL_PATCH_SPARKLE,   "4", "Patch - Sparkle" },
    { PAL_VINYL_CURIOUS,   "5", "Vinyl - Curious" },
    { PAL_GINNY_HEART,     "6", "Ginny - Heart" },
    { PAL_PATCH_CURIOUS,   "7", "Patch - Curious" },
    { PAL_VINYL_SNOOZE,    "8", "Vinyl - Snooze" },
    { PAL_VINYL_SPARKLE,   "9", "Vinyl - Sparkle" },
};

} // namespace

const PalDefinition *pal_catalog_get(uint8_t id)
{
    if (id == PAL_NONE || id >= PAL_COUNT) return nullptr;
    return &kPals[id - 1];
}

const PalDefinition *pal_catalog_find_item(const char *item_id)
{
    if (!item_id || !*item_id) return nullptr;
    for (const auto &pal : kPals) {
        if (strcmp(pal.item_id, item_id) == 0) return &pal;
    }
    return nullptr;
}

size_t pal_catalog_size()
{
    return sizeof(kPals) / sizeof(kPals[0]);
}

