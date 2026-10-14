#pragma once

#include <stdint.h>

// WS2812 status lights, same strip and pin as the badge, driven dim: idle
// green, busy gold, fault red. All output passes through the badge's
// gamma-capped curve plus an extra divide-by-4, so full logical brightness
// lands around 16/255 physical. Init failure (or no strip) degrades to
// no-ops; the box runs headless either way.
void lights_init(void);

// Set every pixel to one dimmed status color.
void lights_status(uint8_t r, uint8_t g, uint8_t b);
