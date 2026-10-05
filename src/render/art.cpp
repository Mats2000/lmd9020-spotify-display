#include "art.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

ArtBuilder::~ArtBuilder() { free(band_); }

void ArtBuilder::reserve(int srcW, int bandRows) {
    if (band_) return;
    band_ = (uint16_t*)malloc((size_t)bandRows * srcW * 2);
    bandCap_ = band_ ? bandRows : 0;
    reservedW_ = band_ ? srcW : 0;
}

void ArtBuilder::begin(uint32_t* dst, int srcW, int srcH) {
    dst_ = dst;
    srcW_ = srcW;
    srcH_ = srcH;
    side_ = srcW < srcH ? srcW : srcH;
    cropX_ = (srcW - side_) / 2;
    cropY_ = (srcH - side_) / 2;
    for (int tx = 0; tx <= ART_W; tx++) colStart_[tx] = (uint16_t)(cropX_ + tx * side_ / ART_W);
    accRows_ = 0;
    accTy_ = -1;
    lastTy_ = -1;
    bandY0_ = -1;
    bandRows_ = 0;
    memset(err_, 0, sizeof(err_));
    stats_.reset();
}

void ArtBuilder::block(const uint8_t* rgb, int x0, int y0, int x1, int y1) {
    int bw = x1 - x0 + 1, bh = y1 - y0 + 1;
    for (int i = 0, n = bw * bh; i < n; i++) stats_.add(rgb[i * 3], rgb[i * 3 + 1], rgb[i * 3 + 2]);

    if (y0 != bandY0_) {  // a new row of blocks: the previous one is complete
        flushBand();
        bandY0_ = y0;
        bandRows_ = bh;
        if (bh > bandCap_ || (reservedW_ && reservedW_ < srcW_)) {
            reservedW_ = 0;
            free(band_);
            band_ = (uint16_t*)malloc((size_t)bh * srcW_ * 2);
            bandCap_ = band_ ? bh : 0;
        }
    }
    if (!band_ || x1 >= srcW_) return;
    for (int r = 0; r < bh && r < bandRows_; r++) {
        uint16_t* out = band_ + (size_t)r * srcW_ + x0;
        const uint8_t* in = rgb + r * bw * 3;
        for (int i = 0; i < bw; i++, in += 3) out[i] = (uint16_t)(((in[0] >> 3) << 11) | ((in[1] >> 2) << 5) | (in[2] >> 3));
    }
}

void ArtBuilder::finish() {
    flushBand();
    if (accRows_) emitRow(accTy_);
    accRows_ = 0;
    if (lastTy_ >= 0) {  // the bottom row, with itself standing in for the row below
        const uint8_t* mid = win_[lastTy_ % 3];
        outputRow(lastTy_, lastTy_ > 0 ? win_[(lastTy_ - 1) % 3] : mid, mid, mid);
    }
}

void ArtBuilder::flushBand() {
    if (bandY0_ < 0 || !band_) return;
    for (int r = 0; r < bandRows_; r++) {
        int cy = bandY0_ + r - cropY_;
        if (cy >= 0 && cy < side_) sourceRow(band_ + (size_t)r * srcW_, cy);
    }
    bandY0_ = -1;
}

// Box-filters a source row into the output columns and accumulates it into its output row.
void ArtBuilder::sourceRow(const uint16_t* row, int cy) {
    uint32_t sums[ART_W * 3];
    uint16_t counts[ART_W];
    for (int tx = 0; tx < ART_W; tx++) {
        int a = colStart_[tx], b = colStart_[tx + 1];
        if (b <= a) b = a + 1;  // scaling up: nearest
        uint32_t r = 0, g = 0, bl = 0;
        for (int x = a; x < b; x++) {
            uint16_t p = row[x];  // RGB565 back to 8 bits a channel
            r += ((p >> 11) << 3) | (p >> 13);
            g += (((p >> 5) & 63) << 2) | ((p >> 9) & 3);
            bl += ((p & 31) << 3) | ((p >> 2) & 7);
        }
        sums[tx * 3] = r, sums[tx * 3 + 1] = g, sums[tx * 3 + 2] = bl;
        counts[tx] = (uint16_t)(b - a);
    }
    auto add = [&](void) {
        for (int tx = 0; tx < ART_W; tx++)
            for (int ch = 0; ch < 3; ch++) acc_[tx * 3 + ch] += (uint16_t)(sums[tx * 3 + ch] * 16 / counts[tx]);
        accRows_++;
    };

    if (side_ >= ART_H) {
        int ty = cy * ART_H / side_;
        if (ty != accTy_ && accRows_) {
            emitRow(accTy_);
            accRows_ = 0;
        }
        if (!accRows_) memset(acc_, 0, sizeof(acc_));
        accTy_ = ty;
        add();
    } else {
        for (int ty = (cy * ART_H + side_ - 1) / side_; ty < ((cy + 1) * ART_H + side_ - 1) / side_ && ty < ART_H; ty++) {
            memset(acc_, 0, sizeof(acc_));
            accRows_ = 0;
            add();
            emitRow(ty);
        }
        accRows_ = 0;
    }
}

// Averages row ty, then writes the row above it now that its neighbours exist.
void ArtBuilder::emitRow(int ty) {
    if (ty < 0 || ty >= ART_H || !accRows_) return;
    uint8_t* row = win_[ty % 3];
    for (int i = 0; i < ART_W * 3; i++) row[i] = (uint8_t)((acc_[i] / accRows_ + 8) >> 4);
    if (ty >= 1) {
        const uint8_t* mid = win_[(ty - 1) % 3];
        outputRow(ty - 1, ty >= 2 ? win_[(ty - 2) % 3] : mid, mid, row);
    }
    lastTy_ = ty;
}

// Sharpens row ty (brightness only, stronger across than down), then writes it
// Floyd-Steinberg dithered in a serpentine scan.
void ArtBuilder::outputRow(int ty, const uint8_t* up, const uint8_t* mid, const uint8_t* down) {
    auto luma = [](const uint8_t* p) { return (77 * p[0] + 150 * p[1] + 29 * p[2]) >> 4; };  // x16

    int16_t* cur = err_[ty & 1];
    int16_t* next = err_[(ty + 1) & 1];
    memset(next, 0, sizeof(err_[0]));
    static const int LEVELS[3] = {7, 7, 3};
    bool rtl = ty & 1;
    for (int i = 0; i < ART_W; i++) {
        int tx = rtl ? ART_W - 1 - i : i;
        int dir = rtl ? -1 : 1;
        const uint8_t* c = mid + tx * 3;
        int delta = 0;
#if COVER_SHARPEN
        int y = luma(c);
        int yl = luma(mid + (tx > 0 ? tx - 1 : tx) * 3), yr = luma(mid + (tx < ART_W - 1 ? tx + 1 : tx) * 3);
        int yu = luma(up + tx * 3), yd = luma(down + tx * 3);
        delta = (150 * (2 * y - yl - yr) + 80 * (2 * y - yu - yd)) >> 8;  // about 0.6 across, 0.3 down
#endif
        uint8_t q[3];
        for (int ch = 0; ch < 3; ch++) {
            int L = LEVELS[ch];
            int v16 = c[ch] * 16 + delta;  // value x16
            v16 = v16 < 0 ? 0 : (v16 > 255 * 16 ? 255 * 16 : v16);
#if COVER_DITHER
            v16 += cur[(tx + 1) * 3 + ch];
#endif
            int v = (v16 + 8) / 16;
            int level = (v * L + 127) / 255;
            level = level < 0 ? 0 : (level > L ? L : level);
            q[ch] = (uint8_t)level;
#if COVER_DITHER
            int e = v16 - level * 255 * 16 / L;  // error x16, shared out 7/16, 3/16, 5/16, 1/16
            cur[(tx + 1 + dir) * 3 + ch] += (int16_t)(e * 7 / 16);
            next[(tx + 1 - dir) * 3 + ch] += (int16_t)(e * 3 / 16);
            next[(tx + 1) * 3 + ch] += (int16_t)(e * 5 / 16);
            next[(tx + 1 + dir) * 3 + ch] += (int16_t)(e / 16);
#endif
        }
        putArtPixel(dst_, tx, ty, (uint8_t)((q[0] << 5) | (q[1] << 2) | q[2]));
    }
}

void fillPlaceholderArt(uint32_t* dst, const ScenePalette& pal) {
    // Darker than the background, so the white title on it reads.
    uint32_t c = toRGB332(lerp(pal.bgRGB, RGB{0, 0, 0}, 110)) * 0x01010101u;
    volatile uint32_t* w = dst;
    for (int i = 0; i < ART_WORDS; i++) w[i] = c;
}
