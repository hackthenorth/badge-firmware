#pragma once

// Serialises the badge's LittleFS into one JSON document, for upload to the
// server as a restorable backup (docs/BADGE-EVENT-EXPERIENCE.md §11).
//
// Text files go in with their contents. Binaries — icon.bin and anything else
// that isn't valid UTF-8 — go in as stubs carrying path, size and a reason.
// Stubs rather than omission is deliberate: the server diffing two syncs has
// to be able to tell "the badge deleted this file" from "the badge chose not
// to send it".
//
// The document is always well-formed, even when the walk hits its budget. On
// overflow, later files degrade to stubs and `complete` goes false; the
// manifest of what exists is the most valuable part, so the walk never aborts.

#include <stddef.h>
#include <stdint.h>

// Byte budget for the JSON, NOT a limit on filesystem size.
//
// The wire ceiling is BBX_XFER_MAX_LEN (59,160 = 255 frames x 232 B), so this
// can be raised without touching the protocol. It is well under that on
// purpose: a sweep is ~3.9 KB/s, so 20 KB is already ~5 s of holding still at
// a station.
static constexpr size_t kFsSnapshotCap = 20 * 1024;

// Per-file content cap. Longer files are included up to here with "trunc":true,
// cut at a UTF-8 codepoint boundary.
static constexpr size_t kFsSnapshotMaxFile = 4096;

// Past this a file is stubbed "too_big" rather than truncated — 4 KB of a
// 40 KB log is misleading, not useful.
static constexpr size_t kFsSnapshotBigFile = kFsSnapshotMaxFile * 4;

// Longest absolute path the walk will follow. Deliberately larger than
// hal_fs_mkdir_p's 128 and hal_console resolve_path's 160, so the snapshot
// never silently omits a file those could create.
static constexpr size_t kFsSnapshotMaxPath = 192;

// Directory nesting cap. The deepest real path is
// /littlefs/config/contacts/<id>.cfg, so this is generous.
static constexpr int kFsSnapshotMaxDepth = 6;

struct FsSnapshotStats {
    // Regular files encountered. Invariant:
    //   files_seen == files_text + files_binary + files_too_big
    //               + files_dropped + files_unreadable
    uint32_t files_seen;
    uint32_t files_text;       // included with content
    uint32_t files_binary;     // stubbed: extension denylist, or not valid UTF-8
    uint32_t files_too_big;    // stubbed: over kFsSnapshotBigFile
    uint32_t files_dropped;    // out of budget; stubbed if even that fit
    uint32_t files_unreadable; // stubbed: stat or open failed
    uint32_t truncated;       // included, but cut at kFsSnapshotMaxFile
    uint32_t paths_skipped;   // path too long or past the depth cap (files+dirs)
    size_t   bytes;           // final JSON length, excluding the NUL
    uint32_t crc32;           // esp_rom_crc32_le(0, json, bytes) — seed 0, zlib
    bool     complete;        // false if anything was dropped or skipped
    uint32_t elapsed_ms;
};

// Walks `root` (normally "/littlefs") and returns a NUL-terminated JSON
// document, or nullptr if the buffer could not be allocated. `stats` may be
// null. Free with fs_snapshot_free().
//
// Runs synchronously and does a lot of small LittleFS reads, so it blocks its
// caller for a few hundred milliseconds. Callers on the LVGL tick must put a
// "working" frame on screen before calling.
char *fs_snapshot_build(const char *root, size_t cap, FsSnapshotStats *stats);

void fs_snapshot_free(char *buf);

// Smallest snapshot worth taking. Below this the document stops being a useful
// backup — this much still holds identity.json, solana.json and most of
// /littlefs/config, which is the genuinely irreplaceable part.
static constexpr size_t kFsSnapshotMinCap = 6 * 1024;

// Staging file for the Sync app's upload. Flash, not RAM: with the radio up
// the heap holds ~6 KB free, so no RAM buffer scheme can stage a snapshot.
// The walk always skips this path, and the transfer streams frames from it.
static constexpr const char *kFsSnapshotTmpPath = "/littlefs/.sync_snapshot.json";

// Streams the JSON document into `tmp_path` (created, holding `header_len`
// reserved bytes first for the caller's transport header), honouring `cap`
// exactly the way fs_snapshot_build does. Returns the JSON length excluding
// the header, or 0 on IO error (open/write/truncate failure). Budget
// overflows degrade to stubs with complete:false, same as the RAM builder.
size_t fs_snapshot_write_file(const char *tmp_path, size_t header_len,
                              size_t cap, FsSnapshotStats *stats);

// Largest cap that will actually fit in the heap right now, stepping down from
// `want`, or 0 if even kFsSnapshotMinCap won't fit.
//
// Two separate conditions, because there are two separate allocations: the
// document needs one contiguous run of `cap`, and the file scratch chunk plus
// the caller's working room only need to exist *somewhere*. Requiring one
// contiguous block big enough for all of it — as an earlier version did —
// fails on a badge whose heap is merely fragmented, not full, and presents as
// "Not enough memory to sync" with 60 KB free.
//
// A smaller cap is not a failure: the walk degrades to "budget" stubs and sets
// complete:false, so the server still gets the full manifest of what exists
// plus the highest-priority file bodies.
size_t fs_snapshot_pick_cap(size_t want);

// Largest contiguous block and total free, both internal 8-bit capable. For
// diagnostics — an out-of-memory message that doesn't say by how much is not
// actionable.
void fs_snapshot_heap_info(size_t *largest_out, size_t *free_out);
