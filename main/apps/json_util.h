#pragma once

// A bounded JSON writer plus a correct string escaper.
//
// Exists because the badge builds JSON by hand in several places and the
// obvious snprintf-into-a-stack-buffer approach has two failure modes that
// only show up server-side: a truncated document, and a string that is not
// valid JSON because a control character or a malformed UTF-8 sequence went
// through unescaped.
//
// The writer never overflows its buffer. On a would-be overflow it sets a
// sticky flag and drops the append, so a caller can write a whole document
// optimistically and check once at the end.

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

// Appends stop `reserve` bytes short of the end, so a caller streaming a large
// body can always close the document. Call jout_release_reserve() before
// writing that tail.
struct JsonOut {
    char  *base;
    size_t cap;
    size_t len;
    size_t reserve;
    bool   overflow;   // sticky: set on the first dropped append
};

void jout_init(JsonOut &o, char *buf, size_t cap, size_t reserve);

// Bytes that may still be appended before the reserve.
size_t jout_room(const JsonOut &o);

// Frees the tail reserve for the closing fields. One-way.
void jout_release_reserve(JsonOut &o);

// Rewind is what makes "start writing an entry, discover it doesn't fit or
// isn't valid UTF-8, and replace it with a stub" possible without buffering
// the entry separately. It clears the overflow flag: the caller is explicitly
// undoing whatever overflowed.
size_t jout_mark(const JsonOut &o);
void   jout_rewind(JsonOut &o, size_t mark);

// All return false on overflow, having appended nothing.
bool jout_putc(JsonOut &o, char c);
bool jout_puts(JsonOut &o, const char *s);
bool jout_putn(JsonOut &o, const char *s, size_t n);
bool jout_printf(JsonOut &o, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

// NUL-terminate in place without counting the NUL as content. The buffer must
// have one spare byte, which jout_init() guarantees by refusing a cap of 0.
const char *jout_cstr(JsonOut &o);

// Appends `n` bytes of `s` as the *body* of a JSON string — the caller writes
// the surrounding quotes. Escapes per RFC 8259: `"` and `\`, the five short
// forms, and every other C0 control plus DEL as \u00XX.
//
// Bytes >= 0x80 are validated as UTF-8 and passed through intact. Returns
// false on overflow OR on a malformed sequence; on false the caller should
// jout_rewind() to a mark taken before the entry started. Rejecting rather
// than substituting is deliberate — a badge file that is not text is a file
// the server should be told about explicitly, not handed as mojibake.
bool json_escape_stream(JsonOut &o, const uint8_t *s, size_t n);

// Length of the longest prefix of `s` that is well-formed UTF-8. Rejects
// overlong encodings, surrogates and anything above U+10FFFF, so a truncated
// multi-byte sequence at the end simply isn't included.
//
// This is how content gets cut at a codepoint boundary: slicing at an
// arbitrary byte would emit an invalid sequence and break the whole document.
size_t utf8_valid_prefix(const uint8_t *s, size_t n);

// File-backed bounded writer with the same semantics as JsonOut: appends
// stop `reserve` bytes short of `cap`, overflow is sticky, and mark/rewind
// can undo a partially written entry (via ftruncate, which the LittleFS VFS
// supports). Backed by a small RAM staging buffer, so documents far larger
// than the heap can be streamed to flash a few hundred bytes at a time.
struct FileOut {
    FILE   *f;
    size_t  base_off;    // absolute file offset the logical document starts at
    size_t  file_len;    // logical bytes flushed to the file so far
    uint8_t *stage;
    size_t  stage_cap;
    size_t  stage_len;
    size_t  cap;
    size_t  len;         // logical length = file_len + stage_len
    size_t  reserve;
    bool    overflow;    // sticky: set on the first dropped append
    bool    io_error;    // sticky: fwrite/ftruncate/fseek failed
};

void fout_init(FileOut &o, FILE *f, uint8_t *stage, size_t stage_cap,
               size_t cap, size_t reserve, size_t base_off);
size_t fout_room(const FileOut &o);
void fout_release_reserve(FileOut &o);
size_t fout_mark(const FileOut &o);
// Returns false (setting io_error) if the file could not be truncated back.
bool fout_rewind(FileOut &o, size_t mark);
bool fout_putc(FileOut &o, char c);
bool fout_puts(FileOut &o, const char *s);
bool fout_putn(FileOut &o, const char *s, size_t n);
bool fout_printf(FileOut &o, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
// Pushes staged bytes to the file. Returns false on IO error.
bool fout_flush(FileOut &o);
size_t fout_len(const FileOut &o);
bool json_escape_stream(FileOut &o, const uint8_t *s, size_t n);

// NUL-terminated convenience form for fixed-size stack buffers. Always
// produces valid JSON: it never emits a partial escape, and it drops
// malformed UTF-8 rather than failing (callers of this form have no error
// path). Returns the number of bytes written, excluding the NUL.
//
// Note a control character expands to six bytes, so size `out` generously if
// the input is untrusted; the result is truncated, never overflowed.
size_t json_escape(char *out, size_t out_sz, const char *s);
