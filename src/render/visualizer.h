#pragma once

#include <stdint.h>

#include "canvas.h"

// The visualizers behind the cover, chosen in the Mac app. They draw palette indices
// 0..GLOW_LEVELS - 1 (dark to bright, see glowLook) everywhere but the cover.
enum VizStyle {
    VIZ_GLOW,      // classic visualizer: loops swirling out in soft feedback trails
    VIZ_SPECTRUM,  // a 2000s receiver's fluorescent analyzer, split by the cover, peaks that hold
    VIZ_HALO,      // a glow breathing around the cover, rippling on each beat
    VIZ_AURORA,    // curtains of light swaying behind the cover, rising with the music
    VIZ_LAVA,      // slow smooth colour waves, quicker and brighter with the music
    VIZ_COUNT,
};

struct VizInput {
    const uint8_t* band;  // 16, low to high, 0..255
    float bass, mid, high, beat;  // 0..1
};

// What a visualizer keeps from frame to frame.
struct VizState {
    bool fed = false;  // the frame on screen holds this visualizer's picture, to feed back from
    float spin = 0, phase = 0;
    uint32_t beats[6] = {};  // when recent beats landed (ms)
    int nextBeat = 0;
    bool beatHeld = false;
    float level[16] = {};     // each band as the analyzer's meter shows it (0..1)
    float peaks[16] = {};
    float peakHold[16] = {};  // seconds left before the peak falls
    float peakFall[16] = {};  // how fast it is falling, per second
};

// The cover is [x0, x1) x [y0, y1); rows from `bottom` down are the song line's strip.
void drawVisualizer(int style, Canvas& c, int x0, int y0, int x1, int y1, int bottom, float dt, uint32_t ms,
                    const VizInput& in, VizState& s);
