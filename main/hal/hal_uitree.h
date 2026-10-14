#pragma once

#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif

struct UiTreeStats {
    uint32_t nodes;
    uint32_t max_depth;
    bool truncated;   // hit the node or depth cap; the dump is partial
};

// Print the live LVGL widget tree to stdout: class, absolute coords, state,
// and label text. Covers the active screen and the top layer.
//
// This is the cheap counterpart to hal_screencap_stream() — it answers "what
// does that label say and is that button focused?" in a few dozen lines rather
// than a 150 KB image, and unlike a screenshot it survives being diffed.
//
// Takes the LVGL lock itself; must NOT be called with the lock already held.
bool hal_uitree_dump(UiTreeStats *out);

#ifdef __cplusplus
}
#endif
