#include "hal_config.h"
#include "hal_fs.h"

#include "esp_log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

static const char *TAG = "hal_config";
static const char *CONFIG_DIR = "/littlefs/config";

static bool path_for(const char *slug, char *out, size_t out_sz)
{
    if (!slug || !*slug) return false;
    int n = snprintf(out, out_sz, "%s/%s.cfg", CONFIG_DIR, slug);
    return n > 0 && (size_t)n < out_sz;
}

static bool read_int_from_file(const char *path, const char *key, int *out)
{
    FILE *f = fopen(path, "r");
    if (!f) return false;
    char line[64];
    size_t klen = strlen(key);
    bool found = false;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, key, klen) != 0) continue;
        if (line[klen] != '=') continue;
        *out = atoi(line + klen + 1);
        found = true;
        break;
    }
    fclose(f);
    return found;
}

static bool write_int_to_file(const char *path, const char *key, int value)
{
    char tmp_path[160];
    if (snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path) >= (int)sizeof(tmp_path))
        return false;

    FILE *src = fopen(path, "r");
    FILE *dst = fopen(tmp_path, "w");
    if (!dst) {
        if (src) fclose(src);
        ESP_LOGE(TAG, "open %s for write: %s", tmp_path, strerror(errno));
        return false;
    }

    bool replaced = false;
    size_t klen = strlen(key);
    if (src) {
        char line[64];
        while (fgets(line, sizeof(line), src)) {
            if (!replaced && strncmp(line, key, klen) == 0 && line[klen] == '=') {
                fprintf(dst, "%s=%d\n", key, value);
                replaced = true;
            } else {
                fputs(line, dst);
            }
        }
        fclose(src);
    }
    if (!replaced) fprintf(dst, "%s=%d\n", key, value);
    fclose(dst);

    if (rename(tmp_path, path) != 0) {
        ESP_LOGE(TAG, "rename %s -> %s: %s", tmp_path, path, strerror(errno));
        unlink(tmp_path);
        return false;
    }
    return true;
}

// Longest supported line: launcher_order value (<=1024) + key + '=' + NL.
// A smaller buffer silently truncates long values on read (dropping the
// tail of the launcher grid once ~30 apps are installed) and, worse,
// splits the target line into chunks on write, preserving the leftover
// tail as garbage lines that accumulate on every persist.
//
// Heap-allocated, not stacked: both string functions run on the 3584-byte
// main task at boot (register_all -> persist, slugs_load), where a 1 KB
// frame overflows the stack. malloc failure degrades to false, the same
// contract as every other I/O error here.
constexpr size_t kMaxLineBytes = 1152;

static bool read_string_from_file(const char *path, const char *key,
                                  char *out, size_t out_sz)
{
    if (out_sz == 0) return false;
    FILE *f = fopen(path, "r");
    if (!f) return false;
    char *line = (char *)malloc(kMaxLineBytes);
    if (!line) {
        fclose(f);
        return false;
    }
    size_t klen = strlen(key);
    bool found = false;
    while (fgets(line, kMaxLineBytes, f)) {
        if (strncmp(line, key, klen) != 0) continue;
        if (line[klen] != '=') continue;
        const char *v = line + klen + 1;
        size_t vlen = strlen(v);
        while (vlen > 0 && (v[vlen - 1] == '\n' || v[vlen - 1] == '\r')) vlen--;
        size_t copy = vlen < out_sz - 1 ? vlen : out_sz - 1;
        memcpy(out, v, copy);
        out[copy] = '\0';
        found = true;
        break;
    }
    free(line);
    fclose(f);
    return found;
}

static bool write_string_to_file(const char *path, const char *key, const char *value)
{
    char tmp_path[160];
    if (snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path) >= (int)sizeof(tmp_path))
        return false;

    FILE *src = fopen(path, "r");
    FILE *dst = fopen(tmp_path, "w");
    if (!dst) {
        if (src) fclose(src);
        ESP_LOGE(TAG, "open %s for write: %s", tmp_path, strerror(errno));
        return false;
    }

    bool replaced = false;
    size_t klen = strlen(key);
    char *line = (char *)malloc(kMaxLineBytes);
    if (!line) {
        if (src) fclose(src);
        fclose(dst);
        unlink(tmp_path);
        return false;
    }
    if (src) {
        while (fgets(line, kMaxLineBytes, src)) {
            if (!replaced && strncmp(line, key, klen) == 0 && line[klen] == '=') {
                fprintf(dst, "%s=%s\n", key, value);
                replaced = true;
            } else {
                fputs(line, dst);
            }
        }
        fclose(src);
    }
    if (!replaced) fprintf(dst, "%s=%s\n", key, value);
    free(line);
    fclose(dst);

    if (rename(tmp_path, path) != 0) {
        ESP_LOGE(TAG, "rename %s -> %s: %s", tmp_path, path, strerror(errno));
        unlink(tmp_path);
        return false;
    }
    return true;
}

esp_err_t hal_config_init()
{
    if (!hal_fs_mkdir_p(CONFIG_DIR)) {
        ESP_LOGE(TAG, "could not create %s", CONFIG_DIR);
        return ESP_FAIL;
    }
    return ESP_OK;
}

int hal_config_get_int(const char *slug, const char *key, int def)
{
    if (!key) return def;
    char path[128];
    if (!path_for(slug, path, sizeof(path))) return def;
    int v = def;
    if (read_int_from_file(path, key, &v)) return v;
    return def;
}

void hal_config_set_int(const char *slug, const char *key, int v)
{
    if (!key) return;
    char path[128];
    if (!path_for(slug, path, sizeof(path))) return;
    write_int_to_file(path, key, v);
}

bool hal_config_get_string(const char *slug, const char *key,
                           char *out, size_t out_sz, const char *def)
{
    auto apply_default = [&]() {
        if (!out || out_sz == 0) return;
        if (def) {
            size_t dlen = strlen(def);
            size_t copy = dlen < out_sz - 1 ? dlen : out_sz - 1;
            memcpy(out, def, copy);
            out[copy] = '\0';
        } else {
            out[0] = '\0';
        }
    };
    if (!key || !out || out_sz == 0) {
        apply_default();
        return false;
    }
    char path[128];
    if (!path_for(slug, path, sizeof(path))) {
        apply_default();
        return false;
    }
    if (read_string_from_file(path, key, out, out_sz)) return true;
    apply_default();
    return false;
}

void hal_config_set_string(const char *slug, const char *key, const char *v)
{
    if (!key || !v) return;
    char path[128];
    if (!path_for(slug, path, sizeof(path))) return;
    write_string_to_file(path, key, v);
}
