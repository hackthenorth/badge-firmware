#include "share_bundle.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

struct PackCtx {
    uint8_t *buf = nullptr;
    FILE   *file = nullptr;
    uint8_t chunk[SHARE_STREAM_CHUNK_LEN];
    size_t   len = 0;
    size_t   cap = 0;
    int      files = 0;
    bool     manifest = false;
    bool     main = false;
    char     why[160] = {};
};

// Join a/b with one slash. Runtime sizes, so no -Wformat-truncation demand
// for provable bounds — overlong inputs fail instead of truncating, because
// a truncated path could collide two different files.
int join2(char *out, size_t outsz, const char *a, const char *b)
{
    size_t al = strlen(a), bl = strlen(b);
    if (al + 1 + bl + 1 > outsz) return 0;
    memcpy(out, a, al);
    out[al] = '/';
    memcpy(out + al + 1, b, bl + 1);
    return 1;
}

void pack_fail(PackCtx *ctx, const char *msg)
{
    size_t n = strlen(msg);
    if (n > sizeof(ctx->why) - 1) n = sizeof(ctx->why) - 1;
    memcpy(ctx->why, msg, n);
    ctx->why[n] = '\0';
}

int pack_reserve(PackCtx *ctx, size_t extra)
{
    if (extra > SHARE_BUNDLE_MAX_LEN - ctx->len) {
        pack_fail(ctx, "app is too big to share (48 KiB max)");
        return 0;
    }
    if (ctx->file || ctx->len + extra <= ctx->cap) return 1;
    size_t want = ctx->len + extra;
    if (want > SHARE_BUNDLE_MAX_LEN) {
        pack_fail(ctx, "app is too big to share (48 KiB max)");
        return 0;
    }
    size_t cap = ctx->cap ? ctx->cap * 2 : 4096;
    while (cap < want) cap *= 2;
    if (cap > SHARE_BUNDLE_MAX_LEN) cap = SHARE_BUNDLE_MAX_LEN;
    uint8_t * grown = (uint8_t *)realloc(ctx->buf, cap);
    if (!grown) {
        pack_fail(ctx, "out of memory packing the app");
        return 0;
    }
    ctx->buf = grown;
    ctx->cap = cap;
    return 1;
}

int pack_write(PackCtx *ctx, const void *data, size_t len)
{
    if (!pack_reserve(ctx, len)) return 0;
    if (ctx->file) {
        if (fwrite(data, 1, len, ctx->file) != len) {
            pack_fail(ctx, "cannot write the staged app");
            return 0;
        }
    } else {
        memcpy(ctx->buf + ctx->len, data, len);
    }
    ctx->len += len;
    return 1;
}

// Admission rule shared by pack and size: regular files only, no dotfiles,
// no appdata/ save data. One rule in one place so the list size and the
// bundle always agree.
int admissible(const char *rel, int is_dir)
{
    if (!rel || !*rel) return 0;
    if (rel[0] == '.' || rel[0] == '/') return 0;
    if (strcmp(rel, "appdata") == 0) return 0;
    if (strncmp(rel, "appdata/", 8) == 0) return 0;
    if (strchr(rel, '\\') != nullptr) return 0;
    // Lexical ".." check: no normalization to get wrong.
    const char *p = rel;
    while (*p) {
        const char *slash = strchr(p, '/');
        size_t seg = slash ? (size_t)(slash - p) : strlen(p);
        if (p[0] == '.') return 0;
        if (seg == 0) return 0;   // empty segment ("a//b")
        if (!slash) break;
        if (!slash[1]) return 0;
        p = slash + 1;
    }
    (void)is_dir;
    return 1;
}

int pack_file(PackCtx *ctx, const char *abs_path, const char *rel_path)
{
    if (ctx->files >= SHARE_BUNDLE_MAX_FILES) {
        pack_fail(ctx, "app has too many files to share (16 max)");
        return 0;
    }
    size_t path_len = strlen(rel_path);
    if (path_len == 0 || path_len > SHARE_BUNDLE_MAX_PATH) {
        pack_fail(ctx, "app has a file path that is too long");
        return 0;
    }
    FILE *f = fopen(abs_path, "rb");
    if (!f) {
        pack_fail(ctx, "cannot read an app file");
        return 0;
    }
    setvbuf(f, nullptr, _IONBF, 0);
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f); pack_fail(ctx, "cannot seek an app file"); return 0;
    }
    long size = ftell(f);
    if (fseek(f, 0, SEEK_SET) != 0 || size < 0 || (size_t)size > SHARE_BUNDLE_MAX_FILE) {
        fclose(f);
        pack_fail(ctx, "app has a file that is too big to share");
        return 0;
    }
    if (!pack_reserve(ctx, 1 + path_len + 4 + (size_t)size)) {
        fclose(f);
        return 0;
    }
    const uint8_t pl = (uint8_t)path_len;
    uint32_t v = (uint32_t)size;
    const uint8_t dl[4] = {(uint8_t)v, (uint8_t)(v >> 8),
                           (uint8_t)(v >> 16), (uint8_t)(v >> 24)};
    if (!pack_write(ctx, &pl, 1) || !pack_write(ctx, rel_path, path_len)
        || !pack_write(ctx, dl, sizeof(dl))) {
        fclose(f);
        return 0;
    }
    size_t remaining = (size_t)size;
    while (remaining) {
        const size_t n = remaining < sizeof(ctx->chunk) ? remaining : sizeof(ctx->chunk);
        if (fread(ctx->chunk, 1, n, f) != n) {
            fclose(f); pack_fail(ctx, "cannot read an app file"); return 0;
        }
        if (!pack_write(ctx, ctx->chunk, n)) { fclose(f); return 0; }
        remaining -= n;
    }
    fclose(f);
    ctx->files++;
    ctx->manifest |= strcmp(rel_path, "manifest.cfg") == 0;
    ctx->main |= strcmp(rel_path, "main.lua") == 0;
    return 1;
}

int pack_walk(PackCtx *ctx, const char *abs_base, const char *rel_base, int depth)
{
    if (depth > SHARE_BUNDLE_MAX_DEPTH) {
        pack_fail(ctx, "app directory is nested too deep to share");
        return 0;
    }
    char dir_path[256];
    if (rel_base[0]) {
        if (!join2(dir_path, sizeof(dir_path), abs_base, rel_base)) {
            pack_fail(ctx, "app has a file path that is too long");
            return 0;
        }
    } else {
        size_t n = strlen(abs_base);
        if (n + 1 > sizeof(dir_path)) {
            pack_fail(ctx, "app has a file path that is too long");
            return 0;
        }
        memcpy(dir_path, abs_base, n + 1);
    }
    DIR *d = opendir(dir_path);
    if (!d) {
        pack_fail(ctx, "cannot read the app directory");
        return 0;
    }
    // Stream entries straight from readdir: no name table on the stack (the
    // LVGL task budget is tight and this recurses). Bundle order follows
    // directory order, which is fine — unpack does not care.
    struct dirent *de;
    while ((de = readdir(d)) != nullptr) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;
        char rel[256], abs[300];
        if (rel_base[0]) {
            if (!join2(rel, sizeof(rel), rel_base, de->d_name)) continue;
        } else {
            size_t n = strlen(de->d_name);
            if (n + 1 > sizeof(rel)) continue;
            memcpy(rel, de->d_name, n + 1);
        }
        if (strlen(rel) > 200) continue;
        if (!admissible(rel, 0)) continue;
        if (!join2(abs, sizeof(abs), abs_base, rel)) continue;
        struct stat st;
        if (stat(abs, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            if (!pack_walk(ctx, abs_base, rel, depth + 1)) {
                closedir(d);
                return 0;
            }
        } else if (S_ISREG(st.st_mode)) {
            if (!pack_file(ctx, abs, rel)) {
                closedir(d);
                return 0;
            }
        }
    }
    closedir(d);
    return 1;
}

int has_file(PackCtx *ctx, const char *want_name)
{
    // Scan the packed records for a top-level leaf name. Records start past
    // the 6-byte header placeholder.
    size_t pos = 6;
    size_t want = strlen(want_name);
    while (pos < ctx->len) {
        if (pos + 1 > ctx->len) break;
        uint8_t pl = ctx->buf[pos++];
        if (pos + pl + 4 > ctx->len) break;
        if (pl == want && memcmp(ctx->buf + pos, want_name, want) == 0) return 1;
        pos += pl;
        uint32_t dl = (uint32_t)ctx->buf[pos]
            | ((uint32_t)ctx->buf[pos + 1] << 8)
            | ((uint32_t)ctx->buf[pos + 2] << 16)
            | ((uint32_t)ctx->buf[pos + 3] << 24);
        pos += 4 + dl;
        if (pos > ctx->len) break;
    }
    return 0;
}

} // namespace

uint32_t share_bundle_crc32(const uint8_t *data, size_t len)
{
    return share_bundle_crc32_update(0, data, len);
}

uint32_t share_bundle_crc32_update(uint32_t seed, const uint8_t *data, size_t len)
{
    // Bitwise, no 1 KiB lookup table: DRAM is the binding constraint on this
    // badge (see the radio-init heap notes), and this runs on pack/verify
    // only — a few ms on a 48 KiB bundle, never on a hot path. Check value
    // for "123456789" is 0xCBF43926 (pinned by tools/share-test).
    uint32_t crc = seed ^ 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int k = 0; k < 8; k++) {
            crc = (crc & 1) ? (0xEDB88320u ^ (crc >> 1)) : (crc >> 1);
        }
    }
    return crc ^ 0xFFFFFFFFu;
}

int share_bundle_valid_slug(const char *slug)
{
    if (!slug) return 0;
    size_t len = strlen(slug);
    if (len == 0 || len > 32) return 0;
    if (!((slug[0] >= 'a' && slug[0] <= 'z') || (slug[0] >= '0' && slug[0] <= '9'))) return 0;
    for (size_t i = 1; i < len; i++) {
        char c = slug[i];
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-') continue;
        return 0;
    }
    return 1;
}

uint8_t *share_bundle_pack(const char *app_dir, size_t *out_len,
                           char *why, size_t why_sz)
{
    PackCtx ctx;
    if (!app_dir || !*app_dir || !out_len) {
        if (why && why_sz) snprintf(why, why_sz, "bad arguments");
        return nullptr;
    }
    *out_len = 0;
    struct stat st;
    if (stat(app_dir, &st) != 0 || !S_ISDIR(st.st_mode)) {
        if (why && why_sz) snprintf(why, why_sz, "app directory is missing");
        return nullptr;
    }
    // Header placeholder: magic + version + count filled in at the end.
    if (!pack_reserve(&ctx, 6)) {
        if (why && why_sz) snprintf(why, why_sz, "%s", ctx.why);
        free(ctx.buf);
        return nullptr;
    }
    ctx.len = 6;
    if (!pack_walk(&ctx, app_dir, "", 0)) {
        if (why && why_sz) snprintf(why, why_sz, "%s", ctx.why);
        free(ctx.buf);
        return nullptr;
    }
    if (ctx.files == 0) {
        if (why && why_sz) snprintf(why, why_sz, "app directory is empty");
        free(ctx.buf);
        return nullptr;
    }
    if (!has_file(&ctx, "manifest.cfg") || !has_file(&ctx, "main.lua")) {
        if (why && why_sz) snprintf(why, why_sz, "app is missing manifest.cfg or main.lua");
        free(ctx.buf);
        return nullptr;
    }
    ctx.buf[0] = SHARE_BUNDLE_MAGIC_0;
    ctx.buf[1] = SHARE_BUNDLE_MAGIC_1;
    ctx.buf[2] = SHARE_BUNDLE_MAGIC_2;
    ctx.buf[3] = SHARE_BUNDLE_MAGIC_3;
    ctx.buf[4] = SHARE_BUNDLE_VERSION;
    ctx.buf[5] = (uint8_t)ctx.files;
    *out_len = ctx.len;
    return ctx.buf;
}

int share_bundle_pack_file(const char *app_dir, const char *bundle_path,
                           size_t *out_len, uint32_t *out_crc,
                           char *why, size_t why_sz)
{
    if (out_len) *out_len = 0;
    if (out_crc) *out_crc = 0;
    if (why && why_sz) why[0] = '\0';
    struct stat st;
    if (!app_dir || !bundle_path || !out_len || !out_crc
        || stat(app_dir, &st) != 0 || !S_ISDIR(st.st_mode)) {
        if (why && why_sz) snprintf(why, why_sz, "app directory is missing");
        return 0;
    }
    PackCtx ctx;
    ctx.file = fopen(bundle_path, "w+b");
    if (!ctx.file) {
        if (why && why_sz) snprintf(why, why_sz, "cannot stage the app on storage");
        return 0;
    }
    setvbuf(ctx.file, nullptr, _IONBF, 0);
    uint8_t header[6] = {'A', 'P', 'P', 'B', SHARE_BUNDLE_VERSION, 0};
    bool ok = pack_write(&ctx, header, sizeof(header)) && pack_walk(&ctx, app_dir, "", 0);
    if (ok && (!ctx.manifest || !ctx.main)) {
        pack_fail(&ctx, "app is missing manifest.cfg or main.lua");
        ok = false;
    }
    header[5] = (uint8_t)ctx.files;
    if (ok) ok = fseek(ctx.file, 0, SEEK_SET) == 0
        && fwrite(header, 1, sizeof(header), ctx.file) == sizeof(header)
        && fflush(ctx.file) == 0 && fseek(ctx.file, 0, SEEK_SET) == 0;
    uint32_t crc = 0;
    for (size_t remaining = ctx.len; ok && remaining;) {
        const size_t n = remaining < sizeof(ctx.chunk) ? remaining : sizeof(ctx.chunk);
        ok = fread(ctx.chunk, 1, n, ctx.file) == n;
        if (ok) crc = share_bundle_crc32_update(crc, ctx.chunk, n);
        remaining -= n;
    }
    if (fclose(ctx.file) != 0) ok = false;
    if (!ok) {
        unlink(bundle_path);
        if (why && why_sz) snprintf(why, why_sz, "%s",
            ctx.why[0] ? ctx.why : "cannot write or verify the staged app");
        return 0;
    }
    *out_len = ctx.len;
    *out_crc = crc;
    return 1;
}

namespace {

struct BundleReader {
    const uint8_t *data;
    FILE *file;
    size_t len;
    size_t pos = 0;
    bool read(void *out, size_t count) {
        if (count > len - pos) return false;
        if (file) {
            if (fread(out, 1, count, file) != count) return false;
        } else if (count) {
            memcpy(out, data + pos, count);
        }
        pos += count;
        return true;
    }
};

int unpack_reader(BundleReader &reader, const char *staging_dir,
                  char *why, size_t why_sz)
{
    if (why && why_sz) why[0] = '\0';
    uint8_t header[6];
    if (reader.len > SHARE_BUNDLE_MAX_LEN || !staging_dir || !*staging_dir
        || !reader.read(header, sizeof(header)) || memcmp(header, "APPB", 4)
        || header[4] != SHARE_BUNDLE_VERSION || !header[5]
        || header[5] > SHARE_BUNDLE_MAX_FILES) {
        if (why && why_sz) snprintf(why, why_sz, "transfer is not a valid app bundle");
        return 0;
    }
    struct stat st;
    if (stat(staging_dir, &st) == 0) {
        if (why && why_sz) snprintf(why, why_sz, "staging area is busy, try again");
        return 0;
    }
    if (mkdir(staging_dir, 0755) != 0) {
        if (why && why_sz) snprintf(why, why_sz, "cannot stage the app");
        return 0;
    }
    bool got_manifest = false, got_main = false;
    uint8_t chunk[SHARE_STREAM_CHUNK_LEN];
    for (unsigned i = 0; i < header[5]; ++i) {
        uint8_t pl = 0, length[4];
        char rel[SHARE_BUNDLE_MAX_PATH + 1];
        if (!reader.read(&pl, 1) || !pl || pl > SHARE_BUNDLE_MAX_PATH
            || !reader.read(rel, pl) || memchr(rel, '\0', pl)
            || !reader.read(length, sizeof(length))) goto corrupt;
        rel[pl] = '\0';
        const uint32_t dl = (uint32_t)length[0] | ((uint32_t)length[1] << 8)
            | ((uint32_t)length[2] << 16) | ((uint32_t)length[3] << 24);
        int depth = 0;
        for (const char *p = rel; *p; ++p) depth += *p == '/';
        if (dl > SHARE_BUNDLE_MAX_FILE || dl > reader.len - reader.pos
            || depth > SHARE_BUNDLE_MAX_DEPTH || !admissible(rel, 0)) goto corrupt;
        char out_path[256];
        if (!join2(out_path, sizeof(out_path), staging_dir, rel)) goto corrupt;
        char *slash = strchr(out_path + strlen(staging_dir) + 1, '/');
        while (slash) {
            *slash = '\0';
            if (stat(out_path, &st) == 0) {
                if (!S_ISDIR(st.st_mode)) goto corrupt;
            } else if (mkdir(out_path, 0755) != 0) goto corrupt;
            *slash = '/';
            slash = strchr(slash + 1, '/');
        }
        // Reject duplicate paths and file/directory collisions.
        if (stat(out_path, &st) == 0) goto corrupt;
        FILE *f = fopen(out_path, "wb");
        if (!f) goto corrupt;
        setvbuf(f, nullptr, _IONBF, 0);
        bool ok = true;
        for (size_t remaining = dl; remaining && ok;) {
            const size_t n = remaining < sizeof(chunk) ? remaining : sizeof(chunk);
            ok = reader.read(chunk, n) && fwrite(chunk, 1, n, f) == n;
            remaining -= n;
        }
        if (fclose(f) != 0) ok = false;
        if (!ok) goto corrupt;
        got_manifest |= strcmp(rel, "manifest.cfg") == 0;
        got_main |= strcmp(rel, "main.lua") == 0;
    }
    if (reader.pos != reader.len || !got_manifest || !got_main) goto corrupt;
    return 1;
corrupt:
    if (why && why_sz) snprintf(why, why_sz, "transfer is corrupted or storage is full");
    share_bundle_remove_dir(staging_dir);
    return 0;
}

} // namespace

int share_bundle_unpack(const uint8_t *data, size_t len,
                        const char *staging_dir, char *why, size_t why_sz)
{
    BundleReader reader{data, nullptr, data ? len : 0};
    return unpack_reader(reader, staging_dir, why, why_sz);
}

int share_bundle_unpack_file(const char *bundle_path, const char *staging_dir,
                             char *why, size_t why_sz)
{
    struct stat st;
    if (!bundle_path || stat(bundle_path, &st) != 0 || !S_ISREG(st.st_mode)
        || st.st_size < 6 || st.st_size > SHARE_BUNDLE_MAX_LEN) {
        if (why && why_sz) snprintf(why, why_sz, "received app file is missing or invalid");
        return 0;
    }
    FILE *f = fopen(bundle_path, "rb");
    if (!f) {
        if (why && why_sz) snprintf(why, why_sz, "cannot read received app");
        return 0;
    }
    setvbuf(f, nullptr, _IONBF, 0);
    BundleReader reader{nullptr, f, (size_t)st.st_size};
    int ok = unpack_reader(reader, staging_dir, why, why_sz);
    if (fclose(f) != 0 && ok) {
        share_bundle_remove_dir(staging_dir);
        if (why && why_sz) snprintf(why, why_sz, "cannot close received app");
        ok = 0;
    }
    return ok;
}

int share_bundle_slug_from_manifest(const char *manifest_path,
                                    char *slug_out, size_t slug_sz)
{
    if (!manifest_path || !slug_out || slug_sz == 0) return 0;
    slug_out[0] = '\0';
    FILE *f = fopen(manifest_path, "r");
    if (!f) return 0;
    char line[256];
    int ok = 0;
    while (fgets(line, sizeof(line), f)) {
        // Tolerate leading space; require the key to start the entry.
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (strncmp(p, "slug", 4) != 0) continue;
        p += 4;
        while (*p == ' ' || *p == '\t') p++;
        if (*p != '=') continue;
        p++;
        while (*p == ' ' || *p == '\t') p++;
        char *end = p + strlen(p);
        while (end > p && (end[-1] == '\n' || end[-1] == '\r'
                           || end[-1] == ' ' || end[-1] == '\t')) end--;
        size_t n = (size_t)(end - p);
        if (n == 0 || n >= slug_sz || n > 32) continue;
        char tmp[33];
        memcpy(tmp, p, n);
        tmp[n] = '\0';
        if (!share_bundle_valid_slug(tmp)) continue;
        snprintf(slug_out, slug_sz, "%s", tmp);
        ok = 1;
        break;
    }
    fclose(f);
    return ok;
}

int share_bundle_patch_slug(const char *manifest_path, const char *new_slug)
{
    if (!manifest_path || !new_slug || !share_bundle_valid_slug(new_slug)) return 0;
    // Heap buffer: manifests are small but comment lines are unbounded, so
    // this must not live on the LVGL task stack.
    FILE *f = fopen(manifest_path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size < 0 || size > 8192) {
        fclose(f);
        return 0;
    }
    char *text = (char *)malloc((size_t)size + 1);
    if (!text) {
        fclose(f);
        return 0;
    }
    if (size > 0 && fread(text, 1, (size_t)size, f) != (size_t)size) {
        fclose(f);
        free(text);
        return 0;
    }
    fclose(f);
    // Second buffer for the patched copy: lines move, so patching in place
    // would need shifting; a straight copy is easier to get right.
    char *out = (char *)malloc((size_t)size + 64);
    if (!out) {
        free(text);
        return 0;
    }
    size_t olen = 0;
    int patched = 0;
    char *line = text;
    char *end = text + size;
    while (line < end) {
        char *eol = (char *)memchr(line, '\n', (size_t)(end - line));
        size_t llen = eol ? (size_t)(eol - line) + 1 : (size_t)(end - line);
        const char *p = line;
        while (p < line + llen && (*p == ' ' || *p == '\t')) p++;
        int is_slug = !patched && (size_t)(line + llen - p) >= 5
            && strncmp(p, "slug", 4) == 0;
        if (is_slug) {
            const char *q = p + 4;
            while (q < line + llen && (*q == ' ' || *q == '\t')) q++;
            is_slug = (q < line + llen && *q == '=');
        }
        if (is_slug) {
            // "slug=" + 32-char max slug + "\n" = 39 < 64 by construction.
            olen += snprintf(out + olen, 64, "slug=%.32s\n", new_slug);
            patched = 1;
        } else {
            memcpy(out + olen, line, llen);
            olen += llen;
        }
        line += llen;
    }
    free(text);
    if (!patched) {
        free(out);
        return 0;
    }
    f = fopen(manifest_path, "wb");
    if (!f) {
        free(out);
        return 0;
    }
    int ok = fwrite(out, 1, olen, f) == olen;
    fclose(f);
    free(out);
    return ok;
}

void share_bundle_remove_dir(const char *path)
{
    if (!path || !*path) return;
    DIR *d = opendir(path);
    if (!d) {
        unlink(path);
        return;
    }
    struct dirent *de;
    while ((de = readdir(d)) != nullptr) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;
        char child[300];
        if (!join2(child, sizeof(child), path, de->d_name)) continue;
        struct stat st;
        if (stat(child, &st) == 0 && S_ISDIR(st.st_mode)) {
            share_bundle_remove_dir(child);
        } else {
            unlink(child);
        }
    }
    closedir(d);
    rmdir(path);
}

size_t share_bundle_dir_size(const char *app_dir)
{
    if (!app_dir) return 0;
    size_t total = 0;
    DIR *d = opendir(app_dir);
    if (!d) return 0;
    struct dirent *de;
    while ((de = readdir(d)) != nullptr) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;
        char child[300];
        if (!join2(child, sizeof(child), app_dir, de->d_name)) continue;
        struct stat st;
        if (stat(child, &st) != 0) continue;
        if (S_ISREG(st.st_mode)) {
            if (de->d_name[0] == '.') continue;
            total += (size_t)st.st_size;
        } else if (S_ISDIR(st.st_mode)) {
            if (strcmp(de->d_name, "appdata") == 0 || de->d_name[0] == '.') continue;
            total += share_bundle_dir_size(child);
        }
    }
    closedir(d);
    return total;
}
