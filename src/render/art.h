#pragma once

#include <stdint.h>

#include "color.h"
#include "config.h"

// Cover size in lines; narrower in pixels so it comes out square on screen.
constexpr int ART_H = 184;
constexpr int ART_W = (int)(ART_H / DISPLAY_PIXEL_ASPECT + 0.5f);

// Packed four pixels per 32-bit word: the buffer lives in IRAM, which only allows
// 32-bit access. Use these helpers.
constexpr int ART_STRIDE = (ART_W + 3) & ~3;
constexpr int ART_WORDS = ART_STRIDE * ART_H / 4;

inline void putArtPixel(uint32_t* art, int x, int y, uint8_t v) {
    volatile uint32_t* w = art + ((y * ART_STRIDE + x) >> 2);
    int shift = (x & 3) * 8;
    *w = (*w & ~(0xFFu << shift)) | ((uint32_t)v << shift);
}

// Unpacks row y into a normal byte buffer (a framebuffer row).
inline void copyArtRow(const uint32_t* art, int y, uint8_t* out) {
    const volatile uint32_t* src = art + y * (ART_STRIDE / 4);
    for (int x = 0; x < ART_W; x += 4) {
        uint32_t w = src[x >> 2];
        int n = ART_W - x < 4 ? ART_W - x : 4;
        for (int i = 0; i < n; i++) out[x + i] = (uint8_t)(w >> (8 * i));
    }
}

// After the pixels: the cover's own palette (0x00RRGGBB, darkest first) when it has one, then
// those colours encoded for the video signal (while decoding: their brightness and colour).
constexpr int ART_PALETTE_WORDS = 256;
constexpr int ART_ALLOC_WORDS = ART_WORDS + 2 * ART_PALETTE_WORDS;
inline uint32_t* artPalette(uint32_t* art) { return art + ART_WORDS; }
inline const uint32_t* artPalette(const uint32_t* art) { return art + ART_WORDS; }
inline uint32_t* artEncoded(uint32_t* art) { return art + ART_WORDS + ART_PALETTE_WORDS; }

// Picks up to 256 colours for a cover from n sample pixels (0x00RRGGBB words at the start of
// `art`, n <= ART_WORDS - 512), into artPalette(art). Returns how many.
int buildArtPalette(uint32_t* art, int n);

// Turns decoded JPEG blocks into ART_W x ART_H packed RGB332, a band at a time:
// box-filtered, centre-cropped, brightness-sharpened and error-diffusion dithered.
class ArtBuilder {
public:
    ~ArtBuilder();
    // Optional: allocate the band buffer before the download starts.
    void reserve(int srcW, int bandRows);
    // srcW x srcH is the size of the decoded (already down-scaled) image. With colours > 0 the
    // pixels index artPalette(dst) (see buildArtPalette) instead of being RGB332.
    void begin(uint32_t* dst, int srcW, int srcH, int colours = 0);
    // One decoded RGB888 block covering x0..x1, y0..y1, as the JPEG decoder hands them out.
    void block(const uint8_t* rgb, int x0, int y0, int x1, int y1);
    // Call after the last block.
    void finish();
    const ArtStats& stats() const { return stats_; }

private:
    void flushBand();
    void sourceRow(const uint16_t* row, int cy);
    void emitRow(int ty);
    void outputRow(int ty, const uint8_t* up, const uint8_t* mid, const uint8_t* down);

    int nearest(int r, int g, int b) const;

    uint32_t* dst_ = nullptr;
    int colours_ = 0;
    int srcW_ = 0, srcH_ = 0, side_ = 0, cropX_ = 0, cropY_ = 0;

    uint16_t* band_ = nullptr;  // band rows of srcW_ RGB565 pixels (16-bit: RAM is tight while decoding)
    int bandY0_ = -1, bandRows_ = 0, bandCap_ = 0, reservedW_ = 0;

    uint16_t colStart_[ART_W + 1];  // output column tx averages source columns [colStart_[tx], colStart_[tx+1])
    uint16_t acc_[ART_W * 3];       // sums (x16) for the output row being built
    int accRows_ = 0, accTy_ = -1;
    uint8_t win_[3][ART_W * 3];        // the last three averaged rows, for sharpening
    int lastTy_ = -1;
    int16_t err_[2][(ART_W + 2) * 3];  // dither error, this row and the next (x16)
    ArtStats stats_;
};

// Flat stand-in when a cover can't be fetched or decoded.
void fillPlaceholderArt(uint32_t* dst, const ScenePalette& pal);
