#pragma once

#include <cstddef>
#include <cstdint>

struct SponsorInfo {
    const char *slug;
    const char *name;
    const char *description;
    const char *link;
};

size_t sponsor_catalog_count();
const SponsorInfo *sponsor_catalog_at(size_t index);
const SponsorInfo *sponsor_catalog_find_slug(const char *slug);
