#pragma once

#include "hal_accel.h"

// Detect an impact from the change in the full acceleration vector. Comparing
// vector magnitude alone misses rotations because gravity remains close to 1 g
// even when its X/Y/Z components move sharply. Squared deltas also avoid a
// sqrt in the 50 Hz app tick.
class CombinedAxisBumpDetector {
public:
    static constexpr float kDefaultThresholdMg = 300.0f;
    static constexpr int kDefaultRefractoryTicks = 8;

    void reset() { *this = {}; }

    bool update(const AccelSample &sample,
                float threshold_mg = kDefaultThresholdMg,
                int refractory_ticks = kDefaultRefractoryTicks)
    {
        if (!primed_) {
            previous_ = sample;
            primed_ = true;
            return false;
        }

        const float dx = sample.x_mg - previous_.x_mg;
        const float dy = sample.y_mg - previous_.y_mg;
        const float dz = sample.z_mg - previous_.z_mg;
        previous_ = sample;

        if (refractory_ > 0) --refractory_;
        const float threshold_sq = threshold_mg * threshold_mg;
        if (dx * dx + dy * dy + dz * dz <= threshold_sq || refractory_ > 0)
            return false;

        refractory_ = refractory_ticks;
        return true;
    }

private:
    AccelSample previous_{};
    bool primed_ = false;
    int refractory_ = 0;
};
