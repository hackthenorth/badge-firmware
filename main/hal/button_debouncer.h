#pragma once

#include <cstdint>

// Accept an edge only after two consecutive 10 ms samples. This preserves the
// old sampling loop's bounce rejection without its 50-100 ms input latency.
class ButtonDebouncer {
public:
    explicit ButtonDebouncer(uint16_t initial_pressed)
        : stable_pressed_(initial_pressed), candidate_pressed_(initial_pressed)
    {
    }

    uint16_t update(uint16_t sampled_pressed)
    {
        uint16_t changed = 0;
        for (uint8_t index = 0; index < kButtonCount; ++index) {
            const uint16_t bit = static_cast<uint16_t>(1u << index);
            const bool sampled = (sampled_pressed & bit) != 0;
            const bool stable = (stable_pressed_ & bit) != 0;
            const bool candidate = (candidate_pressed_ & bit) != 0;

            if (sampled == stable) {
                set_candidate(bit, sampled);
                consecutive_[index] = 0;
                continue;
            }
            if (sampled != candidate) {
                set_candidate(bit, sampled);
                consecutive_[index] = 1;
                continue;
            }
            if (++consecutive_[index] < kStableSamples) continue;

            if (sampled) stable_pressed_ |= bit;
            else stable_pressed_ &= static_cast<uint16_t>(~bit);
            consecutive_[index] = 0;
            changed |= bit;
        }
        return changed;
    }

    uint16_t stable_pressed() const { return stable_pressed_; }

private:
    void set_candidate(uint16_t bit, bool pressed)
    {
        if (pressed) candidate_pressed_ |= bit;
        else candidate_pressed_ &= static_cast<uint16_t>(~bit);
    }

    static constexpr uint8_t kButtonCount = 9;
    static constexpr uint8_t kStableSamples = 2;
    uint16_t stable_pressed_ = 0;
    uint16_t candidate_pressed_ = 0;
    uint8_t consecutive_[kButtonCount] = {};
};
