#include "art.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifdef ARDUINO
#include <esp_heap_caps.h>
#endif

ArtBuilder::~ArtBuilder() { free((void*)band_); }

void ArtBuilder::allocBand(int rows, int w) {
    free((void*)band_);
    bandStride_ = (w + 1) / 2;
    const size_t bytes = (size_t)rows * bandStride_ * 4;
    void* p = nullptr;
#ifdef ARDUINO
    // Only IRAM proper (0x4008_0000..0x400A_0000): the rest of the executable heap is ordinary
    // RAM seen through another address, which would gain nothing.
    p = heap_caps_malloc(bytes, MALLOC_CAP_EXEC);
    if (p && ((uintptr_t)p < 0x40080000 || (uintptr_t)p + bytes > 0x400A0000)) {
        heap_caps_free(p);
        p = nullptr;
    }
#endif
    if (!p) p = malloc(bytes);
    band_ = (volatile uint32_t*)p;
    bandCap_ = band_ ? rows : 0;
}

void ArtBuilder::reserve(int srcW, int bandRows) {
    if (band_) return;
    allocBand(bandRows, srcW);
    reservedW_ = band_ ? srcW : 0;
}

void ArtBuilder::begin(uint32_t* dst, int srcW, int srcH, int colours) {
    dst_ = dst;
    colours_ = colours;
    if (colours_ > 0) {  // brightness and colour of each palette entry, for nearest()
        const volatile uint32_t* pal = artPalette(dst);
        volatile uint32_t* yuv = artEncoded(dst);
        for (int i = 0; i < colours_; i++) {
            uint32_t p = pal[i];
            int r = (p >> 16) & 255, g = (p >> 8) & 255, b = p & 255;
            int y = (77 * r + 150 * g + 29 * b) >> 8, u = (126 * (b - y)) >> 8, v = (224 * (r - y)) >> 8;
            yuv[i] = (uint32_t)y | ((uint32_t)(u & 255) << 8) | ((uint32_t)(v & 255) << 16);
        }
    }
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
            allocBand(bh, srcW_);
        }
    }
    if (!band_ || x1 >= srcW_) return;
    for (int r = 0; r < bh && r < bandRows_; r++) {
        volatile uint32_t* row = band_ + (size_t)r * bandStride_;
        const uint8_t* in = rgb + r * bw * 3;
        for (int x = x0; x <= x1; x++, in += 3) {
            const uint32_t p = ((in[0] >> 3) << 11) | ((in[1] >> 2) << 5) | (in[2] >> 3);
            const uint32_t w = row[x >> 1];
            row[x >> 1] = x & 1 ? (w & 0xFFFF) | p << 16 : (w & 0xFFFF0000) | p;
        }
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
        if (cy >= 0 && cy < side_) sourceRow(band_ + (size_t)r * bandStride_, cy);
    }
    bandY0_ = -1;
}

// Box-filters a source row into the output columns and accumulates it into its output row.
void ArtBuilder::sourceRow(const volatile uint32_t* row, int cy) {
    uint32_t sums[ART_W * 3];
    uint16_t counts[ART_W];
    for (int tx = 0; tx < ART_W; tx++) {
        int a = colStart_[tx], b = colStart_[tx + 1];
        if (b <= a) b = a + 1;  // scaling up: nearest
        uint32_t r = 0, g = 0, bl = 0;
        for (int x = a; x < b; x++) {
            const uint32_t p = (row[x >> 1] >> ((x & 1) << 4)) & 0xFFFF;  // RGB565 back to 8 bits a channel
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

// Composite turns any stray pixel into a coloured speck, so each pixel takes the nearest of its
// 8 neighbouring RGB332 colours with colour error weighted heavily, and only part of the error
// is passed on (Floyd-Steinberg, serpentine). A cover with its own palette needs much less.
namespace {
constexpr int CHROMA_WEIGHT = 3;    // colour error counts this many times brightness error
constexpr int LUMA_KEEP = 12;       // sixteenths of the brightness error passed on
constexpr int GREY_KEEP = 2;        // sixteenths of the colour error passed on near grey and white...
constexpr int COLOUR_KEEP = 12;     // ...rising to this in saturated colour, so gradients stay smooth
constexpr int DEAD_ZONE = 6 * 16;   // brightness error below this isn't passed on: flat areas stay flat
constexpr int ERR_LIMIT = 48 * 16;  // per channel, x16
// With the cover's own palette:
constexpr int PAL_LUMA_KEEP = 14, PAL_CHROMA_KEEP = 10, PAL_DEAD_ZONE = 2 * 16;

inline int yOf(uint32_t w) { return (int)(w & 255); }
inline int uOf(uint32_t w) { return (int8_t)((w >> 8) & 255); }
inline int vOf(uint32_t w) { return (int8_t)((w >> 16) & 255); }
}  // namespace

// The palette entry closest to r, g, b: entries are sorted by brightness, so search outward
// from the matching brightness until brightness alone is further than the best so far.
int ArtBuilder::nearest(int r, int g, int b) const {
    const volatile uint32_t* yuv = artEncoded(dst_);
    const int y = (77 * r + 150 * g + 29 * b) >> 8, u = (126 * (b - y)) >> 8, v = (224 * (r - y)) >> 8;
    int lo = 0, hi = colours_;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (yOf(yuv[mid]) < y) lo = mid + 1;
        else hi = mid;
    }
    int best = 0, bestCost = 0x7fffffff;
    for (int j = lo; j < colours_; j++) {
        uint32_t w = yuv[j];
        int dy = yOf(w) - y, cost = 3 * dy * dy;
        if (cost >= bestCost) break;
        int du = uOf(w) - u, dv = vOf(w) - v;
        cost += du * du + dv * dv;
        if (cost < bestCost) bestCost = cost, best = j;
    }
    for (int j = lo - 1; j >= 0; j--) {
        uint32_t w = yuv[j];
        int dy = y - yOf(w), cost = 3 * dy * dy;
        if (cost >= bestCost) break;
        int du = uOf(w) - u, dv = vOf(w) - v;
        cost += du * du + dv * dv;
        if (cost < bestCost) bestCost = cost, best = j;
    }
    return best;
}

void ArtBuilder::outputRow(int ty, const uint8_t* up, const uint8_t* mid, const uint8_t* down) {
#if COVER_SHARPEN
    auto luma = [](const uint8_t* p) { return (77 * p[0] + 150 * p[1] + 29 * p[2]) >> 4; };  // x16
#else
    (void)up, (void)down;
#endif

    int16_t* cur = err_[ty & 1];
    int16_t* next = err_[(ty + 1) & 1];
    memset(next, 0, sizeof(err_[0]));
    static const int LEVELS[3] = {7, 7, 3};
    const volatile uint32_t* pal = artPalette(dst_);
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
        int t[3];  // target x16, with the error passed on
        for (int ch = 0; ch < 3; ch++) {
            int v16 = c[ch] * 16 + delta;
#if COVER_DITHER
            int e = cur[(tx + 1) * 3 + ch];
            v16 += e < -ERR_LIMIT ? -ERR_LIMIT : (e > ERR_LIMIT ? ERR_LIMIT : e);
#endif
            t[ch] = v16 < 0 ? 0 : (v16 > 255 * 16 ? 255 * 16 : v16);
        }

        int got[3], lumaKeep, chromaKeep, deadZone;
        uint8_t out;
        if (colours_ > 0) {
            int j = nearest((t[0] + 8) >> 4, (t[1] + 8) >> 4, (t[2] + 8) >> 4);
            uint32_t p = pal[j];
            got[0] = (int)((p >> 16) & 255) * 16, got[1] = (int)((p >> 8) & 255) * 16, got[2] = (int)(p & 255) * 16;
            out = (uint8_t)j;
            lumaKeep = PAL_LUMA_KEEP, chromaKeep = PAL_CHROMA_KEEP, deadZone = PAL_DEAD_ZONE;
        } else {
            int val[2][3], lo[3];
            for (int ch = 0; ch < 3; ch++) {
                int L = LEVELS[ch];
                int l = t[ch] * L / (255 * 16);
                if (l >= L) l = L - 1;
                lo[ch] = l;
                val[0][ch] = l * 255 * 16 / L;
                val[1][ch] = (l + 1) * 255 * 16 / L;
            }
            int best = 0, bestCost = 0x7fffffff;
            for (int m = 0; m < 8; m++) {
                int dr = val[(m >> 2) & 1][0] - t[0], dg = val[(m >> 1) & 1][1] - t[1], db = val[m & 1][2] - t[2];
                int dy = (77 * dr + 150 * dg + 29 * db) >> 8;
                int du = (126 * (db - dy)) >> 8, dv = (224 * (dr - dy)) >> 8;
                int cost = dy * dy + CHROMA_WEIGHT * (du * du + dv * dv);
                if (cost < bestCost) bestCost = cost, best = m;
            }
            int pick[3] = {(best >> 2) & 1, (best >> 1) & 1, best & 1};
            for (int ch = 0; ch < 3; ch++) got[ch] = val[pick[ch]][ch];
            out = (uint8_t)(((lo[0] + pick[0]) << 5) | ((lo[1] + pick[1]) << 2) | (lo[2] + pick[2]));
            int hi = t[0] > t[1] ? (t[0] > t[2] ? t[0] : t[2]) : (t[1] > t[2] ? t[1] : t[2]);
            int lo3 = t[0] < t[1] ? (t[0] < t[2] ? t[0] : t[2]) : (t[1] < t[2] ? t[1] : t[2]);
            int sat = hi - lo3;  // x16
            lumaKeep = LUMA_KEEP, deadZone = DEAD_ZONE;
            chromaKeep = sat >= 96 * 16 ? COLOUR_KEEP : GREY_KEEP + (COLOUR_KEEP - GREY_KEEP) * sat / (96 * 16);
        }
#if COVER_DITHER
        int e[3];
        for (int ch = 0; ch < 3; ch++) e[ch] = t[ch] - got[ch];
        int ey = (77 * e[0] + 150 * e[1] + 29 * e[2]) >> 8;
        int eyKept = ey > deadZone ? ey - deadZone : (ey < -deadZone ? ey + deadZone : 0);
        for (int ch = 0; ch < 3; ch++) {
            int d = (eyKept * lumaKeep + (e[ch] - ey) * chromaKeep) / 16;  // shared out 7/16, 3/16, 5/16, 1/16
            cur[(tx + 1 + dir) * 3 + ch] += (int16_t)(d * 7 / 16);
            next[(tx + 1 - dir) * 3 + ch] += (int16_t)(d * 3 / 16);
            next[(tx + 1) * 3 + ch] += (int16_t)(d * 5 / 16);
            next[(tx + 1 + dir) * 3 + ch] += (int16_t)(d / 16);
        }
#else
        (void)got, (void)lumaKeep, (void)chromaKeep, (void)deadZone;
#endif
        putArtPixel(dst_, tx, ty, out);
    }
}

// Variance-style cut: keep splitting the box whose widest channel (weighted toward green, as
// brightness is) times its pixel count is largest, at that channel's mean.
int buildArtPalette(uint32_t* art, int n) {
    volatile uint32_t* px = art;
    volatile uint32_t* box = art + n;          // start | count << 16
    volatile uint32_t* score = art + n + 256;  // (spread * count) << 2 | channel
    static const int WEIGHT[3] = {3, 4, 2};
    auto measure = [&](int start, int count) -> uint32_t {
        if (count < 2) return 0;
        int lo[3] = {255, 255, 255}, hi[3] = {0, 0, 0};
        for (int i = start; i < start + count; i++) {
            uint32_t p = px[i];
            for (int ch = 0; ch < 3; ch++) {
                int v = (p >> (16 - 8 * ch)) & 255;
                if (v < lo[ch]) lo[ch] = v;
                if (v > hi[ch]) hi[ch] = v;
            }
        }
        int widest = 0, spread = 0;
        for (int ch = 0; ch < 3; ch++)
            if ((hi[ch] - lo[ch]) * WEIGHT[ch] > spread) spread = (hi[ch] - lo[ch]) * WEIGHT[ch], widest = ch;
        return spread < 6 ? 0 : ((uint32_t)spread * count) << 2 | widest;
    };
    int boxes = 1;
    box[0] = (uint32_t)n << 16;
    score[0] = measure(0, n);
    while (boxes < 256) {
        int b = -1;
        uint32_t top = 0;
        for (int i = 0; i < boxes; i++)
            if ((score[i] >> 2) > top) top = score[i] >> 2, b = i;
        if (b < 0) break;
        const int start = box[b] & 0xFFFF, count = box[b] >> 16, shift = 16 - 8 * (score[b] & 3);
        uint32_t sum = 0;
        for (int i = start; i < start + count; i++) sum += (px[i] >> shift) & 255;
        const uint32_t mean = sum / count;
        int i = start, j = start + count - 1;  // values <= mean to the front
        while (i <= j) {
            if (((px[i] >> shift) & 255) <= mean) {
                i++;
            } else {
                uint32_t tmp = px[i];
                px[i] = px[j], px[j] = tmp;
                j--;
            }
        }
        const int left = i - start;
        if (left == 0 || left == count) {  // can't split further
            score[b] = 0;
            continue;
        }
        box[b] = (uint32_t)start | (uint32_t)left << 16;
        score[b] = measure(start, left);
        box[boxes] = (uint32_t)(start + left) | (uint32_t)(count - left) << 16;
        score[boxes] = measure(start + left, count - left);
        boxes++;
    }
    // Each box's average, then darkest first.
    volatile uint32_t* pal = artPalette(art);
    for (int b = 0; b < boxes; b++) {
        const int start = box[b] & 0xFFFF, count = box[b] >> 16;
        uint32_t r = 0, g = 0, bl = 0;
        for (int i = start; i < start + count; i++) {
            uint32_t p = px[i];
            r += (p >> 16) & 255, g += (p >> 8) & 255, bl += p & 255;
        }
        pal[b] = (r / count) << 16 | (g / count) << 8 | (bl / count);
    }
    auto luma = [](uint32_t p) { return 77 * ((p >> 16) & 255) + 150 * ((p >> 8) & 255) + 29 * (p & 255); };
    for (int i = 1; i < boxes; i++) {
        uint32_t p = pal[i];
        int j = i - 1;
        while (j >= 0 && luma(pal[j]) > luma(p)) pal[j + 1] = pal[j], j--;
        pal[j + 1] = p;
    }
    return boxes;
}

void fillPlaceholderArt(uint32_t* dst, const ScenePalette& pal) {
    // Darker than the background, so the white title on it reads.
    uint32_t c = toRGB332(lerp(pal.bgRGB, RGB{0, 0, 0}, 110)) * 0x01010101u;
    volatile uint32_t* w = dst;
    for (int i = 0; i < ART_WORDS; i++) w[i] = c;
}
