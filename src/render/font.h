#pragma once

#include <stdint.h>

// Anti-aliased bitmap font from tools/make_fonts.py: 4-bit alpha, two pixels per byte.
struct Glyph {
    uint16_t code;     // Unicode code point; the table is sorted by this
    uint16_t advance;  // pen advance in 1/16 px
    uint8_t width, height;
    int8_t left;       // bitmap x relative to the pen
    int8_t top;        // bitmap's first row relative to the baseline (negative = above)
    uint32_t offset;   // into Font::bitmap
};

struct Font {
    const uint8_t* bitmap;
    const Glyph* glyphs;
    uint16_t count;
    uint8_t cap;       // cap height in px, for optical vertical centring
    uint8_t ascent;
    uint8_t descent;
};
