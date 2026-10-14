#pragma once

#include <cstddef>
#include <cstdint>

// Stable badge-facing customization IDs. Zero is deliberately the absence of
// a customization; the nine artwork slots never move once firmware ships.
enum PalId : uint8_t {
    PAL_NONE = 0,
    PAL_GINNY_LOCKED_IN = 1,
    PAL_GINNY_SPARKLE = 2,
    PAL_PATCH_SNOOZE = 3,
    PAL_PATCH_SPARKLE = 4,
    PAL_VINYL_CURIOUS = 5,
    PAL_GINNY_HEART = 6,
    PAL_PATCH_CURIOUS = 7,
    PAL_VINYL_SNOOZE = 8,
    PAL_VINYL_SPARKLE = 9,
    PAL_COUNT = 10,
};

struct PalDefinition {
    uint8_t id;
    const char *item_id;  // opaque Solana reward ID
    const char *name;
};

const PalDefinition *pal_catalog_get(uint8_t id);
const PalDefinition *pal_catalog_find_item(const char *item_id);
size_t pal_catalog_size();

