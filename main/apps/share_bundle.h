#pragma once

// App bundle pack/unpack for badge-to-badge sharing.
//
// A bundle is one app directory serialised as:
//
//   "APPB" u8 version(1) u8 file_count
//   per file: u8 path_len | path | u32 data_len (LE) | data
//
// Paths are relative ("manifest.cfg", "main.lua", "pkg/mod.lua"). The
// receiver re-validates every cap on unpack — the sender is another badge,
// not a trusted source.
//
// Caps: total bundle <= 48 KiB on disk, <= 16 files, each file <= 48 KiB,
// path <= 64 bytes, depth
// <= 4. manifest.cfg + main.lua are required, matching what ScriptApp::load
// demands.
//
// Deliberately free of LVGL and ESP-IDF (UTF-8/ASCII C only) so the host
// test in tools/share-test/ verifies it with plain g++.

#include <stddef.h>
#include <stdint.h>

#define SHARE_BUNDLE_MAGIC_0 'A'
#define SHARE_BUNDLE_MAGIC_1 'P'
#define SHARE_BUNDLE_MAGIC_2 'P'
#define SHARE_BUNDLE_MAGIC_3 'B'
#define SHARE_BUNDLE_VERSION 1

#define SHARE_BUNDLE_MAX_LEN   (48 * 1024)
#define SHARE_BUNDLE_MAX_FILES 16
#define SHARE_BUNDLE_MAX_FILE  (48 * 1024)
#define SHARE_BUNDLE_MAX_PATH  64
#define SHARE_BUNDLE_MAX_DEPTH 4
#define SHARE_STREAM_CHUNK_LEN 1024

#ifdef __cplusplus
extern "C" {
#endif

// zlib CRC32 (poly 0xEDB88320, init/XOR 0xFFFFFFFF). Same convention as
// esp_rom_crc32_le(0, ...) used by BBX, without the IDF dependency.
uint32_t share_bundle_crc32(const uint8_t *data, size_t len);
// Continue a CRC from a previous block; pass 0 for the first block.
uint32_t share_bundle_crc32_update(uint32_t seed, const uint8_t *data, size_t len);

// Firmware path: stage/read the serialized bundle on disk, using at most a
// 1 KiB data-copy buffer. No allocation grows with the app size. Partial
// output is removed on failure. bundle_path must be outside app_dir.
int share_bundle_pack_file(const char *app_dir, const char *bundle_path,
                           size_t *out_len, uint32_t *out_crc,
                           char *why, size_t why_sz);
int share_bundle_unpack_file(const char *bundle_path, const char *staging_dir,
                             char *why, size_t why_sz);

// Slug charset, kept identical to ScriptApp's valid_slug (script_app.cpp):
// [a-z0-9][a-z0-9_-]{0,31}.
int share_bundle_valid_slug(const char *slug);

// Pack /littlefs/apps/<slug>/ (or any app dir) into a malloc'd bundle.
// Skips dotfiles and the app's appdata/ save data — saves are personal, not
// part of the app. Returns null on error with `why` set; free with free().
uint8_t *share_bundle_pack(const char *app_dir, size_t *out_len,
                           char *why, size_t why_sz);

// Unpack a bundle into staging_dir (created, must not exist). Verifies magic,
// version, caps, path safety (no absolute paths, no ".." segments, no
// backslashes) and that manifest.cfg + main.lua are present. Does NOT rename
// into place and does NOT validate manifest semantics — the caller renames
// staging to /littlefs/apps/<slug>/ and lets ScriptApp::load be the single
// semantic validator. Returns false on error with `why` set.
int share_bundle_unpack(const uint8_t *data, size_t len,
                        const char *staging_dir,
                        char *why, size_t why_sz);

// Extract the slug= value from a manifest.cfg (syntax-tolerant: first slug=
// line wins). Used to pick the final dir name before ScriptApp::load runs
// its strict validation. Returns false when no usable slug is found.
int share_bundle_slug_from_manifest(const char *manifest_path,
                                    char *slug_out, size_t slug_sz);

// Rewrite the slug= line of a staged manifest (keep-both installs as
// <slug>-2). Returns false if no slug= line exists.
int share_bundle_patch_slug(const char *manifest_path, const char *new_slug);

// Recursive delete for staging dirs and overwrite installs.
void share_bundle_remove_dir(const char *path);

// Total on-disk size of an app dir (regular files only, same skip rules as
// pack). For the send-list display.
size_t share_bundle_dir_size(const char *app_dir);

#ifdef __cplusplus
}
#endif
