#pragma once

#include "canvas.h"

// Drawing primitives. Framebuffer coordinates; everything clips.

void fillRect(Canvas& c, int x, int y, int w, int h, uint8_t color);
void hline(Canvas& c, int x0, int x1, int y, uint8_t color);
void vline(Canvas& c, int x, int y0, int y1, uint8_t color);

// Anti-aliased line, blended over what's there. alpha 0..256.
void lineAA(Canvas& c, float x0, float y0, float x1, float y1, RGB color, int alpha = 256);

// Ordered-dithered RGB332 via lookup tables (inline: runs per pixel).
struct DitherTables {
    uint8_t bayer[4][4];
    uint8_t off7[16], off3[16];  // threshold offsets for 8-level and 4-level channels
    uint8_t q7[256 + 37], q3[256 + 86];
    DitherTables();
};
extern DitherTables DITHER;

inline uint8_t dither332(RGB c, int x, int y) {
    const DitherTables& d = DITHER;
    int t = d.bayer[y & 3][x & 3];
    return (uint8_t)((d.q7[c.r + d.off7[t]] << 5) | (d.q7[c.g + d.off7[t]] << 2) | d.q3[c.b + d.off3[t]]);
}

// Deterministic noise, so animations are a pure function of time.
float hash01(uint32_t n);                 // 0..1
float valueNoise(uint32_t seed, float t);  // smooth 0..1
