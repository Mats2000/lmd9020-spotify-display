#pragma once

#include "canvas.h"
#include "font.h"

// UTF-8 text in the Sora fonts; x in 1/16 px.

int textWidth16(const Font& font, const char* utf8);

struct TextStyle {
    RGB color;
    int opacity = 256;            // 0..256
    int clipLeft = 0;             // drawn pixels stay inside [clipLeft, clipRight)
    int clipRight = Canvas::W;
    int fadeLeft = 0, fadeRight = 0;  // soft edge widths at the clip, px
    // Nearest-palette edge colours; slower, so only for text on a plain background.
    bool precise = false;
    // > 0: write palette indices ramp .. ramp + rampLevels - 1 by coverage instead of colours,
    // for text over a background of index 0 in a palette of its own (the visualizer's).
    int ramp = 0, rampLevels = 0;
};

// Draws with the pen at x16 (1/16 px) on the baseline; returns the pen position after.
int drawText(Canvas& c, const Font& font, const char* utf8, int x16, int baseline,
             const TextStyle& style);

// Copies at most size-1 bytes without splitting a UTF-8 character.
void copyUtf8(char* dst, const char* src, int size);
