#pragma once

#include <stdbool.h>
#include <stdint.h>

// The emulator is single-threaded: every hook already runs on the one thread
// that owns LVGL, so the port lock always succeeds.
static inline bool lvgl_port_lock(uint32_t timeout_ms)
{
    (void)timeout_ms;
    return true;
}

static inline void lvgl_port_unlock(void) {}
