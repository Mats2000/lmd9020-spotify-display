#pragma once

#include <stdint.h>
#include <string.h>

#include "color.h"

// The framebuffer: 240 rows of 256 RGB332 pixels (rows are separate allocations).
struct Canvas {
    static constexpr int W = 256;
    static constexpr int H = 240;

    uint8_t** rows;

    void fill(uint8_t c) {
        for (int y = 0; y < H; y++) memset(rows[y], c, W);
    }

    // a: 0 (keep dst) .. 256 (pure c)
    void blend(int x, int y, RGB c, int a) {
        if ((unsigned)x >= (unsigned)W || (unsigned)y >= (unsigned)H || a <= 0) return;
        uint8_t* p = &rows[y][x];
        *p = toRGB332Clean(a >= 256 ? c : lerp(fromRGB332(*p), c, a));
    }
};
