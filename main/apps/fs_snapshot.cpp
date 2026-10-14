#include "fs_snapshot.h"

#include "hal_fs.h"
#include "hal_identity.h"
#include "hal_radio.h"
#include "json_util.h"

#include "esp_heap_caps.h"
#include "esp_rom_crc.h"
#include "esp_timer.h"

#include <dirent.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>   // strcasecmp
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>    // unlink

#ifndef BADGE_BUILD_NUMBER
#define BADGE_BUILD_NUMBER 0
#endif

namespace {

// Held back so the closing "counts" object always fits, however the walk went.
constexpr size_t kTailReserve = 512;

// Bytes examined by the binary sniff. A file whose first half-KB is clean text
// but that turns binary later is still caught, by the UTF-8 validation in the
// escaper — this gate just avoids reading and escaping an obvious binary.
constexpr size_t kSniffBytes = 512;

// A truncating cut can land at most 3 bytes inside a UTF-8 sequence. A larger
// gap between the read length and the last valid boundary means the file has
// real garbage in it, not just an unlucky cut.
constexpr size_t kMaxCodepointBackoff = 3;

// Gate 1: reject by name, without opening the file. apps/<slug>/icon.bin is the
// only binary in the shipped layout and costs ~5 KB of LittleFS reads per app
// per sync, so not opening it is worth a little redundancy with the sniff.
const char *const kBinaryExts[] = {
    ".bin", ".png", ".jpg", ".jpeg", ".gif", ".bmp",
    ".ttf", ".otf", ".lz4", ".raw", ".wav", ".mp3", ".z",
};

// Visited first, in this order, so that running out of budget degrades
// predictably: identity and config survive, Lua app sources are what go first.
// Anything not named here is walked afterwards in readdir order.
const char *const kWalkOrder[] = {
    "identity.json",
    "solana.json",
    "config",
    "apps",
};

// Writer shims: JsonOut (RAM builder) and FileOut (file streamer) share one
// budget/overflow/rewind contract, so the walk below is templated over the
// writer and behaves identically on both.
static bool w_putc(JsonOut &w, char c) { return jout_putc(w, c); }
static bool w_putc(FileOut &w, char c) { return fout_putc(w, c); }
static bool w_puts(JsonOut &w, const char *s) { return jout_puts(w, s); }
static bool w_puts(FileOut &w, const char *s) { return fout_puts(w, s); }
static bool w_printf(JsonOut &w, const char *fmt, ...)  __attribute__((format(printf, 2, 3)));
static bool w_printf(FileOut &w, const char *fmt, ...)  __attribute__((format(printf, 2, 3)));
static bool w_printf(JsonOut &w, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    char tmp[192];
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= sizeof(tmp)) return false;
    return jout_putn(w, tmp, (size_t)n);
}
static bool w_printf(FileOut &w, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    char tmp[192];
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= sizeof(tmp)) { w.overflow = true; return false; }
    return fout_putn(w, tmp, (size_t)n);
}
static size_t w_mark(const JsonOut &w) { return jout_mark(w); }
static size_t w_mark(const FileOut &w) { return fout_mark(w); }
static bool w_rewind(JsonOut &w, size_t mark) { jout_rewind(w, mark); return true; }
static bool w_rewind(FileOut &w, size_t mark) { return fout_rewind(w, mark); }
static void w_release_reserve(JsonOut &w) { jout_release_reserve(w); }
static void w_release_reserve(FileOut &w) { fout_release_reserve(w); }

template <typename W>
struct CtxT {
    W                out;
    FsSnapshotStats *st;
    char             path[kFsSnapshotMaxPath];
    size_t           root_len;
    const char      *skip_path;  // staging file: walked over, never included
    uint8_t         *chunk;      // RAM builder's kFsSnapshotMaxFile scratch
    bool             first;      // comma tracking inside "files"
};

enum class Written { Ok, Overflow, Malformed };

bool ends_with_ci(const char *s, const char *suffix)
{
    size_t ls = strlen(s), lx = strlen(suffix);
    if (lx > ls) return false;
    return strcasecmp(s + ls - lx, suffix) == 0;
}

bool denied_ext(const char *name)
{
    for (const char *ext : kBinaryExts) {
        if (ends_with_ci(name, ext)) return true;
    }
    return false;
}

// Gate 2: bytes that well-formed UTF-8 text never contains.
bool looks_binary(const uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        uint8_t c = p[i];
        if (c == 0x00 || c == 0x7F) return true;
        if (c <= 0x08) return true;
        if (c == 0x0B || c == 0x0C) return true;
        if (c >= 0x0E && c <= 0x1F) return true;
    }
    return false;
}

bool is_dot_entry(const char *n)
{
    return n[0] == '.' && (n[1] == '\0' || (n[1] == '.' && n[2] == '\0'));
}

bool in_walk_order(const char *name)
{
    for (const char *w : kWalkOrder) {
        if (strcmp(w, name) == 0) return true;
    }
    return false;
}

// Writes one "key":{...} member, whole or not at all. On failure the writer is
// rewound to exactly where it started, so the caller can try a smaller form.
template <typename W>
Written write_entry(CtxT<W> &c, const char *key, long size,
                    const uint8_t *content, size_t clen, bool trunc,
                    const char *skip_reason)
{
    size_t mark = w_mark(c.out);
    bool ok = true;

    if (!c.first) ok = w_putc(c.out, ',');
    ok = ok && w_putc(c.out, '"');
    ok = ok && json_escape_stream(c.out, (const uint8_t *)key, strlen(key));
    ok = ok && w_puts(c.out, "\":{\"size\":");
    ok = ok && w_printf(c.out, "%ld", size);

    if (ok && skip_reason) {
        ok = w_printf(c.out, ",\"skip\":\"%s\"", skip_reason);
    } else if (ok) {
        ok = ok && w_puts(c.out, ",\"text\":\"");
        ok = ok && json_escape_stream(c.out, content, clen);
        ok = ok && w_putc(c.out, '"');
        if (ok && trunc) ok = w_puts(c.out, ",\"trunc\":true");
    }
    ok = ok && w_putc(c.out, '}');

    if (!ok) {
        // Read the flag before rewinding — rewind deliberately clears it.
        Written why = c.out.overflow ? Written::Overflow : Written::Malformed;
        if (!w_rewind(c.out, mark)) why = Written::Overflow;
        return why;
    }
    c.first = false;
    return Written::Ok;
}

// Stub form. If even this doesn't fit we are inside the tail reserve and the
// file is simply absent; the caller still counts it and clears `complete`.
template <typename W>
bool write_stub(CtxT<W> &c, const char *key, long size, const char *reason)
{
    return write_entry(c, key, size, nullptr, 0, false, reason) == Written::Ok;
}

// Records a failed content entry as the stub it degrades to. Shared by the
// streaming tail below: overflow (over budget) stubs as "budget", anything
// else (binary, malformed, IO trouble mid-entry) as "binary".
template <typename W>
void finish_failed_entry(CtxT<W> &c, const char *key, long size, bool was_overflow)
{
    FsSnapshotStats *st = c.st;
    if (was_overflow) {
        if (!write_stub(c, key, size, "budget")) { /* no room even for that */ }
        st->files_dropped++;
        st->complete = false;
    } else {
        if (!write_stub(c, key, size, "binary")) st->complete = false;
        st->files_binary++;
    }
}

template <typename W>
void handle_file(CtxT<W> &c, long size, bool stat_ok)
{
    FsSnapshotStats *st = c.st;
    // The staging file is our own output (or a stale one from a crashed run):
    // walked over, never included, and never counted.
    if (c.skip_path && strcmp(c.path, c.skip_path) == 0) return;
    st->files_seen++;

    // Keys are relative to the mount point: shorter on the wire, and the
    // server's copy doesn't bake in where the badge happens to mount.
    const char *key = c.path + c.root_len;
    while (*key == '/') key++;

    // A filename that isn't valid UTF-8 can't be a JSON key at all.
    size_t klen = strlen(key);
    if (utf8_valid_prefix((const uint8_t *)key, klen) != klen) {
        st->paths_skipped++;
        st->complete = false;
        return;
    }

    if (!stat_ok) {
        if (!write_stub(c, key, 0, "unreadable")) st->complete = false;
        st->files_unreadable++;
        return;
    }
    if (denied_ext(key)) {
        if (!write_stub(c, key, size, "binary")) st->complete = false;
        st->files_binary++;
        return;
    }
    if ((size_t)size > kFsSnapshotBigFile) {
        if (!write_stub(c, key, size, "too_big")) st->complete = false;
        st->files_too_big++;
        return;
    }

    size_t want = (size_t)size < kFsSnapshotMaxFile ? (size_t)size : kFsSnapshotMaxFile;
    bool trunc = (size_t)size > kFsSnapshotMaxFile;

    if (c.chunk) {
        // RAM builder: the whole content goes through one entry write.
        FILE *f = fopen(c.path, "rb");
        if (!f) {
            if (!write_stub(c, key, size, "unreadable")) st->complete = false;
            st->files_unreadable++;
            return;
        }
        size_t n = fread(c.chunk, 1, want, f);
        fclose(f);

        size_t sniff = n < kSniffBytes ? n : kSniffBytes;
        if (looks_binary(c.chunk, sniff)) {
            if (!write_stub(c, key, size, "binary")) st->complete = false;
            st->files_binary++;
            return;
        }

        if (trunc) {
            // Back off to a codepoint boundary. Slicing mid-sequence would emit
            // invalid UTF-8 and break the whole document, not just this entry.
            size_t vp = utf8_valid_prefix(c.chunk, n);
            if (n - vp <= kMaxCodepointBackoff) n = vp;
            // A bigger gap is genuine garbage; the escaper below rejects it.
        }

        switch (write_entry(c, key, size, c.chunk, n, trunc, nullptr)) {
            case Written::Ok:
                st->files_text++;
                if (trunc) st->truncated++;
                return;
            case Written::Malformed:
                if (!write_stub(c, key, size, "binary")) st->complete = false;
                st->files_binary++;
                return;
            case Written::Overflow:
                if (!write_stub(c, key, size, "budget")) { /* no room even for that */ }
                st->files_dropped++;
                st->complete = false;
                return;
        }
        return;
    }

    // File streamer: frame the entry, then stream the content in slices so
    // packing needs no multi-KB RAM. The escaper is incremental, and a
    // codepoint split across slices only ever holds back up to 3 bytes.
    FILE *f = fopen(c.path, "rb");
    if (!f) {
        if (!write_stub(c, key, size, "unreadable")) st->complete = false;
        st->files_unreadable++;
        return;
    }
    size_t mark = w_mark(c.out);
    bool ok = true;
    if (!c.first) ok = w_putc(c.out, ',');
    ok = ok && w_putc(c.out, '"');
    ok = ok && json_escape_stream(c.out, (const uint8_t *)key, strlen(key));
    ok = ok && w_puts(c.out, "\":{\"size\":");
    ok = ok && w_printf(c.out, "%ld", size);
    ok = ok && w_puts(c.out, ",\"text\":\"");

    uint8_t buf[1024];
    size_t held = 0;   // unescaped tail carried at buf[0..held)
    size_t remaining = want;
    bool binary = false;
    bool first_slice = true;
    while (ok && !binary && remaining > 0) {
        size_t ask = sizeof(buf) - held;
        if (ask > remaining) ask = remaining;
        size_t n = fread(buf + held, 1, ask, f);
        // A short read means the file shrank (or errored): end the content
        // here rather than spinning on EOF.
        bool done = (n < ask) || (n == remaining);
        remaining -= n;
        size_t total = held + n;
        held = 0;
        size_t feed = total;
        // Only a tail of at most one codepoint may be held back; a bigger
        // gap is genuine garbage, not an unlucky cut.
        size_t vp = utf8_valid_prefix(buf, total);
        if (total - vp <= kMaxCodepointBackoff) feed = vp;
        else if (!done || trunc) { binary = true; break; }
        // else: done && !trunc with a ragged tail can only happen on a file
        // that shrank mid-walk; feed it whole and let the escaper decide.
        if (first_slice) {
            first_slice = false;
            size_t sniff = feed < kSniffBytes ? feed : kSniffBytes;
            if (looks_binary(buf, sniff)) { binary = true; break; }
        }
        ok = ok && json_escape_stream(c.out, buf, feed);
        memmove(buf, buf + feed, total - feed);
        held = total - feed;
    }
    fclose(f);

    if (binary) {
        w_rewind(c.out, mark);
        if (!write_stub(c, key, size, "binary")) st->complete = false;
        st->files_binary++;
        return;
    }
    if (!ok) {
        bool was_overflow = c.out.overflow;
        w_rewind(c.out, mark);
        finish_failed_entry(c, key, size, was_overflow);
        return;
    }
    ok = w_putc(c.out, '"');
    if (ok && trunc) ok = w_puts(c.out, ",\"trunc\":true");
    ok = ok && w_putc(c.out, '}');
    if (!ok) {
        bool was_overflow = c.out.overflow;
        w_rewind(c.out, mark);
        finish_failed_entry(c, key, size, was_overflow);
        return;
    }
    c.first = false;
    st->files_text++;
    if (trunc) st->truncated++;
}

// Iterative on purpose: this runs on the LVGL timer stack, where a recursive
// walk carrying a 192-byte path per frame is stack we don't control.
template <typename W>
void walk_subtree(CtxT<W> &c, size_t dir_len)
{
    struct Level { DIR *d; size_t len; };
    Level stack[kFsSnapshotMaxDepth];

    c.path[dir_len] = '\0';
    DIR *d = opendir(c.path);
    if (!d) { c.st->paths_skipped++; c.st->complete = false; return; }

    int sp = 0;
    stack[0].d = d;
    stack[0].len = dir_len;

    while (sp >= 0) {
        struct dirent *de = readdir(stack[sp].d);
        if (!de) {
            closedir(stack[sp].d);
            sp--;
            continue;
        }
        if (is_dot_entry(de->d_name)) continue;

        size_t base = stack[sp].len;
        size_t nlen = strlen(de->d_name);
        if (base + 1 + nlen >= kFsSnapshotMaxPath) {
            c.st->paths_skipped++;
            c.st->complete = false;
            continue;
        }
        c.path[base] = '/';
        memcpy(c.path + base + 1, de->d_name, nlen + 1);
        size_t full = base + 1 + nlen;

        // stat regardless of d_type: the size is needed anyway, and LittleFS
        // reports DT_UNKNOWN in some builds.
        struct stat stt;
        bool stat_ok = stat(c.path, &stt) == 0;
        bool is_dir = stat_ok ? S_ISDIR(stt.st_mode) : (de->d_type == DT_DIR);

        if (is_dir) {
            if (sp + 1 >= kFsSnapshotMaxDepth) {
                c.st->paths_skipped++;
                c.st->complete = false;
                continue;
            }
            DIR *cd = opendir(c.path);
            if (!cd) {
                c.st->paths_skipped++;
                c.st->complete = false;
                continue;
            }
            sp++;
            stack[sp].d = cd;
            stack[sp].len = full;
        } else {
            handle_file(c, stat_ok ? (long)stt.st_size : 0, stat_ok);
        }
    }
}

// One root-level name: a file gets emitted, a directory gets walked.
template <typename W>
void visit_root_entry(CtxT<W> &c, const char *name)
{
    size_t nlen = strlen(name);
    if (c.root_len + 1 + nlen >= kFsSnapshotMaxPath) {
        c.st->paths_skipped++;
        c.st->complete = false;
        return;
    }
    c.path[c.root_len] = '/';
    memcpy(c.path + c.root_len + 1, name, nlen + 1);
    size_t full = c.root_len + 1 + nlen;

    struct stat stt;
    if (stat(c.path, &stt) != 0) return;   // absent is normal, not an error

    if (S_ISDIR(stt.st_mode)) walk_subtree(c, full);
    else                      handle_file(c, (long)stt.st_size, true);
}

template <typename W>
void write_envelope_head(CtxT<W> &c)
{
    uint8_t mac[6] = {};
    hal_radio_local_mac(mac);
    const BadgeIdentity *id = hal_identity_get();

    w_puts(c.out, "{\"v\":1,\"kind\":\"fs_mirror\"");
    w_printf(c.out, ",\"mac\":\"%02x%02x%02x%02x%02x%02x\"",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    w_puts(c.out, ",\"badge_id\":\"");
    json_escape_stream(c.out, (const uint8_t *)id->badge_id, strlen(id->badge_id));
    w_puts(c.out, "\"");

    w_printf(c.out, ",\"provisioned\":%s", id->provisioned ? "true" : "false");
    w_printf(c.out, ",\"attendee_id\":%lu", (unsigned long)id->attendee_id);
    w_printf(c.out, ",\"build\":%d", (int)BADGE_BUILD_NUMBER);

    // Boot-relative, not a wall clock: the badge has no RTC and never syncs
    // time. Kept because the deltas between badges are still useful, but the
    // server must stamp receive time itself.
    w_printf(c.out, ",\"unix_advisory\":%ld", (long)time(nullptr));
    w_printf(c.out, ",\"uptime_ms\":%llu",
             (unsigned long long)(esp_timer_get_time() / 1000));

    size_t used = 0, total = 0;
    if (hal_fs_usage(&used, &total)) {
        w_printf(c.out, ",\"fs\":{\"used\":%u,\"total\":%u}",
                 (unsigned)used, (unsigned)total);
    }
    w_puts(c.out, ",\"files\":{");
}

template <typename W>
void write_envelope_tail(CtxT<W> &c)
{
    const FsSnapshotStats *st = c.st;
    w_release_reserve(c.out);
    w_puts(c.out, "}");
    w_printf(c.out,
             ",\"counts\":{\"seen\":%lu,\"text\":%lu,\"binary\":%lu,"
             "\"too_big\":%lu,\"dropped\":%lu,\"unreadable\":%lu,"
             "\"truncated\":%lu,\"path_skipped\":%lu}",
             (unsigned long)st->files_seen, (unsigned long)st->files_text,
             (unsigned long)st->files_binary, (unsigned long)st->files_too_big,
             (unsigned long)st->files_dropped, (unsigned long)st->files_unreadable,
             (unsigned long)st->truncated, (unsigned long)st->paths_skipped);
    w_printf(c.out, ",\"complete\":%s}", st->complete ? "true" : "false");
}

}  // namespace

void fs_snapshot_heap_info(size_t *largest_out, size_t *free_out)
{
    if (largest_out) {
        *largest_out = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    }
    if (free_out) {
        *free_out = heap_caps_get_free_size(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    }
}

size_t fs_snapshot_pick_cap(size_t want)
{
    // Room the caller needs to keep working once both allocations are live:
    // LVGL still has to render the progress screen for the whole transfer.
    constexpr size_t kWorkingSlack = 8192;
    constexpr size_t kStep = 2048;

    size_t largest = 0, total_free = 0;
    fs_snapshot_heap_info(&largest, &total_free);

    if (want < kFsSnapshotMinCap) want = kFsSnapshotMinCap;

    for (size_t cap = want; ; cap -= kStep) {
        // The document is one allocation and must fit one contiguous run.
        // The +1 is jout_init()'s reserved NUL.
        bool fits_contiguous = largest >= cap + 1;
        // The scratch chunk and the working slack only need to exist, not to
        // be adjacent to the document.
        bool fits_total = total_free >= cap + kFsSnapshotMaxFile + kWorkingSlack;
        if (fits_contiguous && fits_total) return cap;

        if (cap < kFsSnapshotMinCap + kStep) break;
    }
    return 0;
}

char *fs_snapshot_build(const char *root, size_t cap, FsSnapshotStats *stats)
{
    int64_t t0 = esp_timer_get_time();

    FsSnapshotStats local = {};
    local.complete = true;

    if (!root || cap < 1024) return nullptr;
    size_t root_len = strlen(root);
    if (root_len + 2 >= kFsSnapshotMaxPath) return nullptr;

    char *buf = (char *)heap_caps_malloc(cap, MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    if (!buf) return nullptr;
    uint8_t *chunk = (uint8_t *)malloc(kFsSnapshotMaxFile);
    if (!chunk) { free(buf); return nullptr; }

    CtxT<JsonOut> c;
    c.st        = &local;
    c.chunk     = chunk;
    c.skip_path = nullptr;
    c.first     = true;
    c.root_len  = root_len;
    memcpy(c.path, root, root_len + 1);
    jout_init(c.out, buf, cap, kTailReserve);

    write_envelope_head(c);

    for (const char *name : kWalkOrder) {
        visit_root_entry(c, name);
    }

    // Then anything else at the root, so a file added later is still captured.
    c.path[root_len] = '\0';
    if (DIR *d = opendir(c.path)) {
        struct dirent *de;
        while ((de = readdir(d)) != nullptr) {
            if (is_dot_entry(de->d_name)) continue;
            if (in_walk_order(de->d_name)) continue;
            visit_root_entry(c, de->d_name);
        }
        closedir(d);
    }

    write_envelope_tail(c);

    free(chunk);

    local.bytes = c.out.len;
    // Seed 0 is the zlib-compatible convention; see the crc32 field in
    // bbx_proto.h. The station and the server must use the same one.
    local.crc32 = esp_rom_crc32_le(0, (const uint8_t *)buf, local.bytes);
    if (c.out.overflow) local.complete = false;
    local.elapsed_ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);

    if (stats) *stats = local;
    return (char *)jout_cstr(c.out);
}

void fs_snapshot_free(char *buf)
{
    free(buf);
}

size_t fs_snapshot_write_file(const char *tmp_path, size_t header_len,
                              size_t cap, FsSnapshotStats *stats)
{
    int64_t t0 = esp_timer_get_time();

    FsSnapshotStats local = {};
    local.complete = true;

    if (!tmp_path || cap < 1024) return 0;

    // Drop any stale staging file first: it is skipped by the walk below, but
    // there is no reason to carry a dead upload around on flash.
    unlink(tmp_path);
    FILE *f = fopen(tmp_path, "wb");
    if (!f) return 0;

    // Reserve the header room with real bytes rather than a seek past EOF:
    // the transport header is small and this keeps no sparse-file assumptions.
    uint8_t zeros[64] = {};
    size_t left = header_len;
    while (left > 0) {
        size_t n = left < sizeof(zeros) ? left : sizeof(zeros);
        if (fwrite(zeros, 1, n, f) != n) {
            fclose(f);
            unlink(tmp_path);
            return 0;
        }
        left -= n;
    }

    // Staging buffer lives on this task's stack (1 KB); the walk otherwise
    // touches flash a slice at a time and keeps nothing but a FILE* in RAM.
    uint8_t stage[1024];
    CtxT<FileOut> c;
    c.st        = &local;
    c.chunk     = nullptr;
    c.skip_path = tmp_path;
    c.first     = true;
    c.root_len  = strlen("/littlefs");
    memcpy(c.path, "/littlefs", c.root_len + 1);
    fout_init(c.out, f, stage, sizeof(stage), cap, kTailReserve, header_len);
    if (c.out.io_error) {
        fclose(f);
        unlink(tmp_path);
        return 0;
    }

    write_envelope_head(c);

    for (const char *name : kWalkOrder) {
        visit_root_entry(c, name);
    }

    // Then anything else at the root, so a file added later is still captured.
    c.path[c.root_len] = '\0';
    if (DIR *d = opendir(c.path)) {
        struct dirent *de;
        while ((de = readdir(d)) != nullptr) {
            if (is_dot_entry(de->d_name)) continue;
            if (in_walk_order(de->d_name)) continue;
            visit_root_entry(c, de->d_name);
        }
        closedir(d);
    }

    write_envelope_tail(c);

    bool flushed = fout_flush(c.out);
    size_t json_bytes = fout_len(c.out);
    bool io_err = c.out.io_error;
    fclose(f);

    if (!flushed || io_err || json_bytes == 0) {
        unlink(tmp_path);
        return 0;
    }

    local.bytes = json_bytes;
    // CRC covers the whole transfer including the header, which the caller
    // writes after this returns — so crc32 is filled in by the caller.
    if (c.out.overflow) local.complete = false;
    local.elapsed_ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);

    if (stats) *stats = local;
    return json_bytes;
}
