#pragma once

#include <cstddef>
#include <cstdint>

// The physical map is split into six independently sized booth regions. The
// defaults match the current event floor plan, while the console can persist a
// different count for each region without rebuilding firmware.
static constexpr size_t kSponsorRegionCount = 6;
static constexpr size_t kSponsorMapMaxTables = 96;
static constexpr unsigned kSponsorRegionMaxTables = 64;
static constexpr size_t kSponsorDefaultTableCount = 64;
// Shared by booth positions and floor guides to center the map below its header.
static constexpr int16_t kSponsorMapYOffset = 6;

struct SponsorLayout {
    uint8_t region_counts[kSponsorRegionCount] = {};
    size_t table_count = 0;
};

struct SponsorBoothPosition {
    int16_t x = 0;
    int16_t y = 0;
    uint8_t size = 0;
    uint8_t region_index = 0;
};

void sponsor_layout_load(SponsorLayout *out);
size_t sponsor_layout_table_count();
const char *sponsor_layout_region_name(size_t region_index);
uint8_t sponsor_layout_default_count(size_t region_index);
bool sponsor_layout_booth_position(const SponsorLayout &layout,
                                   size_t table_index,
                                   SponsorBoothPosition *out);

// Persists all six counts as one atomic config value. Values may be zero, but
// the combined map must contain at least one booth and at most the map limit.
bool sponsor_layout_save_counts(const unsigned *counts, size_t count);
bool sponsor_layout_save_defaults();
