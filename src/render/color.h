#pragma once

#include <stdint.h>

struct RGB {
    uint8_t r, g, b;
};

// The composite library reads each framebuffer byte as rrrgggbb.
inline uint8_t toRGB332(RGB c) {
    return (uint8_t)((((c.r * 7 + 127) / 255) << 5) | (((c.g * 7 + 127) / 255) << 2) |
                     ((c.b * 3 + 127) / 255));
}

inline RGB fromRGB332(uint8_t c) {
    return RGB{(uint8_t)((c >> 5) * 255 / 7), (uint8_t)(((c >> 2) & 7) * 255 / 7),
               (uint8_t)((c & 3) * 85)};
}

// t = 0 gives a, t = 256 gives b.
inline RGB lerp(RGB a, RGB b, int t) {
    return RGB{(uint8_t)(a.r + (((int)b.r - a.r) * t >> 8)),
               (uint8_t)(a.g + (((int)b.g - a.g) * t >> 8)),
               (uint8_t)(a.b + (((int)b.b - a.b) * t >> 8))};
}

RGB hsv(float hueDeg, float s, float v);
float luminance(RGB c);  // relative luminance, 0..1

// Nearest RGB332 colour as composite shows it (stray colour weighs more than brightness).
uint8_t nearestRGB332(RGB c);

// Near-neutral colours snap to the least tinted RGB332 grey, so edges stay clean on composite.
struct GreyTable {
    uint8_t entry[256];  // by brightness
    GreyTable();
};
extern GreyTable GREY332;

inline uint8_t toRGB332Clean(RGB c) {
    int hi = c.r > c.g ? (c.r > c.b ? c.r : c.b) : (c.g > c.b ? c.g : c.b);
    int lo = c.r < c.g ? (c.r < c.b ? c.r : c.b) : (c.g < c.b ? c.g : c.b);
    if (hi - lo < 20) return GREY332.entry[(77 * c.r + 150 * c.g + 29 * c.b) >> 8];
    return toRGB332(c);
}

// Colour statistics gathered from every decoded cover pixel.
struct ArtStats {
    static constexpr int BINS = 36;  // 10 degrees of hue each
    float weight[BINS];              // chroma-weighted pixel count per hue
    float sat[BINS], val[BINS];      // weighted sums, for the dominant hue's S and V
    float chromaSum, lumSum;
    uint32_t count;

    void reset();
    void add(uint8_t r, uint8_t g, uint8_t b);
};

struct ScenePalette {
    uint8_t bg;      // RGB332 fill behind the cover
    RGB bgRGB;       // the same colour, expanded
    RGB title, artist;
    RGB accent;      // tints the idle wave field after this track
};

ScenePalette defaultPalette();
// Background in the complement of the cover's dominant hue, with readable text colours.
ScenePalette pickPalette(const ArtStats& stats);
