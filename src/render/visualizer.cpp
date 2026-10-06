#include "visualizer.h"

#include <math.h>
#include <string.h>

#include "color.h"
#include "config.h"
#include "draw.h"

namespace {

constexpr float K = DISPLAY_PIXEL_ASPECT;
constexpr float TAU = 6.2831853f;
constexpr int TOP = GLOW_LEVELS - 1;

struct Area {
    Canvas& c;
    int x0, y0, x1, y1, bottom;
    float cx, cy;
    bool cover(int x, int y) const { return x >= x0 && x < x1 && y >= y0 && y < y1; }
};

int keepFor(float dt, float perSecond) { return (int)(256 * expf(-dt * perSecond)); }

// The frame on screen, zoomed out from the cover's centre, turned (radians), softened (blur
// pulls the bilinear weights toward even: 0 sharp .. 7 soft) and kept at keep / 256. With
// nothing to feed back from, black.
void feedback(Area& a, bool fed, float zoom, float turn, int keep, int blur) {
    uint8_t** in = fed ? a.c.shown : nullptr;
    const float cs = cosf(turn) / zoom, sn = sinf(turn) / zoom;
    const int32_t stepX = (int32_t)(cs * 65536), stepY = (int32_t)(sn * K * 65536);
    // Pulling the weights toward even also moves the sample up to half a pixel on; start that
    // much earlier so the softening is centred and nothing drifts.
    const int32_t centre = (int32_t)(32768.0f * blur / (blur + 1));
    for (int y = 0; y < a.bottom && y < Canvas::H; y++) {
        uint8_t* out = a.c.rows[y];
        const float v = y - a.cy;
        int32_t xs = (int32_t)((a.cx - a.cx * cs - v * sn / K) * 65536) - centre;
        int32_t ys = (int32_t)((a.cy - a.cx * sn * K + v * cs) * 65536) - centre;
        const bool coverRow = y >= a.y0 && y < a.y1;
        for (int x = 0; x < Canvas::W; x++, xs += stepX, ys += stepY) {
            if (coverRow && x >= a.x0 && x < a.x1) continue;
            int value = 0;
            const int sx = xs >> 16, sy = ys >> 16;
            if (in && sx >= 0 && sy >= 0 && sx < Canvas::W - 1 && sy < a.bottom - 1 &&
                (sy + 1 < a.y0 || sy >= a.y1 || sx + 1 < a.x0 || sx >= a.x1)) {
                const int fx = (((xs >> 8) & 255) + 128 * blur) / (blur + 1);
                const int fy = (((ys >> 8) & 255) + 128 * blur) / (blur + 1);
                const uint8_t* r0 = in[sy] + sx;
                const uint8_t* r1 = in[sy + 1] + sx;
                const int p00 = r0[0] < GLOW_LEVELS ? r0[0] : 0, p01 = r0[1] < GLOW_LEVELS ? r0[1] : 0;
                const int p10 = r1[0] < GLOW_LEVELS ? r1[0] : 0, p11 = r1[1] < GLOW_LEVELS ? r1[1] : 0;
                const int top = p00 * 256 + (p01 - p00) * fx, low = p10 * 256 + (p11 - p10) * fx;
                value = ((top * 256 + (low - top) * fy) >> 16) * keep >> 8;
            }
            out[x] = (uint8_t)value;
        }
    }
}

// The song line's strip at the darkest, and soft toward it and around the cover, so neither
// edge is a hard line (text colours, past the glow ramp, stay as they are).
void finish(Area& a) {
    for (int y = a.bottom; y < Canvas::H; y++) memset(a.c.rows[y], 0, Canvas::W);
    for (int y = a.bottom - 10 < 0 ? 0 : a.bottom - 10; y < a.bottom && y < Canvas::H; y++)
        for (int x = 0; x < Canvas::W; x++)
            if (a.c.rows[y][x] < GLOW_LEVELS) a.c.rows[y][x] = (uint8_t)(a.c.rows[y][x] * (a.bottom - y) / 10);
    constexpr int EDGE = 8;
    for (int y = a.y0 - EDGE < 0 ? 0 : a.y0 - EDGE; y < a.y1 + EDGE && y < a.bottom; y++)
        for (int x = a.x0 - EDGE < 0 ? 0 : a.x0 - EDGE; x < a.x1 + EDGE && x < Canvas::W; x++) {
            int dist = a.x0 - x;
            if (x - a.x1 + 1 > dist) dist = x - a.x1 + 1;
            if (a.y0 - y > dist) dist = a.y0 - y;
            if (y - a.y1 + 1 > dist) dist = y - a.y1 + 1;
            if (dist > 0 && dist < EDGE && a.c.rows[y][x] < GLOW_LEVELS) a.c.rows[y][x] = (uint8_t)(a.c.rows[y][x] * dist / EDGE);
        }
}

void add(Area& a, int x, int y, int s) {
    if (x < 0 || x >= Canvas::W || y < 0 || y >= a.bottom || a.cover(x, y)) return;
    const int v = a.c.rows[y][x] + s;
    a.c.rows[y][x] = (uint8_t)(v < TOP ? v : TOP);
}

void raise(Area& a, int x, int y, int v) {  // at least v
    if (x < 0 || x >= Canvas::W || y < 0 || y >= a.bottom || a.cover(x, y)) return;
    if (a.c.rows[y][x] < v) a.c.rows[y][x] = (uint8_t)(v < TOP ? v : TOP);
}

void dab(Area& a, int x, int y, int s) {  // a soft 3x3 brush
    add(a, x, y, s);
    add(a, x - 1, y, s / 2), add(a, x + 1, y, s / 2), add(a, x, y - 1, s / 2), add(a, x, y + 1, s / 2);
    add(a, x - 1, y - 1, s / 4), add(a, x + 1, y - 1, s / 4), add(a, x - 1, y + 1, s / 4), add(a, x + 1, y + 1, s / 4);
}

void stroke(Area& a, int ax, int ay, int bx, int by, int s) {
    const int steps = abs(bx - ax) > abs(by - ay) ? abs(bx - ax) : abs(by - ay);
    for (int i = 0; i <= steps; i++) dab(a, ax + (steps ? (bx - ax) * i / steps : 0), ay + (steps ? (by - ay) * i / steps : 0), s);
}

void noteBeat(const VizInput& in, uint32_t ms, VizState& s) {
    if (in.beat > 0.9f && !s.beatHeld) {
        s.beats[s.nextBeat++ % 6] = ms | 1;
        s.beatHeld = true;
    }
    if (in.beat < 0.5f) s.beatHeld = false;
}

// ---- The styles ----

void glow(Area& a, float dt, const VizInput& in, VizState& s) {
    feedback(a, s.fed, expf(dt * (0.4f + 0.8f * in.bass)), dt * (0.5f + 1.0f * in.mid), keepFor(dt, 1.0f), 5);
    s.spin += dt * (0.8f + 1.6f * in.mid);
    const int strength = 46 + (int)(64 * in.beat) + (int)(26 * in.bass);
    for (int curve = 0; curve < 2; curve++) {
        const float r0 = 100 + 44 * in.bass + curve * 16, wobble = 0.22f + 0.08f * curve, wiggle = 0.16f * in.high;
        int px = 0, py = 0;
        for (int k = 0; k <= 180; k++) {
            const float t = k * (TAU / 180);
            const float r = r0 * (1 + wobble * fastSin(t * (3 + curve) + s.spin * (1 + curve)) + wiggle * fastSin(t * 9 - s.spin * 3));
            const float angle = t + s.spin * (0.6f - curve);
            const int nx = (int)(a.cx + r * fastCos(angle) / K), ny = (int)(a.cy + r * fastSin(angle));
            if (k > 0) stroke(a, px, py, nx, ny, strength);
            px = nx, py = ny;
        }
    }
}

// Light through the glass: a fine glow round what's lit in [x0, x1) x [y0, y1), which must
// sit a pixel inside the screen. Each row is read before it is written, so the glow comes from
// the segments alone.
void bloom(Area& a, int x0, int x1, int y0, int y1) {
    uint8_t above[Canvas::W], here[Canvas::W];
    memcpy(above + x0 - 1, a.c.rows[y0 - 1] + x0 - 1, x1 - x0 + 2);
    for (int y = y0; y < y1; y++) {
        uint8_t* row = a.c.rows[y];
        const uint8_t* below = a.c.rows[y + 1];
        memcpy(here + x0 - 1, row + x0 - 1, x1 - x0 + 2);
        for (int x = x0; x < x1; x++) {
            const int glow = (above[x - 1] + 2 * above[x] + above[x + 1] + 2 * (here[x - 1] + 2 * here[x] + here[x + 1]) +
                              below[x - 1] + 2 * below[x] + below[x + 1]) >> 4;
            int v = here[x] > glow / 2 ? here[x] : glow / 2;
            v += glow / 8;
            row[x] = (uint8_t)(v < TOP ? v : TOP);
        }
        memcpy(above + x0 - 1, here + x0 - 1, x1 - x0 + 2);
    }
}

// A 2000s receiver's fluorescent analyzer, split by the cover: the lows rise at the left, the
// highs at the right. Behind its dark filter glass only what is lit shows, each segment a soft
// pill of pale phosphor glowing a little through the glass. The bars move like a meter's
// needle, quick up and slow down, the top segment fading in rather than stepping, and the
// peaks hold, then fall away with a little gravity, gliding between segments.
void spectrum(Area& a, float dt, const VizInput& in, VizState& s) {
    constexpr int COLS = 8, CW = 3, CG = 2, SEG = 3, GAP = 1, PITCH = SEG + GAP;
    constexpr int WIDTH = COLS * (CW + CG) - CG, FROM_COVER = 6;
    static const uint8_t ROW[SEG] = {159, 255, 159}, COL[CW] = {128, 255, 128};  // the pill's shading
    const int bot = a.y1 - 12, n = (bot - a.y0 - 30) / PITCH;
    const int left = a.x0 - FROM_COVER - WIDTH, right = a.x1 + FROM_COVER;

    for (int y = 0; y < a.bottom && y < Canvas::H; y++) {  // dark glass
        uint8_t* row = a.c.rows[y];
        if (y >= a.y0 && y < a.y1) {
            memset(row, 0, a.x0);
            memset(row + a.x1, 0, Canvas::W - a.x1);
        } else {
            memset(row, 0, Canvas::W);
        }
    }
    auto segment = [&](int x, int k, float bright) {
        const int v = (int)(bright * TOP), y = bot - (k + 1) * PITCH + GAP;
        for (int r = 0; r < SEG; r++)
            for (int c = 0; c < CW; c++) raise(a, x + c, y + r, v * ROW[r] * COL[c] >> 16);
    };

    const float up = 1 - expf(-dt / 0.06f), down = 1 - expf(-dt / 0.40f);
    for (int i = 0; i < 16; i++) {
        const float v = in.band[i] / 255.0f;
        s.level[i] += (v - s.level[i]) * (v > s.level[i] ? up : down);
        if (s.level[i] >= s.peaks[i]) {
            s.peaks[i] = s.level[i], s.peakHold[i] = 1.2f, s.peakFall[i] = 0;
        } else if ((s.peakHold[i] -= dt) < 0) {
            s.peakFall[i] += 0.9f * dt;
            s.peaks[i] -= s.peakFall[i] * dt;
            if (s.peaks[i] < s.level[i]) s.peaks[i] = s.level[i];
        }
        const int x = i < COLS ? left + i * (CW + CG) : right + (i - COLS) * (CW + CG);
        const float lit = s.level[i] * n;
        const int full = (int)lit;
        for (int k = 0; k <= full && k < n; k++) segment(x, k, (0.62f + 0.3f * k / n) * (k < full ? 1 : lit - full));
        const float peak = s.peaks[i] * n < n - 1 ? s.peaks[i] * n : n - 1;  // in the segment above it
        if (peak > lit + 1.2f) {
            const int k = (int)peak;
            if (k < n) segment(x, k, 1 - (peak - k));
            if (k + 1 < n) segment(x, k + 1, peak - k);
        }
    }
    const int y0 = bot - n * PITCH;
    bloom(a, left - 1, left + WIDTH + 1, y0 - 1, bot + 1);
    bloom(a, right - 1, right + WIDTH + 1, y0 - 1, bot + 1);
    // The baseline, printed on the glass in grey.
    memset(a.c.rows[bot + 2] + left, GLOW_ARTIST + 1, WIDTH);
    memset(a.c.rows[bot + 2] + right, GLOW_ARTIST + 1, WIDTH);
}

void halo(Area& a, uint32_t ms, const VizInput& in, VizState& s) {
    noteBeat(in, ms, s);
    const float reach = 22 + 50 * in.bass, strength = 120 + 110 * in.bass;
    uint8_t fall[256];  // the glow by distance from the cover, in px
    for (int d = 0; d < 256; d++) fall[d] = (uint8_t)(strength * expf(-d / reach));
    float rings[6], weight[6];
    int n = 0;
    for (int i = 0; i < 6; i++) {
        const uint32_t age = ms - s.beats[i];
        if (!s.beats[i] || age > 1600) continue;
        rings[n] = 8 + age * 0.12f, weight[n++] = 190 * (1 - age / 1600.0f);
    }
    for (int y = 0; y < a.bottom; y++) {
        uint8_t* out = a.c.rows[y];
        const float dy = y < a.y0 ? a.y0 - y : (y >= a.y1 ? y - a.y1 + 1 : 0);
        for (int x = 0; x < Canvas::W; x++) {
            if (a.cover(x, y)) continue;
            const float dx = (x < a.x0 ? a.x0 - x : (x >= a.x1 ? x - a.x1 + 1 : 0)) * K;
            const float d = sqrtf(dx * dx + dy * dy);
            float v = fall[d < 255 ? (int)d : 255];
            for (int i = 0; i < n; i++) {
                const float off = fabsf(d - rings[i]);
                if (off < 4.5f) v += weight[i] * (1 - off / 4.5f);
            }
            out[x] = (uint8_t)(v < TOP ? v : TOP);
        }
    }
}

// Curtains of light: three wavy lower edges with light rising from each, brighter where the
// band under that part of the screen is loud (lows at the middle, highs out to the sides), and
// fine folds shimmering along them.
void aurora(Area& a, float dt, const VizInput& in, VizState& s) {
    feedback(a, s.fed, 1.0f, 0.0f, keepFor(dt, 5.0f), 4);  // a soft persistence
    s.phase += dt * (0.25f + 0.6f * in.mid);
    if (s.phase > 6283.0f) s.phase -= 6283.0f;
    for (int curtain = 0; curtain < 3; curtain++) {
        const float reach = 16 + 42 * in.bass + 8 * curtain;
        uint8_t rise[256];  // light above the edge, by distance (in quarters of `reach`, x4 below)
        for (int d = 0; d < 256; d++) rise[d] = (uint8_t)(255 * expf(-d / 64.0f));
        for (int x = 0; x < Canvas::W; x++) {
            const float at = fabsf(x - a.cx) / (Canvas::W * 0.5f) * 14.99f;  // between bands, not in steps
            const int b0 = (int)at;
            const float band = (in.band[b0] + (in.band[b0 + 1] - in.band[b0]) * (at - b0)) / 255.0f;
            const float edge = a.y1 - 18 - curtain * 36 + 16 * fastSin(x * K * 0.019f + s.phase * (1 + 0.3f * curtain) + curtain * 2.1f) +
                               6 * fastSin(x * K * 0.051f - s.phase * 1.7f);
            // Light rises further in some places than others, so the top edge is ragged.
            const float tall = reach * (0.75f + 0.25f * fastSin(x * K * 0.043f + s.phase * 1.1f + curtain * 2.0f));
            const float folds = 0.88f + 0.12f * fastSin(x * 0.19f + s.phase * 4.0f + curtain * 1.3f);
            const float bright = (70 + 140 * band + 30 * in.beat) * folds;
            const int yEdge = (int)edge, span = (int)(tall * 3);
            const float perRow = 64.0f / tall;
            for (int y = yEdge - span; y <= yEdge + 2; y++) {
                const int d = yEdge - y;
                const int q = (int)(d * perRow);
                const float v = d >= 0 ? bright * rise[q < 255 ? q : 255] / 255 : bright * (1 + d / 3.0f);
                if (v > 4) raise(a, x, y, (int)v);
            }
        }
    }
}

void lava(Area& a, float dt, const VizInput& in, VizState& s) {
    s.phase += dt * (0.25f + 1.6f * in.bass + 0.6f * in.mid);
    if (s.phase > 6283.0f) s.phase -= 6283.0f;
    const float contrast = 0.7f + 0.5f * in.mid, lift = 30 * in.beat + 20 * in.bass;
    int8_t across[Canvas::W];
    for (int x = 0; x < Canvas::W; x++) across[x] = (int8_t)(127 * fastSin(x * K * 0.028f + s.phase));
    for (int y = 0; y < a.bottom; y++) {
        uint8_t* out = a.c.rows[y];
        const float down = 40 * fastSin(y * 0.042f - s.phase * 0.8f);
        for (int x = 0; x < Canvas::W; x++) {
            if (a.cover(x, y)) continue;
            const float diag = 40 * fastSin((x * K + y) * 0.031f + s.phase * 1.3f);
            const float v = 105 + lift + contrast * (across[x] * (45.0f / 127) + down + diag);
            out[x] = (uint8_t)(v < 0 ? 0 : (v > TOP ? TOP : v));
        }
    }
}

}  // namespace

void drawVisualizer(int style, Canvas& c, int x0, int y0, int x1, int y1, int bottom, float dt, uint32_t ms,
                    const VizInput& in, VizState& s) {
    Area a{c, x0, y0, x1, y1, bottom, (x0 + x1) * 0.5f, (y0 + y1) * 0.5f};
    if (!c.shown) s.fed = false;
    switch (style) {
        case VIZ_SPECTRUM: spectrum(a, dt, in, s); break;
        case VIZ_HALO: halo(a, ms, in, s); break;
        case VIZ_AURORA: aurora(a, dt, in, s); break;
        case VIZ_LAVA: lava(a, dt, in, s); break;
        default: glow(a, dt, in, s); break;
    }
    if (s.spin > 6283.0f) s.spin -= 6283.0f;
    finish(a);
    s.fed = true;
}
