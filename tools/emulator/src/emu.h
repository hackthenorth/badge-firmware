#pragma once

// Shared state between the emulator's HAL pieces. The page (shell.html) is the
// "hardware": it calls the exported emu_* entry points and reads back the
// framebuffer, LED and status blocks.

#include <cstddef>
#include <cstdint>

// Virtual clock. Only emu_step() and vTaskDelay() move it, so esp_timer and
// lv_tick always agree and a firmware timeout can never outrun the UI.
int64_t emu_now_us();
void emu_advance_us(int64_t delta_us);

// Light sleep cannot block the browser thread, so esp_light_sleep_start()
// arms this and the main loop stops running LVGL until a wake source fires.
bool emu_sleep_frozen();
void emu_sleep_poll();

// START is the firmware's only wake GPIO.
bool emu_start_wake_pending();

// Radio: called once per main-loop slice to emit due advertisements.
void emu_radio_poll();

// NFC and accelerometer share the I2C bus on a badge; NFC suspends the
// accelerometer cache while its field is enabled.
bool emu_nfc_running();

// Status block the page polls each frame (see emu_status_ptr).
struct EmuStatus {
    uint8_t panel_on;        // LCD powered (hal_display_show reached)
    uint8_t nfc_state;       // 0 off, 1 enabled / field idle, 2 field energised
    uint8_t radio_state;     // 0 off, 1 scanning only, 2 advertising + scanning
    uint8_t sleep_tier;      // SleepTier
    uint8_t leds[6][3];      // physical WS2812 levels, firmware LED order
    uint8_t led_revision;    // bumps on every led_strip_refresh
    uint8_t fb_revision;     // bumps on every display flush
    uint8_t reserved[2];
    uint32_t adv_interval_ms;
    uint32_t scan_duty_pct;
    uint32_t tx_count;
    uint32_t rx_count;
};
EmuStatus &emu_status();

// Thrown through the wasm stack to end the running instance (esp_restart,
// abort). The page catches it and boots a fresh instance.
[[noreturn]] void emu_halt(int reset_reason, const char *message);
