#pragma once

#include <cstdint>

// Recognizes rapid up/down shakes on the badge's Z axis. One strong Z peak
// followed by a strong peak in the opposite direction is one rep. No settled
// pause is required; the 67 app feeds this detector at up to 800 Hz.
class SixSevenMotionDetector {
public:
    void reset();
    void begin_cycle(uint32_t now_ms);

    // Returns true exactly once for each completed movement cycle.
    bool update(float x_mg, float y_mg, float z_mg, uint32_t now_ms);

private:
    enum class Phase : uint8_t {
        AwaitStartPeak,
        AwaitOppositePeak,
    };

    Phase phase_ = Phase::AwaitStartPeak;
    uint32_t first_peak_ms_ = 0;
    float baseline_z_mg_ = 0.0f;
    float latest_z_mg_ = 0.0f;
    int8_t start_direction_ = 0;
    bool calibrated_ = false;
};
