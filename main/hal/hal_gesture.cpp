#include "hal_gesture.h"

#include <cmath>

static float       s_baseline = 1000.0f;
static bool        s_primed = false;
static int         s_shake_refractory = 0;
static int         s_tap_refractory = 0;
static AccelSample s_last = {};
static float       s_last_mag = 0;

static constexpr float SHAKE_THRESHOLD_MG = 1200.0f;
static constexpr float TAP_THRESHOLD_MG   = 1800.0f;
static constexpr int   SHAKE_REFRACTORY_TICKS = 8; // ~160 ms at 50 ms tick
static constexpr int   TAP_REFRACTORY_TICKS   = 4; // ~80 ms

static bool sample_and_update()
{
    if (hal_accel_read(&s_last) != ESP_OK) return false;
    s_last_mag = sqrtf(s_last.x_mg * s_last.x_mg
                     + s_last.y_mg * s_last.y_mg
                     + s_last.z_mg * s_last.z_mg);
    if (!s_primed) { s_baseline = s_last_mag; s_primed = true; }
    s_baseline = s_baseline * 0.92f + s_last_mag * 0.08f;
    return true;
}

bool gesture_poll_shake()
{
    if (!sample_and_update()) return false;
    if (s_shake_refractory > 0) { s_shake_refractory--; return false; }
    float jerk = fabsf(s_last_mag - s_baseline);
    if (jerk > SHAKE_THRESHOLD_MG) {
        s_shake_refractory = SHAKE_REFRACTORY_TICKS;
        return true;
    }
    return false;
}

bool gesture_poll_tap()
{
    if (!sample_and_update()) return false;
    if (s_tap_refractory > 0) { s_tap_refractory--; return false; }
    float jerk = fabsf(s_last_mag - s_baseline);
    if (jerk > TAP_THRESHOLD_MG) {
        s_tap_refractory = TAP_REFRACTORY_TICKS;
        return true;
    }
    return false;
}

Orientation gesture_orientation()
{
    if (!sample_and_update()) return Orientation::Unknown;

    float ax = s_last.x_mg, ay = s_last.y_mg, az = s_last.z_mg;
    float absx = fabsf(ax), absy = fabsf(ay), absz = fabsf(az);

    // Require a dominant axis of at least 700 mg to classify.
    if (absz > absx && absz > absy && absz > 700) return az > 0 ? Orientation::FlatUp   : Orientation::FlatDown;
    if (absx > absy && absx > absz && absx > 700) return ax > 0 ? Orientation::RightEdge : Orientation::LeftEdge;
    if (absy > absx && absy > absz && absy > 700) return ay > 0 ? Orientation::TopEdge   : Orientation::BottomEdge;
    return Orientation::Unknown;
}

AccelSample gesture_last_sample()
{
    return s_last;
}
