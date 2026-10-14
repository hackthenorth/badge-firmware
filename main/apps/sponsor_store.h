#pragma once

#include "sponsor_catalog.h"

#include <cstddef>
#include <cstdint>

using SponsorTableId = uint16_t;
static constexpr size_t kSponsorSlugMaxLength = 31;

enum class SponsorVisitStatus : uint8_t {
    NotSponsor,
    NewVisit,
    AlreadyVisited,
    InvalidPayload,
    StorageError,
};

struct SponsorVisitResult {
    SponsorVisitStatus status = SponsorVisitStatus::NotSponsor;
    const SponsorInfo *sponsor = nullptr;
    SponsorTableId table_id = 0;
    char slug[kSponsorSlugMaxLength + 1] = {};
};

struct SponsorTableState {
    bool visited = false;
    const SponsorInfo *sponsor = nullptr;
    // Preserved even when the compiled catalog does not recognize it.
    char slug[kSponsorSlugMaxLength + 1] = {};
};

struct SponsorStoredAssignment {
    SponsorTableId table_id = 0;
    const SponsorInfo *sponsor = nullptr;
    char slug[kSponsorSlugMaxLength + 1] = {};
};

SponsorVisitResult sponsor_store_record_payload(const char *payload);
bool sponsor_store_get_table_state(SponsorTableId table_id,
                                   SponsorTableState *out);
bool sponsor_store_is_visited(SponsorTableId table_id);
size_t sponsor_store_count();
// Count validated records beyond the visible map. This lets the Sponsors app
// build mapped table state incrementally without reopening every mapped file
// just to include overflow tables in its final progress total.
size_t sponsor_store_count_after(SponsorTableId last_table_id);
// Returns up to `capacity` current, non-empty sponsor assignments sorted by
// table ID. Visited tables whose sponsor moved away remain counted separately.
size_t sponsor_store_list_assignments(SponsorStoredAssignment *out,
                                      size_t capacity);
