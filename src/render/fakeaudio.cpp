#include "fakeaudio.h"

#include <math.h>

#include "draw.h"

static float bump(float x, float centre, float width) {
    float d = (x - centre) / width;
    return expf(-d * d);
}

void FakeAudio::update(uint32_t ms) {
    float dt = started_ ? (ms - last_) * 0.001f : 0.0f;
    if (dt > 0.1f) dt = 0.1f;
    started_ = true;
    last_ = ms;

    // Wraps every 10 minutes (exactly 1120 beats).
    const float t = (ms % 600000u) * 0.001f;
    const float beat = t * (112.0f / 60.0f);
    const int beatIndex = (int)beat;
    const float phase = beat - beatIndex;
    const int phrase = beatIndex / 32;  // 8 bars

    // Each phrase gets its own energy; one in five is a breakdown with no kick.
    float energy = 0.55f + 0.45f * hash01(phrase * 7 + 1);
    bool breakdown = hash01(phrase * 13 + 5) < 0.2f;
    float kick = breakdown ? 0 : expf(-phase * 7.0f);
    float snare = (beatIndex & 1) ? expf(-phase * 9.0f) : 0;
    float hat = expf(-(beat * 2 - floorf(beat * 2)) * 14.0f);

    for (int i = 0; i < BANDS; i++) {
        float x = i / (float)(BANDS - 1);
        float melody = valueNoise(100 + i, t * (1.6f + 0.25f * (i % 5)));
        float target = 0.20f * (1.0f - 0.5f * x)               // spectral tilt
                       + 0.85f * kick * bump(x, 0.05f, 0.16f)  //
                       + 0.55f * snare * bump(x, 0.45f, 0.22f)  //
                       + 0.45f * hat * bump(x, 0.88f, 0.14f)    //
                       + 0.45f * melody * bump(x, 0.35f + 0.3f * valueNoise(7, t * 0.2f), 0.35f);
        target *= 1.9f * energy * (0.85f + 0.3f * valueNoise(200 + i, t * 6.0f));
        if (target > 1.0f) target = 1.0f;

        // Analyser ballistics: jump up, fall at a steady rate.
        if (target > level_[i])
            level_[i] += (target - level_[i]) * (dt * 25.0f > 1 ? 1 : dt * 25.0f);
        else
            level_[i] = level_[i] - dt * 1.4f < target ? target : level_[i] - dt * 1.4f;

        // Peak hold: sit for 0.7 s, then drop.
        if (level_[i] >= peak_[i]) {
            peak_[i] = level_[i];
            hold_[i] = 0.7f;
        } else if ((hold_[i] -= dt) < 0) {
            peak_[i] -= dt * 0.6f;
            if (peak_[i] < level_[i]) peak_[i] = level_[i];
        }
    }

    // VU needles: ~300 ms integration, channels slightly different.
    for (int ch = 0; ch < 2; ch++) {
        float sum = 0, weight = 0;
        for (int i = 0; i < BANDS; i++) {
            float w = ((i + ch) & 1) ? 1.15f : 0.85f;
            sum += level_[i] * w;
            weight += w;
        }
        float target = 1.6f * sum / weight * (0.9f + 0.2f * valueNoise(300 + ch, t * 3.0f));
        float k = 1.0f - expf(-dt / 0.3f);
        vu_[ch] += (target - vu_[ch]) * k;
    }
}
