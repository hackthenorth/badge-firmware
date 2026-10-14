#include "six_seven_motion.h"

#include <cmath>

namespace {

// A slow gravity baseline leaves the fast Z waveform intact. At the 800 Hz
// sampling rate used by 67, alpha 0.0025 has an approximately half-second
// time constant.
constexpr float kBaselineAlpha = 0.0025f;

// One sample is enough at each polarity. This remains above the approximately
// 80 mg motion floor used by the factory sensor check, while an 800 Hz stream
// gives a 10 Hz shake roughly 80 opportunities per cycle to cross it.
constexpr float kPeakThresholdMg = 120.0f;

// A peak pair must finish promptly. Expiring partial pairs prevents an old
// movement from being combined with an unrelated movement later.
constexpr uint32_t kPeakPairTtlMs = 1000;
constexpr float kMaxSaneZMg = 2500.0f;

} // namespace

void SixSevenMotionDetector::reset()
{
    calibrated_ = false;
    phase_ = Phase::AwaitStartPeak;
    first_peak_ms_ = 0;
    baseline_z_mg_ = 0.0f;
    latest_z_mg_ = 0.0f;
    start_direction_ = 0;
}

void SixSevenMotionDetector::begin_cycle(uint32_t now_ms)
{
    if (calibrated_) {
        baseline_z_mg_ = latest_z_mg_;
    }
    phase_ = Phase::AwaitStartPeak;
    first_peak_ms_ = now_ms;
    start_direction_ = 0;
}

bool SixSevenMotionDetector::update(float x_mg, float y_mg, float z_mg,
                                    uint32_t now_ms)
{
    (void)x_mg;
    (void)y_mg;
    if (!std::isfinite(z_mg) || std::fabs(z_mg) > kMaxSaneZMg) return false;

    if (!calibrated_) {
        baseline_z_mg_ = z_mg;
        latest_z_mg_ = z_mg;
        calibrated_ = true;
        begin_cycle(now_ms);
        return false;
    }

    latest_z_mg_ = z_mg;
    const float dynamic_z_mg = z_mg - baseline_z_mg_;
    baseline_z_mg_ += dynamic_z_mg * kBaselineAlpha;

    if (phase_ == Phase::AwaitOppositePeak
        && now_ms - first_peak_ms_ > kPeakPairTtlMs) {
        phase_ = Phase::AwaitStartPeak;
        start_direction_ = 0;
    }

    const int8_t direction = dynamic_z_mg >= kPeakThresholdMg ? 1
        : dynamic_z_mg <= -kPeakThresholdMg ? -1 : 0;
    if (direction == 0) return false;

    switch (phase_) {
    case Phase::AwaitStartPeak:
        // Preserve the first observed polarity between reps. This makes a
        // continuous + - + - waveform count once per full pair rather than
        // once per half-cycle.
        if (start_direction_ == 0) start_direction_ = direction;
        if (direction == start_direction_) {
            first_peak_ms_ = now_ms;
            phase_ = Phase::AwaitOppositePeak;
        }
        break;
    case Phase::AwaitOppositePeak:
        if (direction == -start_direction_) {
            phase_ = Phase::AwaitStartPeak;
            return true;
        }
        break;
    }
    return false;
}
