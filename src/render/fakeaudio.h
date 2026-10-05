#pragma once

#include <stdint.h>

// A made-up 112 BPM song for the level meters, with analyser and VU ballistics.
class FakeAudio {
public:
    static constexpr int BANDS = 16;

    void update(uint32_t ms);
    float band(int i) const { return level_[i]; }  // analyser bar, 0..1
    float peak(int i) const { return peak_[i]; }   // peak-hold marker, 0..1
    float vu(int ch) const { return vu_[ch]; }     // needle: 1.0 = 0 VU, ~1.41 = +3

private:
    float level_[BANDS] = {};
    float peak_[BANDS] = {};
    float hold_[BANDS] = {};
    float vu_[2] = {};
    uint32_t last_ = 0;
    bool started_ = false;
};
