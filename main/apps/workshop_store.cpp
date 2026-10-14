#include "workshop_store.h"
#include "hal_config.h"

#include <cstring>

namespace {

constexpr const char *kTagPrefix = "work:";
constexpr const char *kConfigSlug = "workshop-quest";
constexpr const char *kVisitedKey = "visited";
// Holds every catalog slug comma-separated: the full 37-workshop catalog
// needs 459 bytes, so 1024 leaves room to grow while staying under the
// config layer's 1152-byte line limit. Kept as one buffer per function
// (verify reuses it) to limit stack use under the LVGL task.
constexpr size_t kVisitedBufferSize = 1024;

void read_visits(char out[kVisitedBufferSize])
{
    hal_config_get_string(kConfigSlug, kVisitedKey,
                          out, kVisitedBufferSize, "");
}

bool csv_contains(const char *csv, const char *slug)
{
    if (!csv || !slug || !*slug) return false;
    const size_t slug_length = strlen(slug);
    const char *cursor = csv;
    while (*cursor) {
        const char *end = strchr(cursor, ',');
        const size_t length = end ? static_cast<size_t>(end - cursor)
                                  : strlen(cursor);
        if (length == slug_length && strncmp(cursor, slug, length) == 0) {
            return true;
        }
        if (!end) break;
        cursor = end + 1;
    }
    return false;
}

WorkshopVisitResult result(WorkshopVisitStatus status,
                           const WorkshopInfo *workshop = nullptr)
{
    WorkshopVisitResult value;
    value.status = status;
    value.workshop = workshop;
    return value;
}

} // namespace

bool workshop_store_is_visited(const char *slug)
{
    char visits[kVisitedBufferSize];
    read_visits(visits);
    return csv_contains(visits, slug);
}

size_t workshop_store_count()
{
    return workshop_store_load(nullptr, 0);
}

size_t workshop_store_load(bool *visited, size_t capacity)
{
    char visits[kVisitedBufferSize];
    read_visits(visits);

    size_t count = 0;
    for (size_t i = 0; i < workshop_catalog_count(); ++i) {
        const WorkshopInfo *workshop = workshop_catalog_at(i);
        const bool found = workshop && csv_contains(visits, workshop->slug);
        if (visited && i < capacity) visited[i] = found;
        if (found) ++count;
    }
    return count;
}

WorkshopVisitResult workshop_store_record_payload(const char *payload)
{
    if (!payload || strncmp(payload, kTagPrefix, strlen(kTagPrefix)) != 0) {
        return result(WorkshopVisitStatus::NotWorkshop);
    }

    const char *slug = payload + strlen(kTagPrefix);
    if (!*slug || strchr(slug, ':')) {
        return result(WorkshopVisitStatus::InvalidPayload);
    }

    const WorkshopInfo *workshop = workshop_catalog_find_slug(slug);
    if (!workshop) return result(WorkshopVisitStatus::InvalidPayload);

    char visits[kVisitedBufferSize];
    read_visits(visits);
    if (csv_contains(visits, workshop->slug)) {
        return result(WorkshopVisitStatus::AlreadyVisited, workshop);
    }
    // No cap on the number of workshops: the quest goal stays five, but
    // users can keep scanning new ones after that.

    const size_t current_length = strlen(visits);
    const size_t slug_length = strlen(workshop->slug);
    const size_t separator_length = current_length > 0 ? 1 : 0;
    if (current_length + separator_length + slug_length + 1 > sizeof(visits)) {
        return result(WorkshopVisitStatus::StorageError, workshop);
    }
    if (separator_length) visits[current_length] = ',';
    memcpy(visits + current_length + separator_length,
           workshop->slug, slug_length + 1);

    hal_config_set_string(kConfigSlug, kVisitedKey, visits);

    read_visits(visits);
    if (!csv_contains(visits, workshop->slug)) {
        return result(WorkshopVisitStatus::StorageError, workshop);
    }
    return result(WorkshopVisitStatus::NewVisit, workshop);
}
