#include "sponsor_layout.h"

#include "hal_config.h"

#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

constexpr const char *kConfigSlug = "sponsors";
constexpr const char *kConfigKey = "regions";
constexpr int kCellGap = 1;
constexpr int kLargestCell = 14;
constexpr int kSmallestCell = 2;

struct RegionSpec {
    const char *name;
    uint8_t default_count;
    uint8_t default_columns;
    int16_t x;
    int16_t y;
    int16_t width;
    int16_t height;
};

// These six footprints preserve the supplied two-floor grouping. Counts are
// the current 2x4, 4x1, 2x2, (3x5)-1, 7x4, and 1x6 booth batches.
constexpr RegionSpec kRegions[kSponsorRegionCount] = {
    {"A",  8, 2,   4,  52,  30,  72},
    {"B",  4, 4,  60,  87,  64,  16},
    {"C",  4, 2, 148,  49,  32,  34},
    {"D", 14, 3, 148, 101,  48,  92},
    {"E", 28, 7, 202, 101, 104,  74},
    {"F",  6, 1, 304,  74,  16, 112},
};

bool valid_counts(const unsigned *counts, size_t count)
{
    if (!counts || count != kSponsorRegionCount) return false;
    size_t total = 0;
    for (size_t i = 0; i < count; ++i) {
        if (counts[i] > kSponsorRegionMaxTables) return false;
        total += counts[i];
    }
    return total > 0 && total <= kSponsorMapMaxTables;
}

void apply_defaults(SponsorLayout *out)
{
    if (!out) return;
    *out = {};
    for (size_t i = 0; i < kSponsorRegionCount; ++i) {
        out->region_counts[i] = kRegions[i].default_count;
        out->table_count += kRegions[i].default_count;
    }
}

bool parse_counts(const char *text, unsigned counts[kSponsorRegionCount])
{
    if (!text || !text[0]) return false;
    const char *cursor = text;
    for (size_t i = 0; i < kSponsorRegionCount; ++i) {
        char *end = nullptr;
        const unsigned long parsed = strtoul(cursor, &end, 10);
        if (!end || end == cursor || parsed > kSponsorRegionMaxTables) {
            return false;
        }
        counts[i] = static_cast<unsigned>(parsed);
        if (i + 1 < kSponsorRegionCount) {
            if (*end != ',') return false;
            cursor = end + 1;
        } else if (*end != '\0') {
            return false;
        }
    }
    return valid_counts(counts, kSponsorRegionCount);
}

bool choose_grid(const RegionSpec &region, unsigned count,
                 unsigned *columns_out, unsigned *rows_out,
                 unsigned *cell_out)
{
    if (count == 0 || !columns_out || !rows_out || !cell_out) return false;

    // Preserve the authored shape of each default region. In particular,
    // region D's 14 booths remain three columns wide with a two-booth final
    // row instead of collapsing into an exact but overly narrow 2x7 grid.
    if (count == region.default_count) {
        const unsigned columns = region.default_columns;
        const unsigned rows = (count + columns - 1) / columns;
        const int available_width = region.width
            - static_cast<int>(columns - 1) * kCellGap;
        const int available_height = region.height
            - static_cast<int>(rows - 1) * kCellGap;
        if (available_width <= 0 || available_height <= 0) return false;

        int cell = available_width / static_cast<int>(columns);
        const int height_cell = available_height / static_cast<int>(rows);
        if (height_cell < cell) cell = height_cell;
        if (cell > kLargestCell) cell = kLargestCell;
        if (cell < kSmallestCell) return false;

        *columns_out = columns;
        *rows_out = rows;
        *cell_out = static_cast<unsigned>(cell);
        return true;
    }

    unsigned best_columns = 0;
    unsigned best_rows = 0;
    unsigned best_cell = 0;
    unsigned long best_score = ULONG_MAX;

    for (unsigned columns = 1; columns <= count; ++columns) {
        const unsigned rows = (count + columns - 1) / columns;
        const int available_width = region.width
            - static_cast<int>(columns - 1) * kCellGap;
        const int available_height = region.height
            - static_cast<int>(rows - 1) * kCellGap;
        if (available_width <= 0 || available_height <= 0) continue;

        int cell = available_width / static_cast<int>(columns);
        const int height_cell = available_height / static_cast<int>(rows);
        if (height_cell < cell) cell = height_cell;
        if (cell > kLargestCell) cell = kLargestCell;
        if (cell < kSmallestCell) continue;

        const unsigned unused = columns * rows - count;
        const long aspect = static_cast<long>(columns) * region.height
            - static_cast<long>(rows) * region.width;
        const unsigned long aspect_error = static_cast<unsigned long>(
            aspect < 0 ? -aspect : aspect);
        const unsigned long score = static_cast<unsigned long>(unused) * 100000UL
            + aspect_error * 10UL
            + static_cast<unsigned long>(kLargestCell - cell);
        if (score < best_score) {
            best_score = score;
            best_columns = columns;
            best_rows = rows;
            best_cell = static_cast<unsigned>(cell);
        }
    }

    if (best_columns == 0) return false;
    *columns_out = best_columns;
    *rows_out = best_rows;
    *cell_out = best_cell;
    return true;
}

} // namespace

void sponsor_layout_load(SponsorLayout *out)
{
    apply_defaults(out);
    if (!out) return;

    char text[64] = {};
    if (!hal_config_get_string(kConfigSlug, kConfigKey, text, sizeof(text), "")) {
        return;
    }

    unsigned counts[kSponsorRegionCount] = {};
    if (!parse_counts(text, counts)) return;

    *out = {};
    for (size_t i = 0; i < kSponsorRegionCount; ++i) {
        out->region_counts[i] = static_cast<uint8_t>(counts[i]);
        out->table_count += counts[i];
    }
}

size_t sponsor_layout_table_count()
{
    SponsorLayout layout;
    sponsor_layout_load(&layout);
    return layout.table_count;
}

const char *sponsor_layout_region_name(size_t region_index)
{
    return region_index < kSponsorRegionCount ? kRegions[region_index].name : "?";
}

uint8_t sponsor_layout_default_count(size_t region_index)
{
    return region_index < kSponsorRegionCount
        ? kRegions[region_index].default_count : 0;
}

bool sponsor_layout_booth_position(const SponsorLayout &layout,
                                   size_t table_index,
                                   SponsorBoothPosition *out)
{
    if (!out || table_index >= layout.table_count) return false;

    size_t region_start = 0;
    for (size_t region_index = 0; region_index < kSponsorRegionCount;
         ++region_index) {
        const unsigned count = layout.region_counts[region_index];
        if (table_index >= region_start + count) {
            region_start += count;
            continue;
        }

        unsigned columns = 0;
        unsigned rows = 0;
        unsigned cell = 0;
        if (!choose_grid(kRegions[region_index], count, &columns, &rows, &cell)) {
            return false;
        }

        const size_t local = table_index - region_start;
        const int grid_width = static_cast<int>(columns * cell)
            + static_cast<int>(columns - 1) * kCellGap;
        const int grid_height = static_cast<int>(rows * cell)
            + static_cast<int>(rows - 1) * kCellGap;
        const unsigned row = static_cast<unsigned>(local / columns);
        const unsigned column = static_cast<unsigned>(local % columns);
        int row_x_offset = 0;
        const unsigned final_row_count = count % columns;
        if (final_row_count != 0 && row + 1 == rows) {
            const int final_row_width = static_cast<int>(final_row_count * cell)
                + static_cast<int>(final_row_count - 1) * kCellGap;
            row_x_offset = (grid_width - final_row_width) / 2;
        }
        out->x = static_cast<int16_t>(kRegions[region_index].x
            + (kRegions[region_index].width - grid_width) / 2
            + row_x_offset
            + static_cast<int>(column) * (static_cast<int>(cell) + kCellGap));
        out->y = static_cast<int16_t>(kRegions[region_index].y
            + kSponsorMapYOffset
            + (kRegions[region_index].height - grid_height) / 2
            + static_cast<int>(row) * (static_cast<int>(cell) + kCellGap));
        out->size = static_cast<uint8_t>(cell);
        out->region_index = static_cast<uint8_t>(region_index);
        return true;
    }
    return false;
}

bool sponsor_layout_save_counts(const unsigned *counts, size_t count)
{
    if (!valid_counts(counts, count)) return false;

    char value[64];
    const int written = snprintf(value, sizeof(value), "%u,%u,%u,%u,%u,%u",
        counts[0], counts[1], counts[2], counts[3], counts[4], counts[5]);
    if (written < 0 || written >= static_cast<int>(sizeof(value))) return false;
    hal_config_set_string(kConfigSlug, kConfigKey, value);

    char saved[64] = {};
    return hal_config_get_string(kConfigSlug, kConfigKey, saved, sizeof(saved), "")
        && strcmp(saved, value) == 0;
}

bool sponsor_layout_save_defaults()
{
    unsigned counts[kSponsorRegionCount];
    for (size_t i = 0; i < kSponsorRegionCount; ++i) {
        counts[i] = kRegions[i].default_count;
    }
    return sponsor_layout_save_counts(counts, kSponsorRegionCount);
}
