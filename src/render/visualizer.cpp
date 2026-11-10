#include "visualizer.h"

#include <math.h>
#include <string.h>

#include "color.h"
#include "config.h"
#include "draw.h"
#include "wordram.h"

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

// A flow field, as G-Force calls it: where each point of the next frame comes from in the one
// on screen, given at MESH x MESH cells' corners (16.16 px) and filled in between.
constexpr int MESH = 8;
struct Flow {
    int32_t x[MESH + 1][MESH + 1], y[MESH + 1][MESH + 1];
};
// In word-only RAM (all int32): ordinary RAM is short, and it's too big for the render stack.
Flow& flowMesh() {
    static Flow* f = (Flow*)wordRam(sizeof(Flow));
    return *f;
}

// The frame on screen warped by `f`, softened (blur pulls the bilinear weights toward even:
// 0 sharp .. 7 soft) and kept at keep / 256; with nothing to feed back from, black. Every
// other pixel across, on every other row, is sampled and the rest blended from their
// neighbours: a quarter of the work, and no finer than composite or interlace would show.
void feedback(Area& a, bool fed, const Flow& f, int keep, int blur) {
    uint8_t** in = fed ? a.c.shown : nullptr;
    // Pulling the weights toward even also moves the sample up to half a pixel on; start that
    // much earlier so the softening is centred and nothing drifts.
    const int32_t centre = (int32_t)(32768.0f * blur / (blur + 1));
    const int soft = 65536 / (blur + 1), softAt = 128 * blur * soft;  // w -> (w + 128 blur) / (blur + 1), x65536
    const int bottom = a.bottom < Canvas::H ? a.bottom : Canvas::H;
    constexpr int CW = Canvas::W / MESH, CH = Canvas::H / MESH;
    auto sample = [&](int32_t xs, int32_t ys) {
        xs -= centre, ys -= centre;
        const int sx = xs >> 16, sy = ys >> 16;
        int value = 0;
        if ((unsigned)sx < Canvas::W - 1 && (unsigned)sy < (unsigned)(bottom - 1) &&
            (sy + 1 < a.y0 || sy >= a.y1 || sx + 1 < a.x0 || sx >= a.x1)) {
            const int fx = (((xs >> 8) & 255) * soft + softAt) >> 16, fy = (((ys >> 8) & 255) * soft + softAt) >> 16;
            const uint8_t* r0 = in[sy] + sx;
            const uint8_t* r1 = in[sy + 1] + sx;
            const int p00 = r0[0] < GLOW_LEVELS ? r0[0] : 0, p01 = r0[1] < GLOW_LEVELS ? r0[1] : 0;
            const int p10 = r1[0] < GLOW_LEVELS ? r1[0] : 0, p11 = r1[1] < GLOW_LEVELS ? r1[1] : 0;
            const int top = p00 * 256 + (p01 - p00) * fx, low = p10 * 256 + (p11 - p10) * fx;
            value = ((top * 256 + (low - top) * fy) >> 16) * keep >> 8;
        }
        return value;
    };
    int32_t rowX[MESH + 1], rowY[MESH + 1];  // this row's sources at the mesh columns
    // Across a row, cell by cell: within one the source steps evenly, so it's two adds a sample.
    auto sampleSpan = [&](uint8_t* out, int x0, int x1) {
        if (x0 >= x1) return;
        int prev = 0, x = x0;
        for (int c = x0 / CW; x < x1; c++) {
            const int cell = c < MESH ? c : MESH - 1, end = (c + 1) * CW < x1 ? (c + 1) * CW : x1;
            const int32_t sx = (rowX[cell + 1] - rowX[cell]) / CW, sy = (rowY[cell + 1] - rowY[cell]) / CW;
            int32_t xs = rowX[cell] + sx * (x - cell * CW), ys = rowY[cell] + sy * (x - cell * CW);
            for (; x < end; x += 2, xs += 2 * sx, ys += 2 * sy) {
                const int v = sample(xs, ys);
                if (x > x0) out[x - 1] = (uint8_t)((prev + v) >> 1);
                out[x] = (uint8_t)v;
                prev = v;
            }
        }
        if ((x1 - x0) % 2 == 0) out[x1 - 1] = (uint8_t)prev;
    };
    auto coverRow = [&](int y) { return y >= a.y0 && y < a.y1; };
    // The stretches of row y off the cover: [from[i], to[i]) for i < the count returned.
    auto spans = [&](int y, int* from, int* to) {
        if (!coverRow(y)) return from[0] = 0, to[0] = Canvas::W, 1;
        return from[0] = 0, to[0] = a.x0, from[1] = a.x1, to[1] = Canvas::W, 2;
    };
    int from[2], to[2];
    if (!in) {
        for (int y = 0; y < bottom; y++)
            for (int i = 0, n = spans(y, from, to); i < n; i++) memset(a.c.rows[y] + from[i], 0, to[i] - from[i]);
        return;
    }
    auto sampleRow = [&](int y) {
        const int r = y / CH < MESH ? y / CH : MESH - 1, t = y - r * CH;
        for (int c = 0; c <= MESH; c++) {
            rowX[c] = f.x[r][c] + (int32_t)((int64_t)(f.x[r + 1][c] - f.x[r][c]) * t / CH);
            rowY[c] = f.y[r][c] + (int32_t)((int64_t)(f.y[r + 1][c] - f.y[r][c]) * t / CH);
        }
        for (int i = 0, n = spans(y, from, to); i < n; i++) sampleSpan(a.c.rows[y], from[i], to[i]);
    };
    // Odd rows between two sampled ones: their average, unless the cover is in the way.
    auto blendRow = [&](int y) {
        const uint8_t *up = a.c.rows[y - 1], *down = a.c.rows[y + 1];
        uint8_t* out = a.c.rows[y];
        for (int i = 0, n = spans(y, from, to); i < n; i++) {
            const int x0 = from[i], x1 = to[i];
            if (x1 > a.x0 && x0 < a.x1 && (coverRow(y - 1) || coverRow(y + 1))) {
                const int r = y / CH < MESH ? y / CH : MESH - 1, t = y - r * CH;
                for (int c = 0; c <= MESH; c++) {
                    rowX[c] = f.x[r][c] + (int32_t)((int64_t)(f.x[r + 1][c] - f.x[r][c]) * t / CH);
                    rowY[c] = f.y[r][c] + (int32_t)((int64_t)(f.y[r + 1][c] - f.y[r][c]) * t / CH);
                }
                sampleSpan(out, x0, x1);
                continue;
            }
            for (int x = x0; x < x1; x++) out[x] = (uint8_t)((up[x] + down[x] + 1) >> 1);
        }
    };
    for (int y = 0; y < bottom; y += 2) {
        sampleRow(y);
        if (y >= 2) blendRow(y - 1);
    }
    if (bottom % 2 == 0) sampleRow(bottom - 1);  // the last row has nothing below it
}

// The song line's strip at the darkest, and soft toward it and around the cover, so neither
// edge is a hard line (text colours, past the glow ramp, stay as they are).
void finish(Area& a) {
    for (int y = a.bottom; y < Canvas::H; y++) memset(a.c.rows[y], 0, Canvas::W);
    for (int y = a.bottom - 10 < 0 ? 0 : a.bottom - 10; y < a.bottom && y < Canvas::H; y++)
        for (int x = 0, f = (a.bottom - y) * 256 / 10; x < Canvas::W; x++)
            if (a.c.rows[y][x] < GLOW_LEVELS) a.c.rows[y][x] = (uint8_t)(a.c.rows[y][x] * f >> 8);
    constexpr int EDGE = 8;
    auto fade = [&](int x, int y) {
        int dist = a.x0 - x;
        if (x - a.x1 + 1 > dist) dist = x - a.x1 + 1;
        if (a.y0 - y > dist) dist = a.y0 - y;
        if (y - a.y1 + 1 > dist) dist = y - a.y1 + 1;
        uint8_t& p = a.c.rows[y][x];
        if (dist > 0 && dist < EDGE && p < GLOW_LEVELS) p = (uint8_t)(p * dist >> 3);  // EDGE is 8
    };
    const int left = a.x0 - EDGE < 0 ? 0 : a.x0 - EDGE, right = a.x1 + EDGE < Canvas::W ? a.x1 + EDGE : Canvas::W;
    for (int y = a.y0 - EDGE < 0 ? 0 : a.y0 - EDGE; y < a.y1 + EDGE && y < a.bottom; y++) {
        if (y >= a.y0 && y < a.y1) {  // beside the cover: just the two strips
            for (int x = left; x < a.x0; x++) fade(x, y);
            for (int x = a.x1; x < right; x++) fade(x, y);
        } else {
            for (int x = left; x < right; x++) fade(x, y);
        }
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
    if (x >= 1 && x < Canvas::W - 1 && y >= 1 && y < a.bottom - 1 && y < Canvas::H - 1 &&
        (x + 1 < a.x0 || x - 1 >= a.x1 || y + 1 < a.y0 || y - 1 >= a.y1)) {  // clear of the edges and the cover
        for (int j = -1; j <= 1; j++) {
            uint8_t* row = a.c.rows[y + j] + x;
            for (int i = -1; i <= 1; i++) {
                const int v = row[i] + (i && j ? s / 4 : (i || j ? s / 2 : s));
                row[i] = (uint8_t)(v < TOP ? v : TOP);
            }
        }
        return;
    }
    add(a, x, y, s);
    add(a, x - 1, y, s / 2), add(a, x + 1, y, s / 2), add(a, x, y - 1, s / 2), add(a, x, y + 1, s / 2);
    add(a, x - 1, y - 1, s / 4), add(a, x + 1, y - 1, s / 4), add(a, x - 1, y + 1, s / 4), add(a, x + 1, y + 1, s / 4);
}

void stroke(Area& a, int ax, int ay, int bx, int by, int s) {  // a dab every 2 px: the brush is 3 wide
    const int steps = ((abs(bx - ax) > abs(by - ay) ? abs(bx - ax) : abs(by - ay)) + 1) / 2;
    for (int i = 0; i <= steps; i++) dab(a, ax + (steps ? (bx - ax) * i / steps : 0), ay + (steps ? (by - ay) * i / steps : 0), s);
}

void noteBeat(const VizInput& in, uint32_t ms, VizState& s) {
    if (in.beat > 0.6f && !s.beatHeld) {  // 0.6: frames sample the beat after its peak has started to fall
        s.beats[s.nextBeat++ % 6] = ms | 1;
        s.beatHeld = true;
    }
    if (in.beat < 0.35f) s.beatHeld = false;
}

// ---- The styles ----

float smoothEnergy(const VizInput& in, float dt, VizState& s);

// The waveform at u (0..1 along its 64 samples), -1..1, between samples.
inline float waveAt(const VizInput& in, float u) {
    const float at = u * 62.999f;
    const int i = (int)at;
    return (in.wave[i] + (in.wave[i + 1] - in.wave[i]) * (at - i)) * (1 / 127.0f);
}

// After the classic iTunes visualizer (G-Force): the sound's own waveform drawn into a picture
// that flows outward and turns, so each frame's lines rush out past the edges like a tunnel
// toward you. The wave shape changes every 15 s, or on a big transient: the waveform as a ring
// round the cover, along spinning spiral arms, as oscilloscope traces, or riding lobed loops.
// Louder music flows faster, turns more and draws bigger and brighter.
void glow(Area& a, float dt, uint32_t ms, const VizInput& in, VizState& s) {
    const int beatsBefore = s.nextBeat;
    noteBeat(in, ms, s);
    const float e = smoothEnergy(in, dt, s);
    s.slow += (e - s.slow) * (1 - expf(-dt / 6));
    s.phase += dt * (0.3f + 1.2f * e);
    if (s.phase > 6283.0f) s.phase -= 6283.0f;
    s.spin += dt * (0.4f + 1.5f * e + 0.6f * in.mid);
    if (s.spin > 6283.0f) s.spin -= 6283.0f;
    s.shapeTime += dt;
    if (s.shapeTime > 15 || (s.nextBeat != beatsBefore && e - s.slow > 0.15f && s.shapeTime > 4)) {
        s.lastShape = s.mode;
        s.mode = (s.mode + 1) % 4;
        s.shapeTime = 0;
    }

    // The flow: out from the middle (sources nearer it), turning more toward the edges, with a
    // ripple running through the pull.
    Flow& flow = flowMesh();
    const float push = 1 - dt * (0.3f + 1.1f * e + 0.9f * in.beat);
    const float turn = dt * (0.2f + 0.8f * in.mid) * fastSin(s.phase * 0.23f + 1);
    for (int j = 0; j <= MESH; j++)
        for (int i = 0; i <= MESH; i++) {
            const float u = (i * (Canvas::W / MESH) - a.cx) * K, v = j * (Canvas::H / MESH) - a.cy;
            const float r = sqrtf(u * u + v * v);
            const float z = push * (1 + 0.012f * fastSin(r * 0.08f - s.phase * 3));
            const float th = turn * (0.4f + r / 160);
            const float c = fastCos(th) * z, sn = fastSin(th) * z;
            flow.x[j][i] = (int32_t)((a.cx + (u * c - v * sn) / K) * 65536);
            flow.y[j][i] = (int32_t)((a.cy + u * sn + v * c) * 65536);
        }
    feedback(a, s.fed, flow, keepFor(dt, 1.2f - 0.5f * e), 1);

    const float amp = 16 + 34 * e + 10 * in.beat;
    const int bright = 110 + (int)(90 * e) + (int)(90 * in.beat);
    auto shape = [&](int which, int level) {
        int px = 0, py = 0;
        auto to = [&](int k, float u, float v) {  // u, v: from the cover's centre, u across (x K)
            const int nx = (int)(a.cx + u / K), ny = (int)(a.cy + v);
            if (k > 0) stroke(a, px, py, nx, ny, level);
            px = nx, py = ny;
        };
        if (which == 0) {  // a ring round the cover, its radius the waveform (mirrored, so it closes)
            constexpr int N = 128;
            for (int k = 0; k <= N; k++) {
                const float t = k * (TAU / N), w = waveAt(in, k < N / 2 ? 2.0f * k / N : 2.0f * (N - k) / N);
                const float r = 108 + amp * w;
                to(k, r * fastCos(t + s.spin), r * fastSin(t + s.spin));
            }
        } else if (which == 1) {  // three spiral arms turning, the waveform across each
            for (int arm = 0; arm < 3; arm++) {
                const float base = s.spin * 1.3f + arm * (TAU / 3);
                for (int k = 0; k <= 48; k++) {
                    const float f = k / 48.0f, r = 96 + 110 * f, t = base + f * 1.6f;
                    const float off = amp * 0.7f * waveAt(in, f);
                    to(k, r * fastCos(t) - off * fastSin(t), r * fastSin(t) + off * fastCos(t));
                }
            }
        } else if (which == 2) {  // oscilloscope traces: above and below the cover, and either side
            for (int side = 0; side < 4; side++) {
                for (int k = 0; k <= 48; k++) {
                    const float f = k / 48.0f, w = amp * 0.8f * waveAt(in, f);
                    if (side < 2) {
                        const float v = (side ? 1 : -1) * 100, u = (f - 0.5f) * 300;
                        to(k, u, v + w);
                    } else {
                        const float u = (side == 3 ? 1 : -1) * 120, v = (f - 0.5f) * 200;
                        to(k, u + w, v);
                    }
                }
            }
        } else {  // lobed loops spinning round the cover, the waveform riding them
            for (int curve = 0; curve < 2; curve++) {
                const float r0 = 100 + 30 * e + curve * 16, lobes = 3 + curve;
                for (int k = 0; k <= 128; k++) {
                    const float t = k * (TAU / 128);
                    const float w = waveAt(in, k < 64 ? k / 64.0f : (128 - k) / 64.0f);
                    const float r = r0 * (1 + (0.16f + 0.1f * e) * fastSin(t * lobes + s.spin * (2 + curve))) + amp * 0.5f * w;
                    const float ang = t + s.spin * (curve ? -1.2f : 1.4f);
                    to(k, r * fastCos(ang), r * fastSin(ang));
                }
            }
        }
    };
    if (s.shapeTime < 1.5f) shape(s.lastShape, (int)(bright * (1 - s.shapeTime / 1.5f)));  // the last one fading
    shape(s.mode, s.shapeTime < 1.5f ? (int)(bright * s.shapeTime / 1.5f) : bright);
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

// Every pixel off the cover, row by row: f(x, y, out row).
template <typename F>
inline void offCover(Area& a, F f) {
    for (int y = 0; y < a.bottom && y < Canvas::H; y++) {
        uint8_t* out = a.c.rows[y];
        if (y >= a.y0 && y < a.y1) {
            for (int x = 0; x < a.x0; x++) f(x, y, out);
            for (int x = a.x1; x < Canvas::W; x++) f(x, y, out);
        } else {
            for (int x = 0; x < Canvas::W; x++) f(x, y, out);
        }
    }
}

void halo(Area& a, uint32_t ms, const VizInput& in, VizState& s) {
    noteBeat(in, ms, s);
    const float reach = 22 + 50 * in.bass, strength = 120 + 110 * in.bass;
    float rings[6], weight[6];
    int n = 0;
    for (int i = 0; i < 6; i++) {
        const uint32_t age = ms - s.beats[i];
        if (!s.beats[i] || age > 1600) continue;
        rings[n] = 8 + age * 0.12f, weight[n++] = 190 * (1 - age / 1600.0f);
    }
    // The glow by distance from the cover, rings and all, in half pixels.
    uint8_t glow[512];
    for (int h = 0; h < 512; h++) {
        const float d = h * 0.5f;
        float v = strength * expf(-d / reach);
        for (int i = 0; i < n; i++) {
            const float off = fabsf(d - rings[i]);
            if (off < 4.5f) v += weight[i] * (1 - off / 4.5f);
        }
        glow[h] = (uint8_t)(v < TOP ? v : TOP);
    }
    // Distance (half pixels) by how far across from the cover, for the current row's distance
    // down: square roots only for the few rows above and below it.
    constexpr int ACROSS = 128;
    uint16_t dist[ACROSS];
    int distFor = -1;
    offCover(a, [&](int x, int y, uint8_t* out) {
        const int dy = y < a.y0 ? a.y0 - y : (y >= a.y1 ? y - a.y1 + 1 : 0);
        if (dy != distFor) {
            for (int dx = 0; dx < ACROSS; dx++) {
                const float across = dx * K, d = dy ? sqrtf(across * across + dy * dy) : across;
                dist[dx] = (uint16_t)(d * 2 < 511 ? d * 2 : 511);
            }
            distFor = dy;
        }
        int dx = x < a.x0 ? a.x0 - x : (x >= a.x1 ? x - a.x1 + 1 : 0);
        if (dx >= ACROSS) dx = ACROSS - 1;
        out[x] = glow[dist[dx]];
    });
}

void lava(Area& a, float dt, const VizInput& in, VizState& s) {
    s.phase += dt * (0.25f + 1.6f * in.bass + 0.6f * in.mid);
    if (s.phase > 6283.0f) s.phase -= 6283.0f;
    const float contrast = 0.7f + 0.5f * in.mid, lift = 30 * in.beat + 20 * in.bass;
    // Three waves, across, down and diagonal; the first and last tabled for the frame (the
    // diagonal by x * K + y), already scaled, so a pixel is two lookups and an add.
    constexpr int KQ = (int)(K * 256), DIAGONAL = (Canvas::W * KQ >> 8) + Canvas::H + 1;
    int8_t across[Canvas::W], diag[DIAGONAL];
    for (int x = 0; x < Canvas::W; x++) across[x] = (int8_t)(contrast * 45 * fastSin(x * K * 0.028f + s.phase));
    for (int i = 0; i < DIAGONAL; i++) diag[i] = (int8_t)(contrast * 40 * fastSin(i * 0.031f + s.phase * 1.3f));
    int base = 0, baseFor = -1;
    offCover(a, [&](int x, int y, uint8_t* out) {
        if (y != baseFor) base = (int)(105 + lift + contrast * 40 * fastSin(y * 0.042f - s.phase * 0.8f)), baseFor = y;
        const int v = base + across[x] + diag[(x * KQ >> 8) + y];
        out[x] = (uint8_t)(v < 0 ? 0 : (v > TOP ? TOP : v));
    });
}

// Every pixel off the cover, worked out every other pixel on every other row by sample(x, y)
// and filled in between (no finer than composite or interlace would show anyway).
template <typename F>
void fillHalf(Area& a, F sample) {
    const int bottom = a.bottom < Canvas::H ? a.bottom : Canvas::H;
    auto coverRow = [&](int y) { return y >= a.y0 && y < a.y1; };
    int from[2], to[2];
    auto spans = [&](int y) {
        if (!coverRow(y)) return from[0] = 0, to[0] = Canvas::W, 1;
        return from[0] = 0, to[0] = a.x0, from[1] = a.x1, to[1] = Canvas::W, 2;
    };
    auto sampleSpan = [&](int y, int x0, int x1) {
        if (x0 >= x1) return;
        uint8_t* out = a.c.rows[y];
        int prev = sample(x0, y);
        out[x0] = (uint8_t)prev;
        int x = x0 + 2;
        for (; x < x1; x += 2) {
            const int next = sample(x, y);
            out[x - 1] = (uint8_t)((prev + next) >> 1);
            out[x] = (uint8_t)next;
            prev = next;
        }
        if (x == x1) out[x1 - 1] = (uint8_t)prev;
    };
    for (int y = 0; y < bottom; y += 2) {
        for (int i = 0, n = spans(y); i < n; i++) sampleSpan(y, from[i], to[i]);
        if (y < 2) continue;
        const int b = y - 1;  // the row between: their average, unless the cover is in the way
        const uint8_t *up = a.c.rows[b - 1], *down = a.c.rows[b + 1];
        for (int i = 0, n = spans(b); i < n; i++) {
            if (to[i] > a.x0 && from[i] < a.x1 && (coverRow(b - 1) || coverRow(b + 1))) {
                sampleSpan(b, from[i], to[i]);
                continue;
            }
            uint8_t* out = a.c.rows[b];
            for (int x = from[i]; x < to[i]; x++) out[x] = (uint8_t)((up[x] + down[x] + 1) >> 1);
        }
    }
    if (bottom % 2 == 0)
        for (int i = 0, n = spans(bottom - 1); i < n; i++) sampleSpan(bottom - 1, from[i], to[i]);
}

float smoothEnergy(const VizInput& in, float dt, VizState& s) {
    const float loud = (in.bass + in.mid + in.high) / 3;
    s.energy += (loud - s.energy) * (1 - expf(-dt / (loud > s.energy ? 0.4f : 1.5f)));
    return s.energy < 1 ? s.energy : 1;
}

// A thin line at `level`, with a faint pixel either side so it glows a little.
void laser(Area& a, int ax, int ay, int bx, int by, int level) {
    const int dx = abs(bx - ax), dy = abs(by - ay), sx = ax < bx ? 1 : -1, sy = ay < by ? 1 : -1;
    const bool steep = dy > dx;
    int err = (steep ? dy : dx) / 2;
    for (int n = steep ? dy : dx; n >= 0; n--) {
        if (ax >= -1 && ax <= Canvas::W && ay >= -1 && ay <= a.bottom) {
            raise(a, ax, ay, level);
            if (steep) raise(a, ax - 1, ay, level / 3), raise(a, ax + 1, ay, level / 3);
            else raise(a, ax, ay - 1, level / 3), raise(a, ax, ay + 1, level / 3);
        }
        if (steep) {
            ay += sy;
            if ((err -= dx) < 0) ax += sx, err += dy;
        } else {
            ax += sx;
            if ((err -= dy) < 0) ay += sy, err += dx;
        }
    }
}

// 1 / sqrt(x) to about 0.2% (one Newton step): sqrtf and division are slow in software here.
inline float invSqrt(float x) {
    union {
        float f;
        uint32_t i;
    } u = {x};
    u.i = 0x5f3759df - (u.i >> 1);
    return u.f * (1.5f - 0.5f * x * u.f * u.f);
}

// x - floor(x) without floorf.
inline float fract(float x) {
    x -= (float)(int)x;
    return x < 0 ? x + 1.0f : x;
}

// A techno rave: a warehouse rig locked to the kick. Every bar (four beats) it cuts to the next
// look: a square tunnel (frames of light leaving the cover on the eighths), laser sheets scanning
// up and down on the beat, an LED wall chasing in sixteenths, then fans from the floor corners. Through all of
// them a hard strobe on each kick, a ring of light leaving the cover, and haze streaking out.
void rave(Area& a, float dt, uint32_t ms, const VizInput& in, VizState& s) {
    noteBeat(in, ms, s);
    const float e = smoothEnergy(in, dt, s);
    s.phase += dt * (0.6f + 1.8f * e);
    if (s.phase > 6283.0f) s.phase -= 6283.0f;
    const float push = 1 - dt * (0.8f + 1.4f * e);  // sources nearer the middle: everything rushes out
    Flow& flow = flowMesh();
    for (int j = 0; j <= MESH; j++)
        for (int i = 0; i <= MESH; i++) {
            flow.x[j][i] = (int32_t)((a.cx + (i * (Canvas::W / MESH) - a.cx) * push) * 65536);
            flow.y[j][i] = (int32_t)((a.cy + (j * (Canvas::H / MESH) - a.cy) * push) * 65536);
        }
    feedback(a, s.fed, flow, keepFor(dt, s.step >= 0 && (s.step / 16) % 4 == 0 ? 2.0f : 9.0f - 4.0f * e), 1);  // the tunnel keeps its frames

    // Where we are in the beat: the last kick, and the beat's length from the last two.
    const int n = s.nextBeat;
    const uint32_t last = n ? s.beats[(n - 1) % 6] : 0, prev = n > 1 ? s.beats[(n - 2) % 6] : 0;
    uint32_t period = last && prev ? last - prev : 500;
    if (period < 280 || period > 900) period = 500;
    // Between kicks (or through a breakdown) it keeps counting at the last tempo.
    const uint32_t since = last ? ms - last : ms;
    const int counted = n + (int)(since / period);
    const float beatPhase = (float)(since % period) / period;  // 0..1 through this beat
    const int look = (counted / 4) % 4, beatInBar = counted % 4;
    const int hi = TOP, lit = 150 + (int)(70 * e);

    if (since < 70) {  // the strobe: hard and short, everything off the cover
        const int flash = (int)((70 + 120 * e) * (1 - since / 70.0f));
        offCover(a, [&](int x, int, uint8_t* out) {
            if (out[x] < flash) out[x] = (uint8_t)flash;
        });
    }
    if (since < 40) {  // the kick's ring, just off the cover; the flow carries it out
        const int d = 5;
        for (int x = a.x0 - d; x < a.x1 + d; x++) raise(a, x, a.y0 - d, lit), raise(a, x, a.y1 + d - 1, lit);
        for (int y = a.y0 - d; y < a.y1 + d; y++) raise(a, a.x0 - d, y, lit), raise(a, a.x1 + d - 1, y, lit);
    }

    const int sixteenth = counted * 4 + (int)(beatPhase * 4);
    const bool newStep = sixteenth != s.step;
    s.step = sixteenth;
    if (look == 0) {  // square tunnel: a frame the cover's shape on every eighth (the sixteenths too
                      // when it's driving), the downbeat brightest; the flow carries them out
        if (newStep && ((sixteenth & 1) == 0 || e > 0.6f)) {
            const int d = 9, lv = (sixteenth & 15) == 0 ? hi : ((sixteenth & 1) ? lit / 2 : lit);  // past the cover's soft edge
            for (int x = a.x0 - d; x < a.x1 + d; x++)
                for (int t = 0; t < 2; t++) raise(a, x, a.y0 - d - t, lv), raise(a, x, a.y1 + d - 1 + t, lv);
            for (int y = a.y0 - d; y < a.y1 + d; y++)
                for (int t = 0; t < 2; t++) raise(a, a.x0 - d - t, y, lv), raise(a, a.x1 + d - 1 + t, y, lv);
        }
    } else if (look == 1) {  // laser sheets scanning: down on one beat, up on the next
        const float t = beatInBar & 1 ? 1 - beatPhase : beatPhase;
        const int sheets = e > 0.5f ? 3 : 2;
        for (int k = 0; k < sheets; k++) {
            const int y = (int)(fract(t + k / (float)sheets) * (a.bottom - 2)) + 1;
            laser(a, 0, y, Canvas::W - 1, y, hi);
        }
        if (e > 0.6f) {  // and a vertical one sweeping across, when it's driving
            const int x = (int)(fract(t * 2) * (Canvas::W - 1));
            laser(a, x, 0, x, a.bottom - 1, lit);
        }
    } else if (look == 2) {  // LED wall: bars stepping across in sixteenths, brighter with the hats
        const int step = (int)(beatPhase * 4) + beatInBar * 4;  // sixteenth of the bar
        const int bars = 8, w = Canvas::W / bars;
        const int level = (int)(120 + 110 * in.high);
        for (int k = 0; k < bars; k++) {
            const bool on = ((k + step) % 4 == 0) || (e > 0.55f && (k * 3 + step) % 8 == 0);
            if (!on) continue;
            for (int y = 0; y < a.bottom; y += 2)  // every other line: an LED panel's rows
                for (int x = k * w + 1; x < (k + 1) * w - 1; x++) raise(a, x, y, level);
        }
    } else {  // fans from the floor corners, snapping to a new spread each beat
        const int beams = 3 + (int)(5 * e);
        const float spread = 0.5f + 0.35f * fastSin(counted * 2.3f);
        for (int fan = 0; fan < 2; fan++) {
            const int ox = fan ? Canvas::W - 1 : 0, oy = a.bottom - 1;
            const float aim = fan ? -2.25f : -0.89f;
            const float sweep = 0.45f * fastSin(s.phase * (1.3f + 0.4f * fan) + counted * 1.7f) * (fan ? -1 : 1);
            for (int b = 0; b < beams; b++) {
                const float ang = aim + sweep + (b - (beams - 1) * 0.5f) * spread / beams;
                laser(a, ox, oy, ox + (int)(420 * fastCos(ang) / K), oy + (int)(420 * fastSin(ang)), lit);
            }
        }
    }
}

// log2 to about 1%, for the fractal's smooth colouring.
inline float fastLog2(float x) {
    union {
        float f;
        uint32_t i;
    } u = {x};
    const float e = (float)((int)(u.i >> 23) - 127);
    u.i = (u.i & 0x7FFFFF) | 0x3F800000;
    return e + (-0.34484843f * u.f + 2.02466578f) * u.f - 0.67487759f;
}

// After Aleph: an escape-time fractal worked out on a table of rings and spokes for one
// thirty-degree wedge, mirrored twelvefold round the cover. It dives in and out, drifts through
// a path of shapes faster as the music gets louder, and breathes its zoom with the bass. Every
// beat punches the zoom and snaps it round (each the other way); a big transient (a drop, a hit
// after a quiet stretch) flashes it and cuts to another family: Julia, cubic, burning ship,
// tricorn.
constexpr int FR = 48, FS = 24;               // rings, spokes
constexpr float FR_MIN = 60, FR_MAX = 215;    // the rings' radii, px across (x K)
// The table, four levels to a word, in word-only RAM (ordinary RAM is short).
volatile uint32_t* fractalTab() {
    static volatile uint32_t* t = (volatile uint32_t*)wordRam(FR * FS);
    return t;
}

void fractal(Area& a, float dt, uint32_t ms, const VizInput& in, VizState& s) {
    static const float PATHS[4][4][2] = {
        {{-0.8f, 0.156f}, {-0.4f, 0.6f}, {-0.123f, 0.745f}, {0.285f, 0.01f}},       // z^2 + c
        {{0.50f, 0.00f}, {0.70f, -0.30f}, {0.50f, -0.70f}, {0.30f, -0.90f}},        // z^3 + c
        {{0.30f, -0.20f}, {0.40f, -0.50f}, {-0.10f, 0.30f}, {0.60f, -0.80f}},       // burning ship
        {{-0.95f, 0.25f}, {-0.30f, 0.80f}, {0.30f, -0.70f}, {-1.10f, 0.0f}},       // tricorn
    };
    const int beatsBefore = s.nextBeat;
    noteBeat(in, ms, s);
    const bool beat = s.nextBeat != beatsBefore;
    const float e = smoothEnergy(in, dt, s);
    s.slow += (e - s.slow) * (1 - expf(-dt / 6));
    if (beat) {
        s.punch = 1;
        s.turnKick += (s.nextBeat & 1 ? 1 : -1) * (0.12f + 0.3f * e);
        if (e - s.slow > 0.15f && ms - s.lastDrop > 4000) {  // a transient: flash, and a new family
            s.lastDrop = ms | 1;
            s.mode = (s.mode + 1 + (s.nextBeat & 1)) % 4;
            s.flash = 1;
            s.phase += 1.5f;
        }
    }
    s.punch *= expf(-dt * 5);
    s.flash *= expf(-dt * 3);
    const float kick = s.turnKick * (1 - expf(-dt * 12));
    s.turnKick -= kick;
    s.spin += dt * (0.06f + 0.5f * in.mid) + kick;
    if (s.spin > 6283.0f) s.spin -= 6283.0f;
    s.phase += dt * (0.04f + 0.35f * e);
    if (s.phase > 400.0f) s.phase -= 400.0f;
    s.flow += dt * (0.4f + 2.4f * e);
    if (s.flow > 1000.0f) s.flow -= 1000.0f;

    const int mode = s.mode;
    const int at = (int)s.phase;
    float u = s.phase - at;
    u = u * u * (3 - 2 * u);
    const float(*P)[2] = PATHS[mode];
    const float cr = P[at % 4][0] + (P[(at + 1) % 4][0] - P[at % 4][0]) * u + 0.025f * in.bass * fastSin(s.spin * 3);
    const float ci = P[at % 4][1] + (P[(at + 1) % 4][1] - P[at % 4][1]) * u + 0.025f * in.mid * fastCos(s.spin * 2);
    const float dive = 1 + 0.25f * fastSin(s.phase * 0.9f);  // slowly in and out
    const float zoom = (mode == 2 ? 1.4f : 1.2f) * dive * (1 - 0.1f * in.bass - 0.22f * s.punch);
    constexpr int MAXN = 24;
    float ringAt[6];  // the last beats' rings of light, as table rings
    int rings = 0;
    for (int i = 0; i < 6; i++) {
        const uint32_t age = ms - s.beats[i];
        if (s.beats[i] && age < 900) ringAt[rings++] = age * (FR / 900.0f);
    }
    volatile uint32_t* tab = fractalTab();
    uint8_t row[FS];
    s.parity ^= 1;
    for (int r = s.parity; r < FR; r += 2) {  // half the rings a frame, as Aleph does
        const float rad = (FR_MIN + (FR_MAX - FR_MIN) * r / (FR - 1)) / FR_MAX * zoom;
        float lift = s.flash * 0.7f;
        for (int i = 0; i < rings; i++) {
            const float d = fabsf(r - ringAt[i]);
            if (d < 2.5f) lift += (1 - d / 2.5f) * (0.25f + 0.45f * e);
        }
        for (int k = 0; k < FS; k++) {
            const float th = (k + 0.5f) * (0.5235988f / FS) + s.spin;
            float zr = rad * fastCos(th), zi = rad * fastSin(th);
            int n = 0;
            float m2 = 0;
            for (; n < MAXN; n++) {
                const float zr2 = zr * zr, zi2 = zi * zi;
                m2 = zr2 + zi2;
                if (m2 > 256.0f) break;
                if (mode == 1) {
                    const float nr = zr * (zr2 - 3 * zi2) + cr;
                    zi = zi * (3 * zr2 - zi2) + ci, zr = nr;
                } else if (mode == 2) {
                    zi = fabsf(2 * zr * zi) + ci, zr = zr2 - zi2 + cr;
                } else if (mode == 3) {
                    zi = -2 * zr * zi + ci, zr = zr2 - zi2 + cr;
                } else {
                    zi = 2 * zr * zi + ci, zr = zr2 - zi2 + cr;
                }
            }
            float v = 0.05f;
            if (n < MAXN) {  // smooth escape time: a body that brightens toward the set, and flowing contour lines
                const float l = 0.5f * fastLog2(m2 > 1.0001f ? m2 : 1.0001f);
                const float mu = n + 1 - fastLog2(l > 0.05f ? l : 0.05f);
                float body = mu / 12;
                body = body > 1 ? 1 : sqrtf(body);
                const float x = fract(mu * 1.5f - s.flow);
                const float line = x < 0.3f ? 1 - x / 0.3f : 0;
                v = 0.08f + 0.55f * body + line * (0.3f + 0.3f * e) * (0.4f + 0.6f * body);
            }
            v += lift;
            row[k] = (uint8_t)(v >= 1 ? TOP : v * TOP);
        }
        for (int k = 0; k < FS; k += 4) tab[(r * FS + k) >> 2] = row[k] | row[k + 1] << 8 | row[k + 2] << 16 | (uint32_t)row[k + 3] << 24;
    }
    // Each pixel: its ring by distance, its spoke by the angle folded into one thirty-degree wedge.
    fillHalf(a, [&](int x, int y) {
        const float px = fabsf((x - a.cx) * K) + 0.01f, py = fabsf(y - a.cy) + 0.01f;
        const float r2 = px * px + py * py, rr = r2 * invSqrt(r2);
        int r = (int)((rr - FR_MIN) * ((FR - 1) / (FR_MAX - FR_MIN)));
        if (r < 0) r = 0;
        if (r >= FR) return 0;
        const float lo = px < py ? px : py, hi = px < py ? py : px;
        const float t = lo * invSqrt(hi * hi);
        float ang = t * (0.9724f - 0.1919f * t * t);  // atan, 0..pi/4
        if (py > px) ang = 1.5707963f - ang;
        ang = fract(ang * (1 / 1.0471976f)) * 2;  // in sixty-degree pieces, 0..2, mirrored
        if (ang > 1) ang = 2 - ang;
        const int k = (int)(ang * FS);
        const int i = r * FS + (k < FS ? k : FS - 1);
        return (int)(tab[i >> 2] >> ((i & 3) * 8)) & 255;
    });
}

}  // namespace

void drawVisualizer(int style, Canvas& c, int x0, int y0, int x1, int y1, int bottom, float dt, uint32_t ms,
                    const VizInput& in, VizState& s) {
    Area a{c, x0, y0, x1, y1, bottom, (x0 + x1) * 0.5f, (y0 + y1) * 0.5f};
    if (!c.shown) s.fed = false;
    switch (style) {
        case VIZ_SPECTRUM: spectrum(a, dt, in, s); break;
        case VIZ_HALO: halo(a, ms, in, s); break;
        case VIZ_LAVA: lava(a, dt, in, s); break;
        case VIZ_RAVE: rave(a, dt, ms, in, s); break;
        case VIZ_FRACTAL: fractal(a, dt, ms, in, s); break;
        default: glow(a, dt, ms, in, s); break;
    }
    if (s.spin > 6283.0f) s.spin -= 6283.0f;
    finish(a);
    s.fed = true;
}
