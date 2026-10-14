#include "contacts_store.h"
#include "hal_fs.h"

#include "esp_log.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *TAG = "contacts_store";
static const char *CONTACTS_DIR = "/littlefs/config/contacts";

// badge_id is server-generated and should be alphanumeric, but it arrives
// over the air from an untrusted peer — anyone can advertise a BMP2 frame
// with whatever they like in it. Reduce it to a safe filename so a hostile
// badge_id can't escape the contacts directory or overwrite arbitrary files.
static void safe_key(const char *badge_id, char *out, size_t out_sz)
{
    size_t o = 0;
    for (size_t i = 0; badge_id[i] && o + 1 < out_sz; i++) {
        char c = badge_id[i];
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
               || (c >= '0' && c <= '9') || c == '-' || c == '_';
        out[o++] = ok ? c : '_';
    }
    out[o] = '\0';
}

// Values land in a key=value file read back by hal_config, which is
// line-oriented. A newline inside a peer-supplied string would forge extra
// keys, so strip anything that could break the format.
static void sanitize_value(const char *src, char *out, size_t out_sz)
{
    size_t o = 0;
    for (size_t i = 0; src[i] && o + 1 < out_sz; i++) {
        unsigned char c = (unsigned char)src[i];
        out[o++] = (c == '\n' || c == '\r') ? ' ' : (char)c;
    }
    out[o] = '\0';
}

bool contacts_store_save(const BumpCard &card, bool *added)
{
    if (added) *added = false;
    if (card.badge_id[0] == '\0') return false;

    if (!hal_fs_mkdir_p(CONTACTS_DIR)) {
        ESP_LOGE(TAG, "mkdir %s failed", CONTACTS_DIR);
        return false;
    }

    char key[sizeof(card.badge_id)];
    safe_key(card.badge_id, key, sizeof(key));
    if (key[0] == '\0') return false;

    char path[256];
    if (snprintf(path, sizeof(path), "%s/%s.cfg", CONTACTS_DIR, key) >= (int)sizeof(path))
        return false;
    // ESP-IDF's VFS access() result is not reliable for LittleFS on the badge:
    // it can report ENOENT for a file that is present, which made every repeat
    // bump look new to the caller even though rename() replaced the same file.
    // stat() is implemented by the LittleFS VFS and is also what the console's
    // live directory listing uses.
    struct stat existing = {};
    const bool is_new = stat(path, &existing) != 0;

    char tmp_path[272];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);

    FILE *f = fopen(tmp_path, "w");
    if (!f) {
        ESP_LOGE(TAG, "open %s: %s", tmp_path, strerror(errno));
        return false;
    }

    char buf[80];
    #define WRITE_FIELD(k, v) do { sanitize_value((v), buf, sizeof(buf)); \
                                   fprintf(f, k "=%s\n", buf); } while (0)
    WRITE_FIELD("badge_id",  card.badge_id);
    WRITE_FIELD("name",      card.display_name);
    WRITE_FIELD("email",     card.net_email);
    WRITE_FIELD("phone",     card.net_phone);
    WRITE_FIELD("linkedin",  card.net_linkedin);
    WRITE_FIELD("discord",   card.net_discord);
    WRITE_FIELD("instagram", card.net_instagram);
    WRITE_FIELD("x",         card.net_x);
    #undef WRITE_FIELD
    fprintf(f, "role=%u\n", (unsigned)card.role);
    // Raw snapshots can reconstruct which successful contacts carried Ripple,
    // but the curated contacts API intentionally does not expose this field.
    fprintf(f, "ripple_depth=%d\n",
            card.ripple_active ? static_cast<int>(card.ripple_depth) : -1);
    fprintf(f, "received_unix=%lld\n", (long long)time(nullptr));
    fclose(f);

    if (rename(tmp_path, path) != 0) {
        ESP_LOGE(TAG, "rename %s -> %s: %s", tmp_path, path, strerror(errno));
        unlink(tmp_path);
        return false;
    }
    ESP_LOGI(TAG, "saved contact %s", key);
    if (added) *added = is_new;
    return true;
}

size_t contacts_store_count()
{
    DIR *d = opendir(CONTACTS_DIR);
    if (!d) return 0;

    size_t n = 0;
    struct dirent *e;
    while ((e = readdir(d)) != nullptr) {
        const char *dot = strrchr(e->d_name, '.');
        if (dot && strcmp(dot, ".cfg") == 0) n++;
    }
    closedir(d);
    return n;
}

static bool is_cfg(const char *name)
{
    const char *dot = strrchr(name, '.');
    return dot && strcmp(dot, ".cfg") == 0;
}

// Bounded copy: snprintf into a smaller buffer trips -Wformat-truncation
// (an error here), and overlapping snprintf trips -Wrestrict.
static void copy_name(char *dst, const char *src, size_t dst_sz)
{
    size_t len = strlen(src);
    if (len >= dst_sz) len = dst_sz - 1;
    memcpy(dst, src, len);
    dst[len] = '\0';
}

// Filenames are <safe_key>.cfg with safe_key up to 63 bytes, so 68+NUL.
constexpr size_t kContactNameBytes = 80;
constexpr size_t kMaxContactsListed = 128;

bool contacts_store_get(size_t index, ContactRecord *out)
{
    if (!out) return false;
    DIR *d = opendir(CONTACTS_DIR);
    if (!d) return false;

    // Collect names first; readdir order is filesystem-dependent.
    char names[kMaxContactsListed][kContactNameBytes];
    size_t n = 0;
    struct dirent *e;
    while ((e = readdir(d)) != nullptr && n < kMaxContactsListed) {
        if (!is_cfg(e->d_name)) continue;
        copy_name(names[n], e->d_name, sizeof(names[n]));
        ++n;
    }
    closedir(d);
    if (index >= n) return false;

    // Insertion sort with memmove (no restrict pitfalls): contact lists
    // are tiny.
    for (size_t i = 1; i < n; ++i) {
        char tmp[kContactNameBytes];
        copy_name(tmp, names[i], sizeof(tmp));
        size_t j = i;
        while (j > 0 && strcmp(names[j - 1], tmp) > 0) {
            memmove(names[j], names[j - 1], sizeof(names[j]));
            --j;
        }
        memmove(names[j], tmp, sizeof(names[j]));
    }

    char path[256];
    if (snprintf(path, sizeof(path), "%s/%s", CONTACTS_DIR, names[index])
        >= (int)sizeof(path)) return false;
    FILE *f = fopen(path, "r");
    if (!f) return false;

    ContactRecord record = {};
    char line[160];
    while (fgets(line, sizeof(line), f)) {
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        char *value = eq + 1;
        size_t vlen = strlen(value);
        while (vlen > 0 && (value[vlen - 1] == '\n' || value[vlen - 1] == '\r')) {
            value[--vlen] = '\0';
        }
        if (!strcmp(line, "name")) {
            snprintf(record.name, sizeof(record.name), "%s", value);
        } else if (!strcmp(line, "badge_id")) {
            snprintf(record.badge_id, sizeof(record.badge_id), "%s", value);
        } else if (!strcmp(line, "role")) {
            record.role = (uint8_t)atoi(value);
        } else if (!strcmp(line, "received_unix")) {
            record.received_unix = atoll(value);
        }
    }
    fclose(f);
    if (record.badge_id[0] == '\0') return false;
    *out = record;
    return true;
}
