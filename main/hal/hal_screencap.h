#pragma once

#include <cstdint>
#include <cstddef>

#ifdef __cplusplus
extern "C" {
#endif

struct ScreenCapStats {
    uint16_t w, h;
    uint32_t stripes;         // flush callbacks seen
    uint32_t pixels_covered;  // summed stripe areas; == w*h on a good capture
    uint32_t raw_bytes;       // decoded RGB565 payload size
    uint32_t wire_bytes;      // base64 characters emitted (excludes newlines)
    uint32_t crc32;           // esp_rom_crc32_le(0, raw) over the RGB565 stream
    uint32_t elapsed_ms;
    bool complete;            // pixels_covered == w*h
};

// Stream the live screen to stdout as RLE+base64 RGB565. See the wire format
// in tools/badge/README.md.
//
// Takes the LVGL lock itself, so it must NOT be called with the lock already
// held. Blocks the UI for the duration (a few hundred ms) because the whole
// encode runs inside a forced synchronous refresh.
//
// Returns false only if the display isn't up or the lock can't be taken; a
// partial capture still returns true with complete=false so the caller can
// report the numbers.
bool hal_screencap_stream(ScreenCapStats *out);

#ifdef __cplusplus
}
#endif
