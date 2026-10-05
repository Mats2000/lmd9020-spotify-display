#include "draw.h"

#include <math.h>

void fillRect(Canvas& c, int x, int y, int w, int h, uint8_t color) {
    int x0 = x < 0 ? 0 : x, x1 = x + w > Canvas::W ? Canvas::W : x + w;
    int y0 = y < 0 ? 0 : y, y1 = y + h > Canvas::H ? Canvas::H : y + h;
    if (x1 <= x0) return;
    for (int yy = y0; yy < y1; yy++) memset(c.rows[yy] + x0, color, x1 - x0);
}

void hline(Canvas& c, int x0, int x1, int y, uint8_t color) {
    if (x1 < x0) {
        int t = x0;
        x0 = x1, x1 = t;
    }
    fillRect(c, x0, y, x1 - x0 + 1, 1, color);
}

void vline(Canvas& c, int x, int y0, int y1, uint8_t color) {
    if (y1 < y0) {
        int t = y0;
        y0 = y1, y1 = t;
    }
    fillRect(c, x, y0, 1, y1 - y0 + 1, color);
}

void lineAA(Canvas& c, float x0, float y0, float x1, float y1, RGB color, int alpha) {
    bool steep = fabsf(y1 - y0) > fabsf(x1 - x0);
    if (steep) {
        float t = x0;
        x0 = y0, y0 = t;
        t = x1, x1 = y1, y1 = t;
    }
    if (x0 > x1) {
        float t = x0;
        x0 = x1, x1 = t;
        t = y0, y0 = y1, y1 = t;
    }
    float gradient = x1 - x0 < 1e-3f ? 0 : (y1 - y0) / (x1 - x0);
    int xs = (int)lroundf(x0), xe = (int)lroundf(x1);
    if (xs < -1) xs = -1;
    if (xe > (steep ? Canvas::H : Canvas::W)) xe = steep ? Canvas::H : Canvas::W;
    for (int x = xs; x <= xe; x++) {
        float y = y0 + gradient * (x - x0);
        int yi = (int)floorf(y);
        int a = (int)((y - yi) * alpha);
        if (steep) {
            c.blend(yi, x, color, alpha - a);
            c.blend(yi + 1, x, color, a);
        } else {
            c.blend(x, yi, color, alpha - a);
            c.blend(x, yi + 1, color, a);
        }
    }
}

DitherTables::DitherTables() {
    static const uint8_t B[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};
    memcpy(bayer, B, sizeof(bayer));
    for (int t = 0; t < 16; t++) {
        off7[t] = (uint8_t)((2 * t + 1) * 255 / (32 * 7));
        off3[t] = (uint8_t)((2 * t + 1) * 255 / (32 * 3));
    }
    for (int w = 0; w < (int)sizeof(q7); w++) q7[w] = (uint8_t)(w * 7 / 255 > 7 ? 7 : w * 7 / 255);
    for (int w = 0; w < (int)sizeof(q3); w++) q3[w] = (uint8_t)(w * 3 / 255 > 3 ? 3 : w * 3 / 255);
}

DitherTables DITHER;  // in RAM: lookups from flash would go through the shared cache

float hash01(uint32_t n) {
    n ^= n >> 16;
    n *= 0x7feb352dU;
    n ^= n >> 15;
    n *= 0x846ca68bU;
    n ^= n >> 16;
    return (n & 0xFFFFFF) / 16777216.0f;
}

float valueNoise(uint32_t seed, float t) {
    float f = floorf(t);
    float u = t - f;
    u = u * u * (3 - 2 * u);
    uint32_t i = (uint32_t)(int32_t)f;
    float a = hash01(seed * 0x9E3779B1U + i), b = hash01(seed * 0x9E3779B1U + i + 1);
    return a + (b - a) * u;
}
