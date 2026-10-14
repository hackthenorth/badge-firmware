#pragma once

#include <cstddef>

struct WorkshopInfo {
    const char *slug;
    const char *code;
    const char *name;
};

size_t workshop_catalog_count();
const WorkshopInfo *workshop_catalog_at(size_t index);
const WorkshopInfo *workshop_catalog_find_slug(const char *slug);
size_t workshop_catalog_index(const WorkshopInfo *workshop);
