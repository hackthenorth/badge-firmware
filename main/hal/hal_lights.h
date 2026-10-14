#pragma once

#include <cstddef>
#include <cstdint>

constexpr size_t LED_COUNT = 6;

// Physical LED positions when looking at the front of the badge:
//
//        0 UpperLeft          1 UpperRight
//        5 MiddleLeft         2 MiddleRight
//        4 BottomLeft         3 BottomRight
//
// Use these labels instead of raw indices whenever physical order matters.
namespace BadgeLed {
constexpr size_t UpperLeft = 0;
constexpr size_t UpperRight = 1;
constexpr size_t MiddleRight = 2;
constexpr size_t BottomRight = 3;
constexpr size_t BottomLeft = 4;
constexpr size_t MiddleLeft = 5;

constexpr size_t ClockwiseFromUpperRight[LED_COUNT] = {
    UpperRight, MiddleRight, BottomRight,
    BottomLeft, MiddleLeft, UpperLeft,
};
} // namespace BadgeLed

// RGB channels use a logical 0-255 range. The HAL maps that entire range onto
// the badge's safe physical output range with gamma correction (squared),
// so logical 255 remains 100% to every caller even though it drives the
// LEDs at ~26% duty (66/255 per channel).
void hal_lights_init();
void hal_lights_set(size_t idx, uint8_t r, uint8_t g, uint8_t b);
void hal_lights_clear();
void hal_lights_show();

// Saved role colour normalized for visible LEDs, matching onboarding.
// Falls back to cyan when the badge has no usable role colour.
void hal_lights_get_role_color(uint8_t out[3]);

// Nonblocking sync confirmation. Called/ticked on the UI task. Three green
// pulses temporarily override LEDs, then restore the latest app-requested frame.
void hal_lights_flash_success();
void hal_lights_tick();
