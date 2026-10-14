#pragma once

#include "workshop_catalog.h"

#include <cstddef>
#include <cstdint>

constexpr size_t kWorkshopQuestGoal = 5;

enum class WorkshopVisitStatus : uint8_t {
    NotWorkshop,
    NewVisit,
    AlreadyVisited,
    // Retained for compatibility; no longer returned since scanning is
    // uncapped, but existing callers still handle it.
    QuestComplete,
    InvalidPayload,
    StorageError,
};

struct WorkshopVisitResult {
    WorkshopVisitStatus status = WorkshopVisitStatus::NotWorkshop;
    const WorkshopInfo *workshop = nullptr;
};

WorkshopVisitResult workshop_store_record_payload(const char *payload);
bool workshop_store_is_visited(const char *slug);
size_t workshop_store_count();
// Fills catalog-indexed visit flags from a single config read and returns the
// total number of recognized workshops collected.
size_t workshop_store_load(bool *visited, size_t capacity);
