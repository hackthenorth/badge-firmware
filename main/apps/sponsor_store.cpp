#include "sponsor_store.h"
#include "hal_fs.h"

#include "esp_log.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <unistd.h>

namespace {

constexpr const char *kTagPrefix = "spon:";
constexpr const char *kStoreDir = "/littlefs/config/sponsors";
constexpr unsigned kStoreVersion = 1;
constexpr unsigned long kMaxTableId = 65535;
const char *TAG = "sponsor_store";

bool slug_char_allowed(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
}

bool slug_valid(const char *slug, bool allow_empty)
{
    if (!slug || (!allow_empty && !slug[0])) return false;
    const size_t length = strlen(slug);
    if (length > kSponsorSlugMaxLength) return false;
    for (size_t i = 0; i < length; ++i) {
        if (!slug_char_allowed(slug[i])) return false;
    }
    return true;
}

bool visit_path(SponsorTableId table_id, char *out, size_t out_size)
{
    if (!out || table_id == 0) return false;
    return snprintf(out, out_size, "%s/%02u.cfg", kStoreDir,
                    static_cast<unsigned>(table_id)) < static_cast<int>(out_size);
}

bool table_from_filename(const char *name, SponsorTableId *table_out)
{
    if (!name || !table_out) return false;

    char *end = nullptr;
    const unsigned long parsed = strtoul(name, &end, 10);
    if (end == name || parsed < 1 || parsed > kMaxTableId
        || strcmp(end, ".cfg") != 0) {
        return false;
    }

    char canonical[16];
    snprintf(canonical, sizeof(canonical), "%02lu.cfg", parsed);
    if (strcmp(name, canonical) != 0) return false;

    *table_out = static_cast<SponsorTableId>(parsed);
    return true;
}

// An empty slug is valid: it preserves table completion after a sponsor moves
// to a new table while making the old table's current identity unknown.
bool read_visit(SponsorTableId expected_table, char *slug_out,
                size_t slug_out_size)
{
    char path[128];
    if (!visit_path(expected_table, path, sizeof(path))) return false;

    FILE *f = fopen(path, "r");
    if (!f) return false;

    unsigned version = 0;
    unsigned table = 0;
    bool saw_slug = false;
    char slug[kSponsorSlugMaxLength + 1] = {};
    char line[96];
    while (fgets(line, sizeof(line), f)) {
        unsigned value = 0;
        if (sscanf(line, "version=%u", &value) == 1) {
            version = value;
        } else if (sscanf(line, "table=%u", &value) == 1) {
            table = value;
        } else if (strncmp(line, "slug=", 5) == 0) {
            size_t length = strcspn(line + 5, "\r\n");
            if (length > kSponsorSlugMaxLength) {
                fclose(f);
                return false;
            }
            memcpy(slug, line + 5, length);
            slug[length] = '\0';
            saw_slug = true;
        }
    }
    fclose(f);

    if (version != kStoreVersion || table != expected_table || !saw_slug
        || !slug_valid(slug, true)) {
        return false;
    }

    if (slug_out && slug_out_size > 0) {
        snprintf(slug_out, slug_out_size, "%s", slug);
    }
    return true;
}

bool write_visit(SponsorTableId table_id, const char *slug)
{
    if (!slug_valid(slug ? slug : "", true)) return false;
    if (!hal_fs_mkdir_p(kStoreDir)) {
        ESP_LOGE(TAG, "mkdir %s failed", kStoreDir);
        return false;
    }

    char path[128];
    if (!visit_path(table_id, path, sizeof(path))) return false;

    char tmp_path[136];
    if (snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path)
        >= static_cast<int>(sizeof(tmp_path))) {
        return false;
    }

    FILE *f = fopen(tmp_path, "w");
    if (!f) {
        ESP_LOGE(TAG, "open %s: %s", tmp_path, strerror(errno));
        return false;
    }

    fprintf(f, "version=%u\n", kStoreVersion);
    fprintf(f, "table=%u\n", static_cast<unsigned>(table_id));
    fprintf(f, "slug=%s\n", slug ? slug : "");
    fprintf(f, "visited_unix=%lld\n", static_cast<long long>(time(nullptr)));
    if (fclose(f) != 0) {
        ESP_LOGE(TAG, "close %s: %s", tmp_path, strerror(errno));
        unlink(tmp_path);
        return false;
    }

    if (rename(tmp_path, path) != 0) {
        ESP_LOGE(TAG, "rename %s -> %s: %s", tmp_path, path, strerror(errno));
        unlink(tmp_path);
        return false;
    }
    ESP_LOGI(TAG, "visited table %u (%s)", static_cast<unsigned>(table_id),
             slug && slug[0] ? slug : "unknown");
    return true;
}

// Sponsor placement may change during the event. Search the persisted records
// rather than the compiled 5x5 range so known and unknown sponsors can move to
// or from overflow table IDs without leaving two current assignments behind.
void clear_other_tables_for_slug(SponsorTableId current_table,
                                 const char *slug)
{
    while (slug && slug[0]) {
        SponsorTableId match = 0;
        DIR *dir = opendir(kStoreDir);
        if (!dir) return;

        while (const dirent *entry = readdir(dir)) {
            SponsorTableId table_id = 0;
            if (!table_from_filename(entry->d_name, &table_id)
                || table_id == current_table) {
                continue;
            }

            char other_slug[kSponsorSlugMaxLength + 1] = {};
            if (read_visit(table_id, other_slug, sizeof(other_slug))
                && strcmp(other_slug, slug) == 0) {
                match = table_id;
                break;
            }
        }
        closedir(dir);

        if (!match) return;
        if (!write_visit(match, "")) {
            ESP_LOGW(TAG, "could not clear moved sponsor %s from table %u",
                     slug, static_cast<unsigned>(match));
            return;
        }
    }
}

SponsorVisitResult invalid(SponsorVisitStatus status)
{
    SponsorVisitResult result;
    result.status = status;
    return result;
}

} // namespace

SponsorVisitResult sponsor_store_record_payload(const char *payload)
{
    if (!payload || strncmp(payload, kTagPrefix, strlen(kTagPrefix)) != 0) {
        return invalid(SponsorVisitStatus::NotSponsor);
    }

    const char *slug_begin = payload + strlen(kTagPrefix);
    const char *separator = strchr(slug_begin, ':');
    if (!separator || separator == slug_begin || strchr(separator + 1, ':')) {
        return invalid(SponsorVisitStatus::InvalidPayload);
    }

    const size_t slug_length = static_cast<size_t>(separator - slug_begin);
    if (slug_length > kSponsorSlugMaxLength) {
        return invalid(SponsorVisitStatus::InvalidPayload);
    }

    char slug[kSponsorSlugMaxLength + 1];
    for (size_t i = 0; i < slug_length; ++i) {
        if (!slug_char_allowed(slug_begin[i])) {
            return invalid(SponsorVisitStatus::InvalidPayload);
        }
        slug[i] = slug_begin[i];
    }
    slug[slug_length] = '\0';

    for (const char *p = separator + 1; *p; ++p) {
        if (*p < '0' || *p > '9') {
            return invalid(SponsorVisitStatus::InvalidPayload);
        }
    }

    char *end = nullptr;
    const long parsed_table = strtol(separator + 1, &end, 10);
    if (!end || *end != '\0' || end == separator + 1
        || parsed_table < 1
        || static_cast<unsigned long>(parsed_table) > kMaxTableId) {
        return invalid(SponsorVisitStatus::InvalidPayload);
    }

    const SponsorTableId table_id = static_cast<SponsorTableId>(parsed_table);
    const SponsorInfo *sponsor = sponsor_catalog_find_slug(slug);

    char current_slug[kSponsorSlugMaxLength + 1] = {};
    const bool was_visited = read_visit(table_id, current_slug,
                                        sizeof(current_slug));
    if (!was_visited || strcmp(current_slug, slug) != 0) {
        if (!write_visit(table_id, slug)) {
            return invalid(SponsorVisitStatus::StorageError);
        }
    }

    clear_other_tables_for_slug(table_id, slug);

    SponsorVisitResult result;
    result.status = was_visited ? SponsorVisitStatus::AlreadyVisited
                                : SponsorVisitStatus::NewVisit;
    result.sponsor = sponsor;
    result.table_id = table_id;
    snprintf(result.slug, sizeof(result.slug), "%s", slug);
    return result;
}

bool sponsor_store_get_table_state(SponsorTableId table_id,
                                   SponsorTableState *out)
{
    if (out) *out = {};

    char slug[kSponsorSlugMaxLength + 1] = {};
    if (!read_visit(table_id, slug, sizeof(slug))) return false;

    if (out) {
        out->visited = true;
        out->sponsor = slug[0] ? sponsor_catalog_find_slug(slug) : nullptr;
        snprintf(out->slug, sizeof(out->slug), "%s", slug);
    }
    return true;
}

bool sponsor_store_is_visited(SponsorTableId table_id)
{
    return sponsor_store_get_table_state(table_id, nullptr);
}

size_t sponsor_store_count()
{
    size_t count = 0;
    DIR *dir = opendir(kStoreDir);
    if (!dir) return 0;

    while (const dirent *entry = readdir(dir)) {
        SponsorTableId table_id = 0;
        if (table_from_filename(entry->d_name, &table_id)
            && sponsor_store_is_visited(table_id)) {
            ++count;
        }
    }
    closedir(dir);
    return count;
}

size_t sponsor_store_count_after(SponsorTableId last_table_id)
{
    size_t count = 0;
    DIR *dir = opendir(kStoreDir);
    if (!dir) return 0;

    while (const dirent *entry = readdir(dir)) {
        SponsorTableId table_id = 0;
        if (table_from_filename(entry->d_name, &table_id)
            && table_id > last_table_id
            && sponsor_store_is_visited(table_id)) {
            ++count;
        }
    }
    closedir(dir);
    return count;
}

size_t sponsor_store_list_assignments(SponsorStoredAssignment *out,
                                      size_t capacity)
{
    if (!out || capacity == 0) return 0;

    size_t count = 0;
    DIR *dir = opendir(kStoreDir);
    if (!dir) return 0;

    while (const dirent *entry = readdir(dir)) {
        SponsorTableId table_id = 0;
        SponsorTableState state;
        if (!table_from_filename(entry->d_name, &table_id)
            || !sponsor_store_get_table_state(table_id, &state)
            || !state.slug[0]) {
            continue;
        }

        if (count == capacity) continue;
        SponsorStoredAssignment &assignment = out[count++];
        assignment.table_id = table_id;
        assignment.sponsor = state.sponsor;
        snprintf(assignment.slug, sizeof(assignment.slug), "%s", state.slug);
    }
    closedir(dir);

    // Directory order is filesystem-specific; table order keeps the appended
    // unknown-sponsor rows deterministic across boots and sync restores.
    for (size_t i = 1; i < count; ++i) {
        const SponsorStoredAssignment current = out[i];
        size_t position = i;
        while (position > 0
               && out[position - 1].table_id > current.table_id) {
            out[position] = out[position - 1];
            --position;
        }
        out[position] = current;
    }
    return count;
}
