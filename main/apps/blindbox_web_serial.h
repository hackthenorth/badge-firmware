#pragma once

#include <cstddef>
#include <cstdint>

// Maximum JSON response accepted from the Web Serial bridge. The browser
// writes this body in small chunks because the USB-Serial-JTAG driver has a
// 256-byte RX ring buffer.
constexpr size_t BLINDBOX_WEB_MAX_RESPONSE = 512;

// Called from the console task after it has read a complete response body.
// The Solana Scavenger Hunt app consumes the body from its LVGL tick, so this function
// must never touch the UI directly.
bool blindbox_web_serial_submit(uint16_t request_id, const char *json, size_t len);
