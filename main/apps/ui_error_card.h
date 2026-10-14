#pragma once

#include <cstdint>

// Creates the reusable top-layer card and its static-backed labels. Must run
// under the LVGL lock before user Lua gets control.
void ui_error_card_prepare();

// Shows the card, or updates the existing card when it is already visible.
void ui_error_card_show(const char *callback,
                        const char *message,
                        std::uint32_t repeat_count,
                        const char *hint);

void ui_error_card_hide();
