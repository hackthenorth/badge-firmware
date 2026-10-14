#pragma once

#include "esp_err.h"
#include <cstdint>

// Idle power management.
//
// LIGHT sleep, not deep sleep. Deep-sleep GPIO wake on the ESP32-C3 requires
// an RTC-capable pin (GPIO 0-5) and all six are taken by the LCD, LED and
// I2C — so deep sleep could only ever be woken by a timer, never a button.
// Light sleep has no such restriction: GPIO9 (Start) wakes it directly.
//
// The power argument agrees with the UX one. Deep sleep saves ~200 uA against
// a floor of several mA from the WS2812s' quiescent draw plus an LCD backlight
// this board gives us no way to switch off. It was buying nothing measurable
// while costing the ability to wake the badge with a button.
//
// Because the backlight cannot be turned off and the ST7789 holds its own
// framebuffer through light sleep, we do NOT blank the panel. We draw a
// "press START to wake" card instead: same power, and nobody thinks their
// badge is broken.

enum SleepTier : uint8_t {
    SLEEP_ACTIVE = 0,   // normal operation
    SLEEP_DIM,          // idle a while: LEDs off, slower tick
    SLEEP_LIGHT,        // sleep card up, esp_light_sleep_start()
};

// Wire up the wake GPIO. Call once, after hal_buttons_init().
esp_err_t hal_sleep_init();

// Reset the idle timer. Called from the registry's button dispatcher; any
// app doing its own input handling does not need to call this.
void hal_sleep_note_activity();

// Drive the state machine. Called from the registry after the active app's
// on_tick(); all timeouts use wall-clock time and do not assume a tick rate.
void hal_sleep_tick();

// Enter the normal sleep-card flow on the next manager tick without waiting
// for the idle timeout. Start remains the only hardware wake button.
void hal_sleep_enter_now();

// Suppress sleep while true — see App::wants_wake_lock(). The registry sets
// this from the active app on every launch.
void hal_sleep_wake_lock(bool on);

SleepTier hal_sleep_tier();

// Seconds of inactivity before entering light sleep. Defaults to 300.
void hal_sleep_set_idle_timeout(uint32_t seconds);
