#pragma once

#include <cstdint>

static constexpr uint8_t kRippleMaxDepth = 255;

struct RippleState {
    bool active = false;
    uint8_t depth = 0;
};

// Ripple is deliberately tiny and non-critical. The local state lives in
// /littlefs/config/ripple.cfg, so the ordinary filesystem snapshot includes it
// without any special upload path.
RippleState ripple_state_load();

// An origin starts at depth 0. A badge adopts peer_depth + 1 only when it does
// not already have a Ripple; the first successful propagation fixes its depth.
bool ripple_state_seed();
bool ripple_state_adopt(uint8_t peer_depth);

// Test/event operations used by the console command.
bool ripple_state_set_depth(unsigned depth);
bool ripple_state_clear();
