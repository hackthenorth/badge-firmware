#pragma once

#include "hal_accel.h"

enum class Orientation {
    Unknown,
    FlatUp,
    FlatDown,
    LeftEdge,
    RightEdge,
    TopEdge,
    BottomEdge,
};

// Each poll reads one accelerometer sample and returns whether the
// corresponding gesture just fired. Intended for use from an app's on_tick;
// refractory handling is internal. The helpers share a single baseline, so
// polling both `shake` and `tap` from the same app is fine.

bool        gesture_poll_shake();    // large sustained deviation; default ~1200 mg
bool        gesture_poll_tap();      // sharper, higher-threshold single-sample hit
Orientation gesture_orientation();   // reads one sample and classifies gravity axis

// Returns the last sample read by any of the above (or an all-zero sample
// if nothing has been polled yet). Useful for apps that want the raw values.
AccelSample gesture_last_sample();
