#include "json_util.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

void jout_init(JsonOut &o, char *buf, size_t cap, size_t reserve)
{
    o.base     = buf;
    o.cap      = cap ? cap - 1 : 0;   // keep one byte for jout_cstr()'s NUL
    o.len      = 0;
    o.reserve  = reserve;
    o.overflow = false;
    if (o.reserve > o.cap) o.reserve = o.cap;
    if (o.base && cap) o.base[0] = '\0';
}

size_t jout_room(const JsonOut &o)
{
    size_t limit = o.cap - o.reserve;
    return o.len >= limit ? 0 : limit - o.len;
}

void jout_release_reserve(JsonOut &o)
{
    o.reserve = 0;
}

size_t jout_mark(const JsonOut &o)
{
    return o.len;
}

void jout_rewind(JsonOut &o, size_t mark)
{
    if (mark <= o.len) o.len = mark;
    o.overflow = false;
}

bool jout_putn(JsonOut &o, const char *s, size_t n)
{
    if (!o.base) { o.overflow = true; return false; }
    if (n > jout_room(o)) { o.overflow = true; return false; }
    memcpy(o.base + o.len, s, n);
    o.len += n;
    return true;
}

bool jout_putc(JsonOut &o, char c)
{
    return jout_putn(o, &c, 1);
}

bool jout_puts(JsonOut &o, const char *s)
{
    return s ? jout_putn(o, s, strlen(s)) : true;
}

bool jout_printf(JsonOut &o, const char *fmt, ...)
{
    // Formats into the buffer directly, then accepts or rejects the result —
    // vsnprintf always NUL-terminates within the size it is given, and we only
    // ever hand it the room that is actually free, so a too-long format is
    // truncated harmlessly and then dropped whole.
    size_t room = jout_room(o);
    if (!o.base || room == 0) { o.overflow = true; return false; }

    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(o.base + o.len, room + 1, fmt, ap);
    va_end(ap);

    if (n < 0 || (size_t)n > room) { o.overflow = true; return false; }
    o.len += (size_t)n;
    return true;
}

const char *jout_cstr(JsonOut &o)
{
    if (!o.base) return "";
    o.base[o.len] = '\0';
    return o.base;
}

// ---------------------------------------------------------------------------
// UTF-8
// ---------------------------------------------------------------------------

// Length of the well-formed sequence starting at s[0], or 0 if it isn't one.
// This is the Unicode "well-formed byte sequences" table, not a naive
// count-the-leading-ones: the ranges below are what reject overlong forms,
// surrogates (U+D800..DFFF) and anything past U+10FFFF.
static size_t utf8_seq_len(const uint8_t *s, size_t n)
{
    if (n == 0) return 0;
    uint8_t b0 = s[0];

    if (b0 <= 0x7F) return 1;

    uint8_t lo, hi;
    size_t need;
    if (b0 >= 0xC2 && b0 <= 0xDF)      { need = 2; lo = 0x80; hi = 0xBF; }
    else if (b0 == 0xE0)               { need = 3; lo = 0xA0; hi = 0xBF; }
    else if (b0 >= 0xE1 && b0 <= 0xEC) { need = 3; lo = 0x80; hi = 0xBF; }
    else if (b0 == 0xED)               { need = 3; lo = 0x80; hi = 0x9F; }
    else if (b0 >= 0xEE && b0 <= 0xEF) { need = 3; lo = 0x80; hi = 0xBF; }
    else if (b0 == 0xF0)               { need = 4; lo = 0x90; hi = 0xBF; }
    else if (b0 >= 0xF1 && b0 <= 0xF3) { need = 4; lo = 0x80; hi = 0xBF; }
    else if (b0 == 0xF4)               { need = 4; lo = 0x80; hi = 0x8F; }
    else return 0;   // 0x80..0xC1 (stray continuation / overlong), 0xF5..0xFF

    if (n < need) return 0;
    if (s[1] < lo || s[1] > hi) return 0;
    for (size_t i = 2; i < need; i++) {
        if (s[i] < 0x80 || s[i] > 0xBF) return 0;
    }
    return need;
}

size_t utf8_valid_prefix(const uint8_t *s, size_t n)
{
    size_t i = 0;
    while (i < n) {
        size_t len = utf8_seq_len(s + i, n - i);
        if (len == 0) break;
        i += len;
    }
    return i;
}

// ---------------------------------------------------------------------------
// Escaping
// ---------------------------------------------------------------------------

// Renders one byte < 0x80 into `out` (max 6 bytes) and returns the length.
static size_t escape_ascii(uint8_t c, char *out)
{
    switch (c) {
        case '"':  out[0] = '\\'; out[1] = '"';  return 2;
        case '\\': out[0] = '\\'; out[1] = '\\'; return 2;
        case '\b': out[0] = '\\'; out[1] = 'b';  return 2;
        case '\f': out[0] = '\\'; out[1] = 'f';  return 2;
        case '\n': out[0] = '\\'; out[1] = 'n';  return 2;
        case '\r': out[0] = '\\'; out[1] = 'r';  return 2;
        case '\t': out[0] = '\\'; out[1] = 't';  return 2;
        default: break;
    }
    // Everything else below 0x20, plus DEL. `/` is deliberately not escaped:
    // RFC 8259 permits it bare and escaping it only makes payloads larger.
    if (c < 0x20 || c == 0x7F) {
        static const char kHex[] = "0123456789abcdef";
        out[0] = '\\'; out[1] = 'u'; out[2] = '0'; out[3] = '0';
        out[4] = kHex[(c >> 4) & 0xF];
        out[5] = kHex[c & 0xF];
        return 6;
    }
    out[0] = (char)c;
    return 1;
}

template <typename W>
static bool escape_into(bool (*putn)(W &, const char *, size_t),
                        W &w, const uint8_t *s, size_t n)
{
    size_t i = 0;
    while (i < n) {
        uint8_t c = s[i];
        if (c < 0x80) {
            char esc[6];
            size_t len = escape_ascii(c, esc);
            if (!putn(w, esc, len)) return false;
            i++;
            continue;
        }
        size_t seq = utf8_seq_len(s + i, n - i);
        if (seq == 0) return false;   // malformed: caller rewinds and stubs it
        if (!putn(w, (const char *)(s + i), seq)) return false;
        i += seq;
    }
    return true;
}

bool json_escape_stream(JsonOut &o, const uint8_t *s, size_t n)
{
    return escape_into<JsonOut>(jout_putn, o, s, n);
}

bool json_escape_stream(FileOut &o, const uint8_t *s, size_t n)
{
    return escape_into<FileOut>(fout_putn, o, s, n);
}

size_t json_escape(char *out, size_t out_sz, const char *s)
{
    if (!out || out_sz == 0) return 0;
    if (!s) { out[0] = '\0'; return 0; }

    const uint8_t *p = (const uint8_t *)s;
    size_t n = strlen(s);
    size_t w = 0;
    size_t i = 0;

    while (i < n) {
        uint8_t c = p[i];
        if (c < 0x80) {
            char esc[6];
            size_t len = escape_ascii(c, esc);
            // Escapes are emitted whole or not at all — a truncated \u00 or a
            // trailing lone backslash would break the enclosing document.
            if (w + len >= out_sz) break;
            memcpy(out + w, esc, len);
            w += len;
            i++;
            continue;
        }
        size_t seq = utf8_seq_len(p + i, n - i);
        if (seq == 0) { i++; continue; }   // drop: this form has no error path
        if (w + seq >= out_sz) break;
        memcpy(out + w, p + i, seq);
        w += seq;
        i += seq;
    }

    out[w] = '\0';
    return w;
}

// ---------------------------------------------------------------------------
// FileOut
// ---------------------------------------------------------------------------

void fout_init(FileOut &o, FILE *f, uint8_t *stage, size_t stage_cap,
               size_t cap, size_t reserve, size_t base_off)
{
    o.f         = f;
    o.base_off  = base_off;
    o.file_len  = 0;
    o.stage     = stage;
    o.stage_cap = (stage && stage_cap) ? stage_cap : 0;
    o.stage_len = 0;
    o.cap       = cap;
    o.len       = 0;
    o.reserve   = reserve;
    o.overflow  = false;
    o.io_error  = (f == nullptr) || (stage == nullptr);
    if (o.reserve > o.cap) o.reserve = o.cap;
}

size_t fout_room(const FileOut &o)
{
    size_t limit = o.cap - o.reserve;
    return o.len >= limit ? 0 : limit - o.len;
}

void fout_release_reserve(FileOut &o)
{
    o.reserve = 0;
}

size_t fout_mark(const FileOut &o)
{
    return o.len;
}

bool fout_flush(FileOut &o)
{
    if (o.stage_len == 0) return !o.io_error;
    if (o.io_error || !o.f) { o.io_error = true; return false; }
    size_t w = fwrite(o.stage, 1, o.stage_len, o.f);
    o.file_len += w;
    if (w != o.stage_len) {
        o.io_error = true;
        o.stage_len = 0;
        return false;
    }
    o.stage_len = 0;
    return true;
}

bool fout_rewind(FileOut &o, size_t mark)
{
    if (mark > o.len) return false;
    if (mark >= o.file_len) {
        // The mark is inside (or at the edge of) the staging buffer: just
        // drop staged bytes. The file itself is untouched.
        o.stage_len = mark - o.file_len;
    } else {
        o.stage_len = 0;
        if (!o.f) { o.io_error = true; return false; }
        // Flush stdio's buffer before moving the end of the file: ftruncate()
        // does not discard bytes stdio accepted but hasn't written yet, and
        // the fseek() below would push those stale bytes back out past the new
        // end. The physical file then stays longer than the logical length
        // with shifted content — the badge checksums to EOF while a transfer
        // only covers the logical prefix, which fails as a CRC error on any
        // snapshot large enough to rewind into the file region.
        if (fflush(o.f) != 0) {
            o.io_error = true;
            return false;
        }
        if (ftruncate(fileno(o.f), (off_t)(o.base_off + mark)) != 0) {
            o.io_error = true;
            return false;
        }
        if (fseek(o.f, 0, SEEK_END) != 0) {
            o.io_error = true;
            return false;
        }
        o.file_len = mark;
    }
    o.len = mark;
    o.overflow = false;
    return true;
}

bool fout_putn(FileOut &o, const char *s, size_t n)
{
    if (o.io_error || !o.stage) { o.io_error = true; return false; }
    // Whole or not at all: entries that don't fit degrade to stubs.
    if (n > fout_room(o)) { o.overflow = true; return false; }
    size_t stage_free = o.stage_cap - o.stage_len;
    if (n <= stage_free) {
        memcpy(o.stage + o.stage_len, s, n);
        o.stage_len += n;
        o.len += n;
        return true;
    }
    if (!fout_flush(o)) { o.overflow = true; return false; }
    if (n <= o.stage_cap) {
        memcpy(o.stage, s, n);
        o.stage_len = n;
        o.len += n;
        return true;
    }
    // Larger than the whole staging buffer: straight to the file.
    size_t w = fwrite(s, 1, n, o.f);
    o.file_len += w;
    o.len += w;
    if (w != n) { o.io_error = true; o.overflow = true; return false; }
    return true;
}

bool fout_putc(FileOut &o, char c)
{
    return fout_putn(o, &c, 1);
}

bool fout_puts(FileOut &o, const char *s)
{
    return s ? fout_putn(o, s, strlen(s)) : true;
}

bool fout_printf(FileOut &o, const char *fmt, ...)
{
    // All existing formats are short identifiers and counters; 192 bytes is
    // far more than any of them needs, and an over-long format fails safe
    // into the stub path rather than truncating the document.
    char tmp[192];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= sizeof(tmp)) { o.overflow = true; return false; }
    return fout_putn(o, tmp, (size_t)n);
}

size_t fout_len(const FileOut &o)
{
    return o.len;
}
