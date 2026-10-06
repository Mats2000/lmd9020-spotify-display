#include "savers.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "config.h"
#include "draw.h"
#include "fonts.h"
#include "text.h"

namespace {

constexpr float PI_F = 3.14159265f;
const RGB BLACK = {0, 0, 0};

// Phase of motion at perSec, wrapped to period, in double so it stays smooth for weeks.
float cycle(uint32_t ms, double perSec, double period) {
    return (float)fmod(ms * 0.001 * perSec, period);
}

uint8_t dim(RGB c, int a) { return toRGB332(lerp(BLACK, c, a)); }

// Sine to about 0.1%, for per-particle work: newlib's sinf is slow on the ESP32, very slow
// once the angle is large.
inline float fastSin(float x) {
    int k = (int)(x * 0.15915494f + (x >= 0 ? 0.5f : -0.5f));
    x -= k * 6.2831853f;  // now within [-pi, pi]
    float y = 1.27323954f * x - 0.40528473f * x * fabsf(x);
    return 0.225f * (y * fabsf(y) - y) + y;
}
inline float fastCos(float x) { return fastSin(x + 1.5707963f); }

void textCentered(Canvas& c, const Font& font, const char* s, float cx, int baseline, RGB color,
                  int opacity = 256) {
    TextStyle st;
    st.color = color;
    st.opacity = opacity;
    drawText(c, font, s, (int)lroundf(cx * 16) - textWidth16(font, s) / 2, baseline, st);
}

void textAt(Canvas& c, const Font& font, const char* s, float x, int baseline, RGB color, int opacity = 256) {
    TextStyle st;
    st.color = color;
    st.opacity = opacity;
    drawText(c, font, s, (int)lroundf(x * 16), baseline, st);
}

void textRight(Canvas& c, const Font& font, const char* s, float right, int baseline, RGB color,
               int opacity = 256) {
    TextStyle st;
    st.color = color;
    st.opacity = opacity;
    drawText(c, font, s, (int)lroundf(right * 16) - textWidth16(font, s), baseline, st);
}

// A unit circle in 256 steps, so arcs need no trigonometry per segment.
struct UnitCircle {
    float cosv[257], sinv[257];
    UnitCircle() {
        for (int i = 0; i <= 256; i++) cosv[i] = cosf(i * (2 * PI_F / 256)), sinv[i] = sinf(i * (2 * PI_F / 256));
    }
};
const UnitCircle CIRCLE;

// Ellipse arc that looks circular on screen; radius in lines, angle clockwise from right.
void arc(Canvas& c, float cx, float cy, float r, float a0, float a1, RGB color, int alpha = 256) {
    int s0 = (int)floorf(a0 * (256 / (2 * PI_F))), s1 = (int)ceilf(a1 * (256 / (2 * PI_F)));
    int stride = r > 40 ? 2 : 4;  // about 3 px per segment either way
    const float rx = r / DISPLAY_PIXEL_ASPECT;
    float px = cx + rx * CIRCLE.cosv[s0 & 255], py = cy + r * CIRCLE.sinv[s0 & 255];
    for (int s = s0 + stride; s <= s1 + stride - 1; s += stride) {
        int i = (s > s1 ? s1 : s) & 255;
        float x = cx + rx * CIRCLE.cosv[i], y = cy + r * CIRCLE.sinv[i];
        lineAA(c, px, py, x, y, color, alpha);
        px = x, py = y;
    }
}

// Outline with softened corners.
void frame(Canvas& c, int x0, int y0, int x1, int y1, uint8_t color) {
    hline(c, x0 + 2, x1 - 2, y0, color);
    hline(c, x0 + 2, x1 - 2, y1, color);
    vline(c, x0, y0 + 2, y1 - 2, color);
    vline(c, x1, y0 + 2, y1 - 2, color);
    if (x0 + 1 >= 0 && y0 + 1 >= 0 && x1 - 1 < Canvas::W && y1 - 1 < Canvas::H) {
        c.rows[y0 + 1][x0 + 1] = c.rows[y0 + 1][x1 - 1] = color;
        c.rows[y1 - 1][x0 + 1] = c.rows[y1 - 1][x1 - 1] = color;
    }
}

// Row y of a vertical gradient as a dither pattern, written four pixels at a time.
void ditherRow(Canvas& c, int y, int x0, int x1, RGB col) {
    if (y < 0 || y >= Canvas::H) return;
    uint8_t pattern[4];
    for (int x = 0; x < 4; x++) pattern[x] = dither332(col, x, y);
    if (x0 < 0) x0 = 0;
    if (x1 > Canvas::W) x1 = Canvas::W;
    uint8_t* row = c.rows[y];
    int x = x0;
    for (; x < x1 && (x & 3); x++) row[x] = pattern[x & 3];
    uint32_t word;
    memcpy(&word, pattern, 4);
    for (; x + 4 <= x1; x += 4) memcpy(row + x, &word, 4);
    for (; x < x1; x++) row[x] = pattern[x & 3];
}

RGB mix3(RGB a, RGB b, RGB c, float u) {  // a at 0, b at 0.5, c at 1
    return u < 0.5f ? lerp(a, b, (int)(u * 512)) : lerp(b, c, (int)((u - 0.5f) * 512));
}

// Black to the accent colour in 16 steps, for the particle scenes.
void accentRamp(RGB accent, uint8_t ramp[16]) {
    for (int i = 0; i < 16; i++) ramp[i] = toRGB332(lerp(BLACK, accent, i * 256 / 15));
}

// 0 in the clock's calm patch, rising to 1 at its edge.
inline float calm(const SaverFrame& f, int x, int y) {
    float dx = (x - f.clockX) / (f.clockHalfW + 22.0f), dy = (y - f.clockY) / (f.clockHalfH + 20.0f);
    float e = dx * dx + dy * dy;
    return e < 1.0f ? e * e : 1.0f;
}

// One particle: level 0..15 of the ramp; `big` doubles it to 2x2.
inline void dot(Canvas& c, int x, int y, const uint8_t ramp[16], float level, bool big = false) {
    int l = (int)(level + 0.5f);
    if (l <= 0 || x < 0 || y < 0 || x >= Canvas::W - 1 || y >= Canvas::H - 1) return;
    uint8_t col = ramp[l > 15 ? 15 : l];
    c.rows[y][x] = col;
    if (big) c.rows[y][x + 1] = c.rows[y + 1][x] = c.rows[y + 1][x + 1] = col;
}

// ---------------------------------------------------------------------------
// Waves: a field of dots on a rolling sea, seen from just above.

void drawWaves(Canvas& c, const SaverFrame& f) {
    constexpr int NX = 64, NZ = 32;
    constexpr float DX = 0.46f, DZ = 0.46f;  // grid spacing, world units
    constexpr float Z0 = 1.4f;               // nearest row
    constexpr float AMP = 0.30f;             // each of the two swells
    constexpr float CAM_H = 3.0f, PITCH = 0.55f, FOCAL = 170.0f;
    constexpr float ZFAR = Z0 + (NZ - 1) * DZ;
    const float CX = Canvas::W * 0.5f, CY = Canvas::H * 0.5f;

    uint8_t ramp[16];  // black to the accent colour
    for (int i = 0; i < 16; i++) ramp[i] = toRGB332(lerp(BLACK, f.accent, i * 256 / 15));

    const float sp = sinf(PITCH), cp = cosf(PITCH);
    const float phaseX = cycle(f.ms, 1.25, 2 * PI_F), phaseZ = cycle(f.ms, 0.85, 2 * PI_F);

    // Two crossing swells, so each row and column only needs one sine.
    float swellX[NX], swellZ[NZ], colX[NX];
    for (int i = 0; i < NX; i++) {
        swellX[i] = AMP * sinf(i * 0.33f + phaseX);
        colX[i] = (i - (NX - 1) * 0.5f) * DX / DISPLAY_PIXEL_ASPECT;
    }
    for (int k = 0; k < NZ; k++) swellZ[k] = AMP * sinf(k * 0.47f - phaseZ);

    // Dots thin out around the clock so it floats in a calm patch.
    const float rx = f.clockHalfW + 22.0f, ry = f.clockHalfH + 20.0f;
    const float irx = 1.0f / rx, iry = 1.0f / ry;

    for (int k = NZ - 1; k >= 0; k--) {  // far to near, so near dots win
        const float z = Z0 + k * DZ;
        const float depth = (z - Z0) / (ZFAR - Z0);  // 0 near .. 1 far
        const float rowLight = (1.0f - depth) * (1.0f - depth * 0.35f) * 15.0f;
        // Second-order perspective approximation: no division per dot.
        const float zc0 = CAM_H * sp + z * cp, yc0 = -CAM_H * cp + z * sp;
        const float inv0 = FOCAL / zc0, s = sp / zc0;
        // Only the columns that can land on screen.
        int half = (int)(CX / (inv0 * 0.8f * DX / DISPLAY_PIXEL_ASPECT)) + 1;
        int i0 = (NX - 1) / 2 - half, i1 = NX / 2 + half;
        if (i0 < 0) i0 = 0;
        if (i1 > NX - 1) i1 = NX - 1;
        for (int i = i0; i <= i1; i++) {
            const float y = swellX[i] + swellZ[k];
            const float u = y * s;
            const float inv = inv0 * (1.0f + u + u * u);
            const float fx = CX + colX[i] * inv;
            const float fy = CY - (yc0 + y * cp) * inv;
            if (fx < 0 || fx >= Canvas::W - 1 || fy < 0 || fy >= Canvas::H - 1) continue;
            const int sx = (int)(fx + 0.5f), sy = (int)(fy + 0.5f);

            const float crest = 0.55f + 0.45f * (y * (0.5f / AMP) + 0.5f);  // troughs dimmer
            float b = rowLight * crest;
            const float dx = (sx - f.clockX) * irx, dy = (sy - f.clockY) * iry;
            const float e = dx * dx + dy * dy;
            if (e < 1.0f) b *= e * e;
            const int level = (int)(b + 0.5f);
            if (level <= 0) continue;
            const uint8_t col = ramp[level > 15 ? 15 : level];

            c.rows[sy][sx] = col;
            if (depth < 0.22f && crest > 0.7f) {  // near crests get a bigger dot
                c.rows[sy][sx + 1] = col;
                c.rows[sy + 1][sx] = col;
                c.rows[sy + 1][sx + 1] = col;
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Sphere: a globe of dots turning slowly and breathing out of round.

void drawSphere(Canvas& c, const SaverFrame& f) {
    uint8_t ramp[16];
    accentRamp(f.accent, ramp);
    const float t = cycle(f.ms, 1.0, 3600.0);
    const float spin = cycle(f.ms, 0.20, 2 * PI_F);
    const float tilt = 0.40f + 0.10f * sinf(t * 0.11f);
    const float ct = cosf(tilt), st = sinf(tilt);
    // Breathing: a slow mix of low-order shapes, as polynomials (no trig per dot).
    const float a = 0.11f * sinf(t * 0.53f), b = 0.09f * sinf(t * 0.71f + 1.0f), d = 0.07f * sinf(t * 0.37f + 2.0f);
    const float D = 3.4f, F = 300.0f;
    const float cx = Canvas::W * 0.5f, cy = Canvas::H * 0.5f;
    const int RINGS = 26;

    for (int pass = 0; pass < 2; pass++) {  // far side first, then near
        for (int i = 0; i < RINGS; i++) {
            float lat = -PI_F * 0.5f + (i + 0.5f) * PI_F / RINGS;
            float ry = sinf(lat), rr = cosf(lat);
            int n = (int)(rr * 64.0f) + 6;
            float step = 2 * PI_F / n, start = spin + i * 0.37f;
            float cs = cosf(start), sn = sinf(start);
            const float cd = cosf(step), sd = sinf(step);
            for (int j = 0; j < n; j++) {
                float x = rr * cs, y = ry, z = rr * sn;
                float wob = 1.0f + a * (x * x - z * z) + b * x * y + d * (3.0f * y * y - 1.0f);
                x *= wob, y *= wob, z *= wob;
                float y2 = y * ct - z * st, z2 = y * st + z * ct;
                float ncs = cs * cd - sn * sd;
                sn = sn * cd + cs * sd;
                cs = ncs;
                if ((z2 > 0) != (pass == 1)) continue;
                float scale = F / (D - z2);
                int sx = (int)(cx + x * scale / DISPLAY_PIXEL_ASPECT + 0.5f);
                int sy = (int)(cy - y2 * scale + 0.5f);
                float level = pass ? 7.0f + 8.0f * z2 : 2.0f + 2.5f * (1.0f + z2);
                dot(c, sx, sy, ramp, level * calm(f, sx, sy), pass && z2 > 0.75f);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Vinyl: a dotted record seen at an angle, turning slowly.

void drawVinyl(Canvas& c, const SaverFrame& f) {
    uint8_t ramp[16];
    accentRamp(f.accent, ramp);
    const float spin = cycle(f.ms, 0.26, 2 * PI_F);
    const float wave = cycle(f.ms, 2.0, 2 * PI_F);
    const float ELEV = 0.52f, se = sinf(ELEV), ce = cosf(ELEV);
    const float D = 2.7f, F = 360.0f;
    const float cx = Canvas::W * 0.5f + f.ox, cy = 160.0f + f.oy;
    const float hlc = cosf(2.3f), hls = sinf(2.3f);  // highlight direction
    const int RINGS = 30;

    for (int i = 0; i < RINGS; i++) {
        float r = 0.24f + 0.76f * i / (RINGS - 1);
        float h = 0.045f * sinf(r * 19.0f - wave) * (1.1f - r * 0.4f);
        float groove = (i & 1) ? 0.85f : 1.0f;
        int n = (int)(r * 104.0f) + 8;
        float step = 2 * PI_F / n, start = spin * (1.0f + 0.0f * r) + i * 0.21f;
        float cs = cosf(start), sn = sinf(start);
        const float cd = cosf(step), sd = sinf(step);
        for (int j = 0; j < n; j++) {
            float X = r * cs, Z = r * sn, Y = h;
            float zc = D - Y * se - Z * ce, yc = Y * ce - Z * se;
            float scale = F / zc;
            int sx = (int)(cx + X * scale / DISPLAY_PIXEL_ASPECT + 0.5f);
            int sy = (int)(cy - yc * scale + 0.5f);
            float sheen = cs * hlc + sn * hls;  // cosine to the highlight
            sheen = sheen > 0 ? sheen * sheen * sheen * sheen : 0;
            float level = (4.0f + 3.0f * (Z + 1.0f) + 7.0f * sheen + 40.0f * h) * groove;
            dot(c, sx, sy, ramp, level * calm(f, sx, sy), Z > 0.55f && sheen > 0.5f);
            float ncs = cs * cd - sn * sd;
            sn = sn * cd + cs * sd;
            cs = ncs;
        }
    }
}

// ---------------------------------------------------------------------------
// Ridges: rolling dotted ridge lines, after the Unknown Pleasures cover.

void drawRidges(Canvas& c, const SaverFrame& f) {
    uint8_t ramp[16];
    accentRamp(f.accent, ramp);
    const int ROWS = 19, COLS = 100;
    const float TOP = 100.0f + f.oy, SPACING = 6.8f;  // far row's baseline, and the gap between rows
    const float cx = Canvas::W * 0.5f + f.ox;
    const double rowsPerSec = 0.35;
    const float travel = (float)fmod(f.ms * 0.001 * rowsPerSec, 1.0);
    const uint32_t newest = (uint32_t)(f.ms * 0.001 * rowsPerSec);
    const float t = cycle(f.ms, 1.0, 3600.0);

    int16_t sky[Canvas::W];  // highest point drawn so far in each column
    for (int x = 0; x < Canvas::W; x++) sky[x] = Canvas::H;

    for (int k = ROWS - 1; k >= 0; k--) {  // near (bottom) to far (top)
        float pos = k + travel;             // 0 = far .. ROWS = near
        uint32_t id = newest - k;           // each row keeps its shape as it comes forward
        float base = TOP + pos * SPACING;
        float width = (78.0f + 30.0f * pos / ROWS);  // half-width, px: a gentle recession
        float depth = 1.0f - pos / ROWS;
        float fade = pos < 1.0f ? pos : (pos > ROWS - 1 ? ROWS - pos : 1.0f);
        float level = (13.0f - 8.0f * depth) * fade;
        float amp = (26.0f + 30.0f * hash01(id * 7 + 1)) * (0.85f + 0.3f * pos / ROWS);
        int px = -1, py = 0;
        for (int i = 0; i < COLS; i++) {
            float x = (i - (COLS - 1) * 0.5f) / ((COLS - 1) * 0.5f);  // -1 .. 1
            float centre = expf(-x * x * 6.0f);
            float n = 0.7f * valueNoise(id * 13 + 5, i * 0.21f) + 0.3f * valueNoise(id * 29 + 9, i * 0.6f + t * 0.5f);
            float y = amp * centre * (0.25f + 1.6f * n * n) + 1.5f * valueNoise(id * 3 + 1, i * 0.9f);
            int sx = (int)(cx + x * width + 0.5f);
            int sy = (int)(base - y + 0.5f);
            bool visible = sx >= 0 && sx < Canvas::W && sy < sky[sx];
            if (visible) dot(c, sx, sy, ramp, level * calm(f, sx, sy));
            if (px >= 0) {  // hidden-line removal: raise the skyline under this segment
                for (int xx = px < 0 ? 0 : px; xx <= sx && xx < Canvas::W; xx++) {
                    int yy = sx == px ? sy : py + (sy - py) * (xx - px) / (sx - px);
                    if (yy < sky[xx]) sky[xx] = (int16_t)yy;
                }
            }
            px = sx, py = sy;
        }
    }
}

// ---------------------------------------------------------------------------
// Network: drifting nodes linking up, with packets running along the links.

void drawNetwork(Canvas& c, const SaverFrame& f) {
    const int N = 34;
    const RGB wire = {200, 214, 236}, packet = {255, 56, 70};
    const float t = cycle(f.ms, 1.0, 3600.0);

    for (int y = 16 + f.oy; y < Canvas::H; y += 16)  // the grid: a dot every 16 px
        for (int x = 8 + f.ox; x < Canvas::W; x += 16)
            if (x >= 0 && y >= 0) c.rows[y][x] = toRGB332(RGB{40, 46, 64});

    float px[N], py[N];
    for (int i = 0; i < N; i++) {  // each node on its own slow Lissajous path
        float ax = 40.0f + 70.0f * hash01(i * 11 + 1), ay = 30.0f + 60.0f * hash01(i * 11 + 2);
        float wx = 0.05f + 0.10f * hash01(i * 11 + 3), wy = 0.04f + 0.09f * hash01(i * 11 + 4);
        px[i] = Canvas::W * (0.12f + 0.76f * hash01(i * 11 + 5)) + ax * 0.4f * sinf(t * wx + i);
        py[i] = Canvas::H * (0.12f + 0.76f * hash01(i * 11 + 6)) + ay * 0.4f * sinf(t * wy + i * 1.7f);
    }
    const float R = 54.0f;  // link reach, in screen lines
    for (int i = 0; i < N; i++) {
        for (int j = i + 1; j < N; j++) {
            float dx = (px[j] - px[i]) * DISPLAY_PIXEL_ASPECT, dy = py[j] - py[i];
            float d2 = dx * dx + dy * dy;
            if (d2 > R * R) continue;
            float strength = 1.0f - sqrtf(d2) / R;
            float quiet = calm(f, (int)((px[i] + px[j]) * 0.5f), (int)((py[i] + py[j]) * 0.5f));
            lineAA(c, px[i], py[i], px[j], py[j], wire, (int)(strength * 170 * quiet));
            if (hash01(i * 97 + j * 13) < 0.3f) {  // this link carries traffic
                float u = cycle(f.ms, 0.25 + 0.3 * hash01(i + j * 7), 1.0);
                int x = (int)(px[i] + (px[j] - px[i]) * u), y = (int)(py[i] + (py[j] - py[i]) * u);
                c.blend(x, y, packet, (int)(256 * quiet));
                c.blend(x + 1, y, packet, (int)(160 * quiet));
            }
        }
    }
    char label[8];
    for (int i = 0; i < N; i++) {
        int x = (int)px[i], y = (int)py[i];
        float quiet = calm(f, x, y);
        uint8_t node = toRGB332(lerp(BLACK, RGB{240, 244, 255}, (int)(256 * quiet)));
        fillRect(c, x, y, 2, 2, node);
        if (i % 8 == 3 && quiet > 0.9f) {  // a few nodes wear an address
            hline(c, x - 3, x + 4, y - 3, toRGB332(RGB{90, 96, 120}));
            snprintf(label, sizeof(label), "%02X:%02X", (unsigned)(hash01(i * 3) * 255), (unsigned)(hash01(i * 5) * 255));
            textAt(c, sora_micro, label, x + 6, y + 3, RGB{150, 160, 190}, 200);
        }
    }
}

// ---------------------------------------------------------------------------
// Tunnel: a twisting tube of dot rings with data pulses running down it.

void drawTunnel(Canvas& c, const SaverFrame& f) {
    uint8_t ramp[16];
    accentRamp(RGB{140, 230, 255}, ramp);
    const RGB pulse = {255, 60, 80};
    const int RINGS = 24, DOTS = 48;
    const float DZ = 0.5f, F = 130.0f;
    const float travel = (float)fmod(f.ms * 0.001 * 0.9, DZ);
    const uint32_t passed = (uint32_t)(f.ms * 0.001 * 0.9 / DZ);
    const float t = cycle(f.ms, 1.0, 3600.0);
    const float CX = Canvas::W * 0.5f, CY = Canvas::H * 0.5f;
    const float step = 2 * PI_F / DOTS, cd = cosf(step), sd = sinf(step);

    for (int k = RINGS - 1; k >= 0; k--) {  // far to near
        float z = (k + 1) * DZ - travel;
        if (z < 0.25f) continue;
        // The tube bends: its centre sways with depth and time.
        float ox = 0.5f * sinf(z * 0.45f + t * 0.31f), oy = 0.35f * cosf(z * 0.33f + t * 0.23f);
        float scale = F / z;
        float fog = 1.0f - z / (RINGS * DZ);
        bool marker = ((passed + k) % 4) == 0;
        float level = (marker ? 15.0f : 10.0f) * (0.15f + 0.85f * fog);
        float a = z * 0.18f + t * 0.12f;  // twist
        float cs = cosf(a), sn = sinf(a);
        for (int j = 0; j < DOTS; j++) {
            float wx = ox + cs, wy = oy + sn;
            int sx = (int)(CX + wx * scale / DISPLAY_PIXEL_ASPECT + 0.5f), sy = (int)(CY + wy * scale + 0.5f);
            dot(c, sx, sy, ramp, level * calm(f, sx, sy), z < 1.2f);
            float ncs = cs * cd - sn * sd;
            sn = sn * cd + cs * sd;
            cs = ncs;
        }
    }
    // Pulses along four lanes of the tube, coming toward you.
    for (int lane = 0; lane < 4; lane++) {
        float head = (float)fmod(f.ms * 0.001 * (2.2 + lane * 0.5) + lane * 2.7, RINGS * DZ);
        float z = RINGS * DZ - head;
        float ang = lane * (PI_F * 0.5f) + 0.4f;
        for (int k = 0; k < 6; k++) {
            float zz = z + k * 0.12f;
            if (zz < 0.3f) continue;
            float ox = 0.5f * sinf(zz * 0.45f + t * 0.31f), oy = 0.35f * cosf(zz * 0.33f + t * 0.23f);
            float a = ang + zz * 0.18f + t * 0.12f, scale = F / zz;
            int sx = (int)(CX + (ox + cosf(a)) * scale / DISPLAY_PIXEL_ASPECT), sy = (int)(CY + (oy + sinf(a)) * scale);
            c.blend(sx, sy, pulse, (int)((256 - k * 40) * calm(f, sx, sy)));
        }
    }
}

// ---------------------------------------------------------------------------
// Scope: a green phosphor oscilloscope figure over a dotted graticule.

void drawScope(Canvas& c, const SaverFrame& f) {
    uint8_t ramp[16];
    accentRamp(RGB{120, 255, 150}, ramp);
    const int ox = f.ox, oy = f.oy;
    const float t = cycle(f.ms, 1.0, 3600.0);

    // Graticule: 10 x 8 divisions, dotted, with ticks on the centre lines.
    const int gx0 = 28 + ox, gx1 = 228 + ox, gy0 = 30 + oy, gy1 = 210 + oy;
    const uint8_t grat = toRGB332(RGB{30, 70, 40});
    for (int i = 0; i <= 10; i++) {
        int x = gx0 + (gx1 - gx0) * i / 10;
        for (int y = gy0; y <= gy1; y += (i == 5 ? 3 : 6)) c.rows[y][x] = grat;
    }
    for (int i = 0; i <= 8; i++) {
        int y = gy0 + (gy1 - gy0) * i / 8;
        for (int x = gx0; x <= gx1; x += (i == 4 ? 3 : 6)) c.rows[y][x] = grat;
    }

    // The figure: x = sin(a s + phase), y = sin(b s), with the ratio drifting.
    const float A = 3.0f, B = 2.0f + 0.5f * sinf(t * 0.045f) + 0.25f * sinf(t * 0.017f);
    const float phase = cycle(f.ms, 0.35, 2 * PI_F);
    const float cx = (gx0 + gx1) * 0.5f, cy = (gy0 + gy1) * 0.5f, rx = (gx1 - gx0) * 0.42f, ry = (gy1 - gy0) * 0.42f;
    const int SAMPLES = 520;
    for (int i = 0; i < SAMPLES; i++) {  // the whole trace, faint
        float s = i * (2 * PI_F / SAMPLES);
        int sx = (int)(cx + rx * fastSin(A * s + phase)), sy = (int)(cy - ry * fastSin(B * s));
        dot(c, sx, sy, ramp, 4.0f * calm(f, sx, sy));
    }
    const float head = cycle(f.ms, 1.4, 2 * PI_F);
    for (int i = 0; i < 160; i++) {  // the beam and its afterglow
        float s = head - i * 0.006f;
        int sx = (int)(cx + rx * fastSin(A * s + phase)), sy = (int)(cy - ry * fastSin(B * s));
        dot(c, sx, sy, ramp, (15.0f - i * 0.075f) * calm(f, sx, sy), i < 8);
    }
    textAt(c, sora_micro, "CH1 2V/DIV", gx0, gy1 + 11, RGB{120, 255, 150}, 130);
    textRight(c, sora_micro, "X-Y", gx1, gy1 + 11, RGB{120, 255, 150}, 130);
}

// ---------------------------------------------------------------------------
// Terminal: hex dumps scrolling up both sides, a blinking prompt at the bottom.

void drawTerminal(Canvas& c, const SaverFrame& f) {
    const RGB fg = {170, 220, 190}, red = {255, 70, 80};
    const int LINE = 9, TOP = 18, BOTTOM = 200;
    const float scroll = (float)fmod(f.ms * 0.001 * 7.0, 1e6);  // px scrolled
    const uint32_t first = (uint32_t)(scroll / LINE);
    const int shift = (int)fmodf(scroll, (float)LINE);
    char buf[24];
    for (int col = 0; col < 2; col++) {
        int x = (col ? 158 : 22) + f.ox;
        for (int r = 0; r < (BOTTOM - TOP) / LINE + 1; r++) {
            uint32_t line = first + r + col * 7919;
            int y = TOP + r * LINE - shift + f.oy;
            if (y < TOP || y > BOTTOM) continue;
            uint32_t addr = line * 16;
            snprintf(buf, sizeof(buf), "%04X %02X%02X %02X%02X", (unsigned)(addr & 0xFFFF),
                     (unsigned)(hash01(line * 4) * 255), (unsigned)(hash01(line * 4 + 1) * 255),
                     (unsigned)(hash01(line * 4 + 2) * 255), (unsigned)(hash01(line * 4 + 3) * 255));
            // Fade in at the bottom, out at the top.
            float u = (float)(y - TOP) / (BOTTOM - TOP);
            int opacity = (int)(220 * (u < 0.2f ? u / 0.2f : (u > 0.85f ? (1 - u) / 0.15f : 1.0f)));
            int gap = y - (f.clockY + f.clockHalfH + 2);  // a quiet band where the clock sits
            if (gap > -(2 * f.clockHalfH + 14) && gap < 12) continue;
            bool hot = hash01(line * 31 + 3) < 0.05f;
            textAt(c, sora_micro, buf, x, y, hot ? red : fg, hot ? opacity : opacity * 2 / 3);
        }
    }
    bool cursorOn = (f.ms / 530) & 1;
    const char* prompt = "> connecting to the wired";
    int w = textWidth16(sora_micro, prompt) / 16;
    int px = Canvas::W / 2 - w / 2 + f.ox, py = 220 + f.oy;
    textAt(c, sora_micro, prompt, px, py, fg, 230);
    if (cursorOn) fillRect(c, px + w + 2, py - 7, 4, 8, toRGB332(fg));
}

// ---------------------------------------------------------------------------
// Static: TV snow with a rolling band; now and then the signal drops out.

void drawStatic(Canvas& c, const SaverFrame& f) {
    // Grey levels that are exact in RGB332 (equal r, g and b), weighted to dark.
    static const uint8_t SNOW[8] = {0x00, 0x00, 0x00, 0x00, 0x49, 0x49, 0x92, 0xB6};
    static const uint8_t BAND[8] = {0x00, 0x49, 0x49, 0x92, 0x92, 0xDB, 0xDB, 0xFF};
    static const uint8_t DARK[8] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x49};
    const uint32_t slot = f.ms / 13000, inSlot = f.ms % 13000;
    const bool dropout = inSlot < 700 && hash01(slot * 7 + 1) < 0.8f;
    const float roll = (float)fmod(f.ms * 0.001 * 23.0, Canvas::H + 60.0) - 30.0f;
    const int quietTop = f.clockY - f.clockHalfH - 12, quietBottom = f.clockY + f.clockHalfH + 12;

    uint32_t seed = (f.ms / 33) * 2654435761u + 12345u;
    for (int y = 0; y < Canvas::H; y++) {
        const uint8_t* table = dropout || (y > quietTop && y < quietBottom) ? DARK
                               : (fabsf(y - roll) < 16.0f ? BAND : SNOW);
        uint8_t* row = c.rows[y];
        for (int x = 0; x < Canvas::W; x += 4) {
            seed ^= seed << 13;  // xorshift: no visible pattern, unlike an LCG's low bits
            seed ^= seed >> 17;
            seed ^= seed << 5;
            uint32_t r = seed;
            uint32_t word = table[r & 7] | (table[(r >> 3) & 7] << 8) | (table[(r >> 6) & 7] << 16) |
                            ((uint32_t)table[(r >> 9) & 7] << 24);
            memcpy(row + x, &word, 4);
        }
    }
    if (dropout) {
        const RGB red = {230, 40, 50};
        textCentered(c, sora_artist, inSlot < 350 ? "present day" : "present time", Canvas::W * 0.5f,
                     quietBottom + 16, red, (inSlot / 60) % 2 ? 256 : 150);
    }
}

// ---------------------------------------------------------------------------
// Navi: wireframe windows of a made-up 90s OS opening around the clock.

struct Window {
    int x0, y0, x1, y1;
    const char* title;
};

// 0 closed .. 1 open, for window i at time ms: each lives on its own cycle.
float windowOpen(int i, uint32_t ms) {
    uint32_t period = 23000 + i * 4100, t = (ms + i * 7300) % period;
    const uint32_t OPEN = 350, CLOSED = 3500;
    if (t < OPEN) return t / (float)OPEN;
    if (t < period - CLOSED - OPEN) return 1.0f;
    if (t < period - CLOSED) return 1.0f - (t - (period - CLOSED - OPEN)) / (float)OPEN;
    return 0.0f;
}

void drawNavi(Canvas& c, const SaverFrame& f) {
    const RGB line = {176, 178, 196}, title = {58, 58, 82}, text = {196, 202, 220}, red = {235, 52, 64};
    const int ox = f.ox, oy = f.oy;
    for (int y = 0; y < Canvas::H; y++) ditherRow(c, y, 0, Canvas::W, RGB{8, 10, 20});
    for (int y = 12 + oy; y < Canvas::H; y += 12)
        for (int x = 6 + ox; x < Canvas::W; x += 12)
            if (x >= 0 && y >= 0) c.rows[y][x] = toRGB332(RGB{36, 40, 64});

    const Window wins[4] = {{14, 12, 120, 84, "TERM"},
                            {136, 12, 242, 72, "SIGNAL"},
                            {14, 166, 120, 218, "LINK"},
                            {136, 162, 242, 224, "MEM"}};
    char buf[32];
    for (int i = 0; i < 4; i++) {
        float open = windowOpen(i, f.ms);
        if (open <= 0) continue;
        Window w = wins[i];
        w.x0 += ox, w.x1 += ox, w.y0 += oy, w.y1 += oy;
        int cx = (w.x0 + w.x1) / 2, cy = (w.y0 + w.y1) / 2;
        if (open < 1.0f) {  // opening/closing: the outline grows from a line
            int hw = (int)((w.x1 - w.x0) / 2 * open), hh = (int)((w.y1 - w.y0) / 2 * open * open);
            frame(c, cx - hw, cy - hh, cx + hw, cy + hh, toRGB332(line));
            continue;
        }
        fillRect(c, w.x0 + 1, w.y0 + 1, w.x1 - w.x0 - 1, w.y1 - w.y0 - 1, toRGB332(RGB{4, 4, 10}));
        fillRect(c, w.x0 + 1, w.y0 + 1, w.x1 - w.x0 - 1, 9, toRGB332(title));
        frame(c, w.x0, w.y0, w.x1, w.y1, toRGB332(line));
        textAt(c, sora_micro, w.title, w.x0 + 4, w.y0 + 9, text, 220);
        fillRect(c, w.x1 - 8, w.y0 + 3, 5, 5, toRGB332(i == 1 ? red : line));
        const int top = w.y0 + 11;
        if (i == 0) {  // terminal: a script, typed out a character at a time
            static const char* SCRIPT[] = {"> ping 10.0.7.1", "reply 7 ms", "> trace layer07", "hop 1 ok",
                                           "hop 2 ok", "hop 3 ???", "> who is there", "( no reply )",
                                           "> present day", "> present time"};
            const int LINES = 10, CPS = 9;
            uint32_t chars = (f.ms / (1000 / CPS)) % 260;
            int shown = 0, row = 0;
            for (int l = 0; l < LINES && row < 6; l++) {
                int len = (int)strlen(SCRIPT[l]);
                if ((int)chars <= shown) break;
                int n = (int)chars - shown < len ? (int)chars - shown : len;
                shown += len + 4;  // a beat between lines
                int first = l - 5 > 0 ? l - 5 : 0;
                (void)first;
                snprintf(buf, sizeof(buf), "%.*s", n, SCRIPT[l]);
                textAt(c, sora_micro, buf, w.x0 + 4, top + 9 + (row % 6) * 10, l == 5 || l == 7 ? red : text, 220);
                row++;
            }
        } else if (i == 1) {  // signal: a scrolling trace
            float ph = cycle(f.ms, 2.0, 1000.0);
            float py = 0;
            for (int x = w.x0 + 3; x < w.x1 - 2; x++) {
                float u = (x - w.x0) * 0.09f + ph;
                float v = 0.55f * sinf(u) + 0.3f * sinf(u * 2.7f + 1) + 0.25f * (valueNoise(77, u * 2) - 0.5f);
                float y = (top + w.y1) * 0.5f - v * (w.y1 - top) * 0.4f;
                if (x > w.x0 + 3) lineAA(c, x - 1, py, x, y, RGB{120, 255, 160});
                py = y;
            }
        } else if (i == 2) {  // link: a progress bar that fills, connects, starts over
            uint32_t t = f.ms % 9000;
            int pct = t < 6000 ? (int)(t / 60) : 100;
            snprintf(buf, sizeof(buf), pct < 100 ? "LINK %d%%" : "CONNECTED", pct);
            textAt(c, sora_micro, buf, w.x0 + 5, top + 14, pct < 100 || (t / 300) % 2 ? text : red, 230);
            int bx0 = w.x0 + 5, bx1 = w.x1 - 5, by = top + 22;
            frame(c, bx0, by, bx1, by + 8, toRGB332(line));
            fillRect(c, bx0 + 2, by + 2, (bx1 - bx0 - 3) * pct / 100, 5, toRGB332(text));
        } else {  // memory: a hex grid that changes now and then
            uint32_t gen = f.ms / 700;
            for (int r = 0; r < 5; r++)
                for (int q = 0; q < 4; q++) {
                    uint32_t h = gen * 131 + r * 17 + q * 5 + (r * 4 + q) * (gen % 3);
                    snprintf(buf, sizeof(buf), "%02X", (unsigned)(hash01(h) * 255));
                    bool hot = hash01(h + 99) < 0.06f;
                    textAt(c, sora_micro, buf, w.x0 + 6 + q * 24, top + 11 + r * 10, hot ? red : text, hot ? 256 : 170);
                }
        }
    }

    // The cursor glides between the windows.
    const float t = cycle(f.ms, 0.12, 4.0);
    int from = (int)t, to = (from + 1) % 4;
    float u = t - from;
    u = u * u * (3 - 2 * u);
    float mx = (wins[from].x0 + wins[from].x1) * 0.5f + ((wins[to].x0 + wins[to].x1) - (wins[from].x0 + wins[from].x1)) * 0.5f * u + ox;
    float my = (wins[from].y0 + wins[from].y1) * 0.5f + ((wins[to].y0 + wins[to].y1) - (wins[from].y0 + wins[from].y1)) * 0.5f * u + oy;
    const uint8_t white = 0xFF, black = 0x00;
    for (int r = 0; r < 9; r++) {  // a classic arrow, 9 rows tall
        int w = r < 7 ? r / 1 : 7 - r;
        for (int q = 0; q <= (r < 7 ? r / 2 + 1 : 2); q++) {
            int x = (int)mx + q, y = (int)my + r;
            if (x >= 0 && x < Canvas::W && y >= 0 && y < Canvas::H) c.rows[y][x] = q == 0 || q == (r < 7 ? r / 2 + 1 : 2) || r == 8 ? black : white;
        }
        (void)w;
    }
}

// ---------------------------------------------------------------------------
// Red sky: rooftops, antennas and crows against a blood-red dusk.

void drawRedSky(Canvas& c, const SaverFrame& f) {
    const int ox = f.ox, oy = f.oy;
    const int horizon = 168 + oy;
    const float t = cycle(f.ms, 1.0, 3600.0);

    for (int y = 0; y < Canvas::H; y++) {
        float u = (float)y / horizon;
        ditherRow(c, y, 0, Canvas::W, mix3(RGB{18, 0, 8}, RGB{132, 14, 24}, RGB{246, 112, 52}, u > 1 ? 1 : u));
    }
    // Long low clouds drifting.
    for (int b = 0; b < 5; b++) {
        int y0 = 34 + b * 22 + (int)(hash01(b) * 8) + oy;
        float drift = t * (2.0f + b * 0.7f);
        RGB col = mix3(RGB{18, 0, 8}, RGB{132, 14, 24}, RGB{246, 112, 52}, (float)y0 / horizon);
        col = lerp(col, RGB{40, 0, 12}, 150);
        for (int x = 0; x < Canvas::W; x++) {
            float n = valueNoise(600 + b, (x + drift) * 0.035f);
            int th = (int)((n - 0.45f) * 14);
            for (int k = 0; k < th; k++) {
                int y = y0 + k - th / 2;
                if (y >= 0 && y < Canvas::H) c.rows[y][x] = dither332(col, x, y);
            }
        }
    }

    // Skyline: houses, antennas, a water tower, a block of flats.
    int16_t top[Canvas::W];
    for (int x = 0; x < Canvas::W; x++) top[x] = (int16_t)(horizon + 30);
    int x = -6;
    for (int i = 0; x < Canvas::W + 10; i++) {
        int w = 20 + (int)(hash01(i * 5 + 1) * 26);
        int wall = 16 + (int)(hash01(i * 5 + 2) * 22);
        bool gable = hash01(i * 5 + 3) < 0.6f, flats = i == 4;
        if (flats) w = 44, wall = 58;
        for (int k = 0; k < w; k++) {
            int xx = x + k + ox;
            if (xx < 0 || xx >= Canvas::W) continue;
            int roof = gable ? (int)((w / 2 - abs(k - w / 2)) * 0.6f) : 0;
            top[xx] = (int16_t)(horizon - wall - roof);
        }
        if (hash01(i * 5 + 4) < 0.55f && !flats) {  // a TV antenna
            int ax = x + w / 2 + ox, ay = horizon - wall - (gable ? w * 3 / 10 : 0);
            vline(c, ax, ay - 16, ay, 0);
            hline(c, ax - 5, ax + 5, ay - 13, 0);
            hline(c, ax - 3, ax + 3, ay - 9, 0);
        }
        if (flats) {  // windows, a few lit
            for (int r = 0; r < 6; r++)
                for (int q = 0; q < 5; q++) {
                    bool lit = hash01(r * 7 + q * 3 + (f.ms / 9000) * 11) < 0.22f;
                    if (lit) fillRect(c, x + 4 + q * 8 + ox, horizon - wall + 6 + r * 8 + oy - oy, 4, 4, toRGB332(RGB{255, 196, 96}));
                }
        }
        x += w + (int)(hash01(i * 5 + 5) * 6);
    }
    const uint8_t red = toRGB332(RGB{116, 0, 12});
    for (int xx = 0; xx < Canvas::W; xx++)
        for (int y = top[xx] < 0 ? 0 : top[xx]; y < Canvas::H; y++) {
            uint8_t& p = c.rows[y][xx];
            if (p == toRGB332(RGB{255, 196, 96})) continue;  // keep the lit windows
            p = ((xx + (y & 1) * 2) & 3) == 0 && (y & 1) && hash01((uint32_t)(xx * 13 + y * 7)) < 0.5f ? red : 0;
        }
    // Water tower.
    int tx = 196 + ox, ty = horizon - 70;
    fillRect(c, tx - 9, ty, 19, 14, 0);
    for (int k = 0; k < 4; k++) lineAA(c, tx - 7 + k * 5, ty + 14, tx - 9 + k * 6, horizon - 20, RGB{0, 0, 0});

    // Wires from a pole on the left, with crows.
    const int poleX = 26 + ox;
    fillRect(c, poleX - 2, 46 + oy, 4, Canvas::H, 0);
    fillRect(c, poleX - 14, 54 + oy, 28, 3, 0);
    for (int wire = 0; wire < 3; wire++) {
        float y0 = 54.0f + oy, y1 = 70.0f + wire * 9 + oy, sag = 18.0f + wire * 4;
        float x0 = poleX - 12 + wire * 12, x1 = Canvas::W + 20.0f;
        float px = x0, py = y0;
        for (int sgm = 1; sgm <= 20; sgm++) {
            float u = sgm / 20.0f, xx = x0 + (x1 - x0) * u, yy = y0 + (y1 - y0) * u + sag * 4 * u * (1 - u);
            lineAA(c, px, py, xx, yy, RGB{0, 0, 0});
            px = xx, py = yy;
        }
        for (int b = 0; b < 3; b++) {  // crows perched
            if (hash01(wire * 9 + b) < 0.4f) continue;
            float u = 0.2f + 0.6f * hash01(wire * 9 + b + 50);
            int bx = (int)(x0 + (x1 - x0) * u), by = (int)(y0 + (y1 - y0) * u + sag * 4 * u * (1 - u));
            fillRect(c, bx, by - 4, 4, 4, 0);
            fillRect(c, bx + 3, by - 5, 2, 2, 0);
        }
    }
    // Now and then a crow crosses the sky.
    uint32_t flight = f.ms / 15000, ft = f.ms % 15000;
    if (ft < 6000) {
        float u = ft / 6000.0f;
        float bx = -10 + u * (Canvas::W + 20), by = 40 + 30 * hash01(flight) + 8 * sinf(u * 9);
        float flap = sinf(cycle(f.ms, 20.0, 2 * PI_F)) * 3;
        lineAA(c, bx - 5, by - flap, bx, by, RGB{0, 0, 0});
        lineAA(c, bx, by, bx + 5, by - flap, RGB{0, 0, 0});
    }
}

// ---------------------------------------------------------------------------
// Psyche: the clock as a chip, its pins running out as circuit traces.

// One pin's trace, worked out on demand (the render stack is small).
struct Trace {
    float x[4], y[4];
};

bool psycheTrace(int index, int cx, int cy, int hw, int hh, Trace& t) {
    const int PITCH = 8;
    const int across = (2 * hw) / PITCH - 1, down = (2 * hh) / PITCH - 1;
    int side, i;
    if (index < 2 * across) side = index / across, i = index % across + 1;
    else if (index < 2 * across + 2 * down) side = 2 + (index - 2 * across) / down, i = (index - 2 * across) % down + 1;
    else return false;
    float sx, sy, dx, dy;
    if (side == 0) sx = cx - hw + i * PITCH, sy = cy - hh, dx = 0, dy = -1;
    else if (side == 1) sx = cx - hw + i * PITCH, sy = cy + hh, dx = 0, dy = 1;
    else if (side == 2) sx = cx - hw, sy = cy - hh + i * PITCH, dx = -1, dy = 0;
    else sx = cx + hw, sy = cy - hh + i * PITCH, dx = 1, dy = 0;
    uint32_t h = side * 97 + i;
    float l1 = 4 + hash01(h) * 18, l2 = 6 + hash01(h + 1) * 24;
    float jog = hash01(h + 2) < 0.5f ? -1 : 1;
    t.x[0] = sx, t.y[0] = sy;
    t.x[1] = sx + dx * l1, t.y[1] = sy + dy * l1;
    t.x[2] = t.x[1] + dx * l2 + (dx == 0 ? jog * l2 : 0), t.y[2] = t.y[1] + dy * l2 + (dy == 0 ? jog * l2 : 0);
    float run = hash01(h + 3) < 0.3f ? 20 + hash01(h + 4) * 60 : 400;  // some stop short at a via
    t.x[3] = t.x[2] + dx * run, t.y[3] = t.y[2] + dy * run;
    return true;
}

void drawPsyche(Canvas& c, const SaverFrame& f) {
    const RGB board = {6, 16, 12}, copper = {38, 104, 70}, pulse = {190, 255, 225}, hot = {255, 70, 80};
    const int ox = f.ox;
    for (int y = 0; y < Canvas::H; y++) ditherRow(c, y, 0, Canvas::W, board);

    const int cx = Canvas::W / 2 + ox, cy = f.clockY;
    const int hw = f.clockHalfW + 16, hh = f.clockHalfH + 10;
    Trace t;
    for (int i = 0; psycheTrace(i, cx, cy, hw, hh, t); i++) {
        for (int k = 0; k < 3; k++) lineAA(c, t.x[k], t.y[k], t.x[k + 1], t.y[k + 1], copper, 200);
        if (t.x[3] > 0 && t.x[3] < Canvas::W && t.y[3] > 0 && t.y[3] < Canvas::H)
            arc(c, t.x[3], t.y[3], 2.5f, 0, 2 * PI_F, copper, 220);
    }
    // Pulses.
    for (int i = 0; psycheTrace(i, cx, cy, hw, hh, t); i++) {
        if (hash01(i * 31 + 5) < 0.55f) continue;
        float len[3], total = 0;
        for (int k = 0; k < 3; k++) {
            float dx = t.x[k + 1] - t.x[k], dy = t.y[k + 1] - t.y[k];
            len[k] = sqrtf(dx * dx + dy * dy);
            total += len[k];
        }
        if (total > 260) total = 260;  // pulses are off screen by then anyway
        float speed = 30.0f + 50.0f * hash01(i * 7 + 2);
        float d = cycle(f.ms, speed, total + 60) - 30;
        bool red = hash01(i * 3 + 1) < 0.15f;
        for (int tail = 0; tail < 6; tail++) {
            float dd = d - tail * 2.5f;
            if (dd < 0 || dd > total) continue;
            int k = 0;
            while (k < 2 && dd > len[k]) dd -= len[k++];
            float u = len[k] > 0 ? dd / len[k] : 0;
            float x = t.x[k] + (t.x[k + 1] - t.x[k]) * u, y = t.y[k] + (t.y[k + 1] - t.y[k]) * u;
            c.blend((int)x, (int)y, red ? hot : pulse, 256 - tail * 40);
        }
    }
    // The chip.
    fillRect(c, cx - hw, cy - hh, 2 * hw + 1, 2 * hh + 1, toRGB332(RGB{14, 16, 18}));
    frame(c, cx - hw, cy - hh, cx + hw, cy + hh, toRGB332(RGB{110, 120, 120}));
    arc(c, cx - hw + 6, cy + hh - 6, 2.0f, 0, 2 * PI_F, RGB{110, 120, 120});
    textCentered(c, sora_micro, "PSYCHE 07", cx, cy - hh + 10, RGB{130, 140, 140}, 200);
}

// ---------------------------------------------------------------------------
// Crossing: a night zebra crossing under a streetlight, with a pedestrian signal.

void drawCrossing(Canvas& c, const SaverFrame& f) {
    const int ox = f.ox, oy = f.oy;
    const float HZ = 104.0f + oy, VX = Canvas::W * 0.5f + ox;  // horizon and vanishing point
    // Sky and far buildings.
    for (int y = 0; y < (int)HZ; y++) ditherRow(c, y, 0, Canvas::W, mix3(RGB{4, 4, 16}, RGB{18, 16, 40}, RGB{44, 34, 62}, y / HZ));
    for (int x = 0; x < Canvas::W; x++) {
        int h = (int)(8 + 22 * valueNoise(800, (x - ox) * 0.06f) * valueNoise(801, (x - ox) * 0.013f));
        for (int y = (int)HZ - h; y < (int)HZ; y++) c.rows[y][x] = ((x * 7 + y * 3) % 23 == 0 && hash01(x * 5 + y) < 0.3f) ? toRGB332(RGB{220, 180, 90}) : 0;
    }
    // Road under a pool of lamplight: dithered spans, per-pixel work only under the lamp.
    const float poolX = VX - 18, poolY = 186.0f + oy, poolRx = 120.0f, poolRy = 46.0f;
    const RGB asphalt = {34, 32, 38}, stripe = {150, 150, 150}, lamp = {255, 206, 140};
    const RGB pavement = lerp(asphalt, RGB{0, 0, 0}, 120);
    uint16_t dx2[Canvas::W];  // squared distance from the pool's centre, 1/1024ths
    for (int x = 0; x < Canvas::W; x++) {
        float dx = (x - poolX) / poolRx;
        dx2[x] = (uint16_t)(dx * dx > 1.0f ? 1024 : dx * dx * 1024);
    }
    for (int y = (int)HZ; y < Canvas::H; y++) {
        float d = (y - HZ) + 1;  // distance below the horizon
        float half = d * 1.9f;   // road half-width at this row
        bool band = d > 72 && d < 110;  // the zebra crossing's depth
        // Surface per column: 0 pavement, 1 asphalt, 2 stripe.
        uint8_t kind[Canvas::W];
        const RGB base[3] = {pavement, asphalt, stripe};
        int left = (int)ceilf(VX - half), right = (int)floorf(VX + half);
        int prev = 0;
        for (int b = 0; b <= 12; b++) {  // 12 stripe segments across the road
            int edge = b == 12 ? right + 1 : (int)ceilf(VX - half + half * b / 6.0f);
            if (edge > Canvas::W) edge = Canvas::W;
            int k = b == 0 ? 0 : (band && ((b - 1) & 1) ? 2 : 1);
            for (int x = prev < 0 ? 0 : prev; x < edge; x++) kind[x] = (uint8_t)k;
            if (edge > prev) prev = edge;
        }
        for (int x = prev < 0 ? 0 : prev; x < Canvas::W; x++) kind[x] = 0;
        (void)left;
        uint8_t* row = c.rows[y];
        uint8_t pattern[3][4];  // each surface's dither pattern for this row
        for (int k = 0; k < 3; k++)
            for (int q = 0; q < 4; q++) pattern[k][q] = dither332(base[k], q, y);
        for (int x = 0; x < Canvas::W; x++) row[x] = pattern[kind[x]][x & 3];
        float dy = (y - poolY) / poolRy;
        int dy2 = (int)(dy * dy * 1024);
        if (dy2 >= 1024) continue;
        RGB ramp[3][17];
        for (int k = 0; k < 3; k++)
            for (int i = 0; i <= 16; i++) ramp[k][i] = lerp(base[k], lamp, i * 90 / 16);
        for (int x = 0; x < Canvas::W; x++) {
            int e = 1024 - dx2[x] - dy2;
            if (e <= 0) continue;
            row[x] = dither332(ramp[kind[x]][(e * e) >> 16], x, y);
        }
    }
    // Streetlight on the left, its arm over the road.
    const int lx = 40 + ox;
    fillRect(c, lx - 1, 90 + oy, 3, (int)(Canvas::H - 90), 0);
    lineAA(c, lx, 92 + oy, lx + 30, 88 + oy, RGB{0, 0, 0});
    fillRect(c, lx + 26, 86 + oy, 10, 3, toRGB332(lamp));
    // Wires.
    for (int w = 0; w < 2; w++) {
        float y0 = 94.0f + w * 5 + oy, sag = 10;
        float px = lx, py = y0;
        for (int s = 1; s <= 16; s++) {
            float u = s / 16.0f, x = lx + (Canvas::W + 20 - lx) * u, y = y0 - 10 * u + sag * 4 * u * (1 - u);
            lineAA(c, px, py, x, y, RGB{0, 0, 0});
            px = x, py = y;
        }
    }
    // Pedestrian signal on the right: red 12 s, green 7 s, blinking green 3 s.
    const int sx = 214 + ox, sy = 128 + oy;
    fillRect(c, sx + 5, sy + 24, 2, (int)(Canvas::H - sy), 0);
    fillRect(c, sx, sy, 12, 24, toRGB332(RGB{20, 20, 24}));
    uint32_t phase = f.ms % 22000;
    bool red = phase < 12000, green = !red && (phase < 19000 || (phase / 400) % 2);
    const uint8_t r = toRGB332(red ? RGB{255, 40, 40} : RGB{50, 8, 8});
    const uint8_t g = toRGB332(green ? RGB{60, 255, 160} : RGB{8, 40, 24});
    // Standing figure (top), walking figure (bottom), 5 px wide.
    fillRect(c, sx + 5, sy + 2, 2, 2, r);
    fillRect(c, sx + 4, sy + 4, 4, 4, r);
    fillRect(c, sx + 4, sy + 8, 1, 3, r);
    fillRect(c, sx + 7, sy + 8, 1, 3, r);
    fillRect(c, sx + 5, sy + 13, 2, 2, g);
    fillRect(c, sx + 4, sy + 15, 4, 3, g);
    lineAA(c, sx + 5, sy + 18, sx + 3, sy + 22, red ? RGB{8, 40, 24} : (green ? RGB{60, 255, 160} : RGB{8, 40, 24}));
    lineAA(c, sx + 7, sy + 18, sx + 9, sy + 22, red ? RGB{8, 40, 24} : (green ? RGB{60, 255, 160} : RGB{8, 40, 24}));
}

// ---------------------------------------------------------------------------
// VFD: an 80s head unit's vacuum fluorescent display, unlit segments glowing faintly.

void drawVfd(Canvas& c, const SaverFrame& f) {
    const RGB vfd = {90, 255, 215};
    const uint8_t lit = toRGB332(vfd), ghost = dim(vfd, 34), hot = toRGB332(lerp(vfd, RGB{255, 255, 255}, 150));
    const int ox = f.ox, oy = f.oy;

    frame(c, 18 + ox, 104 + oy, 238 + ox, 214 + oy, dim(vfd, 45));

    // Indicator words, each switching on and off on its own slow schedule.
    static const char* WORDS[] = {"ST", "LOUD", "EQ", "DSP", "TP", "AF", "RDM"};
    float x = 30.0f + ox;
    for (int i = 0; i < 7; i++) {
        bool on = i == 0 || hash01(i * 97 + (f.ms / (11000 + i * 1700)) * 13) > 0.55f;
        textAt(c, sora_micro, WORDS[i], x, 119 + oy, vfd, on ? 256 : 40);
        x += textWidth16(sora_micro, WORDS[i]) / 16.0f + 12.0f;
    }

    // Dot-matrix spectrum: 32 columns of 12 dots, two per analyser band.
    const int COLS = 32, ROWS = 12, DOT_W = 4, DOT_H = 3, ROW_PITCH = 5;
    const float left = 28.0f + ox, pitch = 200.0f / COLS;
    const int bottom = 196 + oy;
    const float t = cycle(f.ms, 1.0, 600.0);
    for (int col = 0; col < COLS; col++) {
        int band = col / 2;
        float wobble = 0.9f + 0.2f * valueNoise(400 + col, t * 3.0f);
        int level = (int)lroundf(f.audio->band(band) * wobble * ROWS);
        int peak = (int)lroundf(f.audio->peak(band) * ROWS);
        int dx = (int)lroundf(left + col * pitch);
        for (int r = 0; r < ROWS; r++) {
            uint8_t px = r < level ? lit : (r == peak - 1 ? hot : ghost);
            fillRect(c, dx, bottom - r * ROW_PITCH - DOT_H, DOT_W, DOT_H, px);
        }
    }
    static const struct { int col; const char* label; } AXIS[] = {
        {1, "63"}, {8, "250"}, {16, "1K"}, {24, "4K"}, {30, "16K"}};
    for (const auto& a : AXIS) textCentered(c, sora_micro, a.label, left + a.col * pitch + 2, 207 + oy, vfd, 90);
}

// ---------------------------------------------------------------------------
// Wired: utility poles and wires at dusk, after Serial Experiments Lain.

struct Pole {
    float x, top, width;
    float arms[2];          // crossarm heights
    float armHalf[2];       // crossarm half-widths
};

void wirePoint(const Pole& p, int i, float ox, float oy, float& x, float& y) {
    // Five insulators: three on the upper arm, two on the lower.
    static const float UPPER[3] = {-0.9f, -0.35f, 0.9f}, LOWER[2] = {-0.8f, 0.8f};
    if (i < 3) {
        x = p.x + UPPER[i] * p.armHalf[0] + ox;
        y = p.arms[0] - 1 + oy;
    } else {
        x = p.x + LOWER[i - 3] * p.armHalf[1] + ox;
        y = p.arms[1] - 1 + oy;
    }
}

// A sagging wire as a quadratic: point at t along it.
void wireAt(float x0, float y0, float x1, float y1, float sag, float t, float& x, float& y) {
    x = x0 + (x1 - x0) * t;
    y = y0 + (y1 - y0) * t + sag * 4.0f * t * (1.0f - t);
}

void drawWired(Canvas& c, const SaverFrame& f) {
    const int ox = f.ox, oy = f.oy;
    const int horizon = 198 + oy;
    const RGB wireColor = {6, 6, 14};

    // Sky.
    for (int y = 0; y < horizon && y < Canvas::H; y++) {
        float u = (float)y / horizon;
        ditherRow(c, y, 0, Canvas::W, mix3(RGB{4, 6, 20}, RGB{36, 40, 72}, RGB{104, 76, 88}, u));
    }

    // Static: a sprinkle of noise, new every couple of frames.
    uint32_t frameNo = f.ms / 40;
    for (int i = 0; i < 170; i++) {
        uint32_t h = frameNo * 7919u + i * 104729u;
        int x = (int)(hash01(h) * Canvas::W), y = (int)(hash01(h + 1) * horizon);
        if (y >= 0 && y < Canvas::H) c.blend(x, y, RGB{220, 225, 240}, 40 + (int)(hash01(h + 2) * 90));
    }

    // Poles: a near one on the right, a middle one on the left, a far one.
    const Pole poles[3] = {
        {224.0f, 6.0f, 6.0f, {40.0f, 58.0f}, {30.0f, 24.0f}},
        {44.0f, 56.0f, 4.0f, {72.0f, 84.0f}, {18.0f, 15.0f}},
        {150.0f, 150.0f, 2.0f, {156.0f, 162.0f}, {9.0f, 7.0f}},
    };
    for (int pi = 2; pi >= 0; pi--) {
        const Pole& p = poles[pi];
        fillRect(c, (int)lroundf(p.x - p.width * 0.5f) + ox, (int)p.top + oy, (int)p.width, horizon - (int)p.top - oy + 1, 0);
        for (int a = 0; a < 2; a++) {
            int w = (int)lroundf(p.armHalf[a] * 2), h = pi == 0 ? 3 : 2;
            fillRect(c, (int)lroundf(p.x - p.armHalf[a]) + ox, (int)p.arms[a] + oy, w, h, 0);
        }
        for (int i = 0; i < 5; i++) {  // insulators
            float x, y;
            wirePoint(p, i, ox, oy, x, y);
            fillRect(c, (int)x - (pi == 0 ? 1 : 0), (int)y - (pi == 0 ? 2 : 1), pi == 0 ? 2 : 1, pi == 0 ? 3 : 2, 0);
        }
    }

    // Wires: near pole to middle pole, then off each side.
    struct Wire { float x0, y0, x1, y1, sag; };
    Wire wires[20];
    int n = 0;
    for (int i = 0; i < 5; i++) {
        float ax, ay, bx, by, cx, cy;
        wirePoint(poles[0], i, ox, oy, ax, ay);
        wirePoint(poles[1], i, ox, oy, bx, by);
        wires[n++] = {bx, by, ax, ay, 20.0f + i * 2.0f};
        wires[n++] = {ax, ay, Canvas::W + 30.0f, ay - 10.0f + i, 4.0f};
        wires[n++] = {-30.0f, by + 14.0f, bx, by, 6.0f};
        if (i < 3 || i == 4) {
            wirePoint(poles[2], i, ox, oy, cx, cy);
            if (n < 20) wires[n++] = {-30.0f, cy + 4.0f, Canvas::W + 30.0f, cy - 6.0f, 10.0f};
        }
    }
    for (int w = 0; w < n; w++) {
        const Wire& wr = wires[w];
        float px = wr.x0, py = wr.y0;
        for (int s = 1; s <= 24; s++) {
            float x, y;
            wireAt(wr.x0, wr.y0, wr.x1, wr.y1, wr.sag, s / 24.0f, x, y);
            lineAA(c, px, py, x, y, wireColor, 230);
            px = x, py = y;
        }
    }

    // Signals: pulses with fading tails running along some of the wires.
    for (int w = 0; w < n; w++) {
        if (hash01(w * 31 + 7) < 0.35f) continue;
        const Wire& wr = wires[w];
        float speed = 0.10f + 0.25f * hash01(w * 13 + 1);
        float head = cycle(f.ms, speed, 1.6) - 0.3f;  // spends a while off the ends
        bool red = hash01(w * 7 + 3) < 0.2f;
        RGB col = red ? RGB{255, 60, 70} : RGB{215, 235, 255};
        for (int k = 0; k < 10; k++) {
            float t = head - k * 0.012f;
            if (t < 0 || t > 1) continue;
            float x, y;
            wireAt(wr.x0, wr.y0, wr.x1, wr.y1, wr.sag, t, x, y);
            c.blend((int)lroundf(x), (int)lroundf(y), col, 256 - k * 24);
        }
    }

    // Ground, with its shadows dotted red.
    fillRect(c, 0, horizon, Canvas::W, Canvas::H - horizon, 0);
    const uint8_t red = toRGB332(RGB{120, 0, 10});
    for (int y = horizon + 2; y < Canvas::H; y += 3)
        for (int x = ((y / 3) & 1) * 2 + ox; x < Canvas::W; x += 4)
            if (x >= 0 && hash01((uint32_t)(x * 7 + y * 131)) < 0.45f) c.rows[y][x] = red;

    textAt(c, sora_micro, "present day", 22 + ox, horizon + 13, RGB{200, 60, 70}, 150);
    textAt(c, sora_micro, "present time", 22 + ox, horizon + 23, RGB{200, 60, 70}, 150);
}

// Every ~7 s, for a few frames, a band of the picture slips sideways.
void glitchWired(Canvas& c, const SaverFrame& f) {
    uint32_t slot = f.ms / 7000, inSlot = f.ms % 7000;
    if (inSlot > 140 || hash01(slot * 3 + 1) < 0.3f) return;
    int y0 = (int)(hash01(slot * 5 + 2) * 200) + 10, h = 4 + (int)(hash01(slot * 5 + 3) * 14);
    int shift = 3 + (int)(hash01(slot * 5 + 4 + inSlot / 40) * 9);
    if (hash01(slot * 5 + 5) < 0.5f) shift = -shift;
    uint8_t tmp[Canvas::W];
    for (int y = y0; y < y0 + h && y < Canvas::H; y++) {
        for (int x = 0; x < Canvas::W; x++) tmp[(x + shift + Canvas::W) % Canvas::W] = c.rows[y][x];
        memcpy(c.rows[y], tmp, Canvas::W);
    }
}

// ---------------------------------------------------------------------------
// Haze: Gen X soft club. Pastel haze, a glowing orb, rising particles, flyer marks.

void drawHaze(Canvas& c, const SaverFrame& f) {
    const int ox = f.ox, oy = f.oy;
    const RGB ink = {38, 42, 98};
    const float t = cycle(f.ms, 1.0, 3600.0);

    RGB bg[Canvas::H];
    for (int y = 0; y < Canvas::H; y++)
        bg[y] = mix3(RGB{118, 136, 190}, RGB{158, 156, 200}, RGB{204, 168, 200}, (float)y / (Canvas::H - 1));
    for (int y = 0; y < Canvas::H; y++) ditherRow(c, y, 0, Canvas::W, bg[y]);

    // Orb: a soft glow, drawn only over the pixels it covers.
    const float orbX = 128.0f + 46.0f * sinf(t * 0.043f), orbY = 96.0f + 22.0f * sinf(t * 0.031f + 1.0f);
    const float R = 78.0f, rx = R / DISPLAY_PIXEL_ASPECT;
    const RGB glow = {250, 236, 248};
    int xa = (int)(orbX - rx), xb = (int)(orbX + rx);
    if (xa < 0) xa = 0;
    if (xb > Canvas::W) xb = Canvas::W;
    // Squared distances in 1/1024ths, so the inner loop is integer only.
    uint16_t dx2[Canvas::W];
    for (int x = xa; x < xb; x++) {
        float dx = (x - orbX) / rx;
        dx2[x] = (uint16_t)(dx * dx > 1.0f ? 1024 : dx * dx * 1024.0f);
    }
    RGB ramp[33];
    for (int y = (int)(orbY - R); y < (int)(orbY + R); y++) {
        if (y < 0 || y >= Canvas::H) continue;
        float dy = (y - orbY) / R;
        int dy2 = (int)(dy * dy * 1024.0f);
        // This row's chord of the circle.
        float w = sqrtf(dy2 < 1024 ? 1.0f - dy2 / 1024.0f : 0) * rx;
        int x0 = (int)(orbX - w), x1 = (int)(orbX + w) + 1;
        if (x0 < xa) x0 = xa;
        if (x1 > xb) x1 = xb;
        if (x0 >= x1) continue;
        // Glow strength (1 - d^2)^2, in 33 steps from this row's background.
        for (int i = 0; i <= 32; i++) ramp[i] = lerp(bg[y], glow, i * 140 / 32);
        uint8_t* row = c.rows[y];
        for (int x = x0; x < x1; x++) {
            int d = 1024 - dx2[x] - dy2;
            if (d <= 0) continue;
            row[x] = dither332(ramp[(d * d) >> 15], x, y);
        }
    }

    // Orbit rings and a short bright arc travelling round the outer one.
    const float cx = (float)f.clockX, cy = (float)f.clockY;
    arc(c, cx, cy, 50.0f + 2.0f * sinf(t * 0.7f), 0, 2 * PI_F, RGB{255, 255, 255}, 120);
    arc(c, cx, cy, 64.0f + 2.0f * sinf(t * 0.5f + 1.0f), 0, 2 * PI_F, RGB{255, 255, 255}, 80);
    float a0 = cycle(f.ms, 0.35, 2 * PI_F);
    arc(c, cx, cy, 64.0f + 2.0f * sinf(t * 0.5f + 1.0f), a0, a0 + 0.9f, RGB{255, 255, 255}, 170);

    // Particles floating up, swaying.
    for (int i = 0; i < 110; i++) {
        float speed = 4.0f + 10.0f * hash01(i * 3 + 1);
        float x = fmodf(hash01(i * 3 + 2) * Canvas::W + 6.0f * sinf(t * 0.4f + i), (float)Canvas::W);
        float y = Canvas::H - fmodf(hash01(i * 3 + 3) * Canvas::H + t * speed, (float)Canvas::H);
        int a = 90 + (int)(140 * hash01(i * 3 + 4));
        int xi = (int)x, yi = (int)y;
        c.blend(xi, yi, RGB{255, 255, 255}, a);
        if (speed > 11.0f) c.blend(xi + 1, yi, RGB{255, 255, 255}, a / 2);
    }

    // Fine print, registration marks and a ruler ticking through the minute.
    char buf[24];
    uint32_t s = f.ms / 1000;
    snprintf(buf, sizeof(buf), "UP %02u:%02u:%02u", (unsigned)(s / 3600 % 100), (unsigned)(s / 60 % 60), (unsigned)(s % 60));
    textAt(c, sora_micro, "SYS \xE2\x80\x94 STANDBY", 24 + ox, 25 + oy, ink, 200);
    textRight(c, sora_micro, buf, 232 + ox, 25 + oy, ink, 200);
    textAt(c, sora_micro, "SOURCE \xC2\xB7 SPOTIFY", 24 + ox, 222 + oy, ink, 200);
    textRight(c, sora_micro, "LMD \xE2\x80\x94 9020", 232 + ox, 222 + oy, ink, 200);
    const int marks[4][2] = {{18, 16}, {238, 16}, {18, 226}, {238, 226}};
    for (const auto& m : marks) {
        hline(c, m[0] - 3 + ox, m[0] + 3 + ox, m[1] + oy, toRGB332(ink));
        vline(c, m[0] + ox, m[1] - 4 + oy, m[1] + 4 + oy, toRGB332(ink));
    }
    const int ry = 206 + oy;
    hline(c, 24 + ox, 232 + ox, ry, dim(ink, 200));
    for (int i = 0; i <= 12; i++) vline(c, 24 + ox + i * 208 / 12, ry - (i % 3 ? 2 : 4), ry - 1, dim(ink, 200));
    float sec = (f.ms % 60000) / 60000.0f;
    int mx = 24 + ox + (int)(sec * 208);
    fillRect(c, mx - 1, ry - 6, 3, 5, toRGB332(ink));
}


// ---------------------------------------------------------------------------
// Shared by the scenes below. They keep to exact colours and neutral lines, which composite
// shows cleanly, rather than dithered fills.

// One-pixel line in a solid colour: far cheaper than lineAA, and crisp on composite.
void line1(Canvas& c, int x0, int y0, int x1, int y1, uint8_t col) {
    const int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1, dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    for (int err = dx + dy;;) {
        if ((unsigned)x0 < (unsigned)Canvas::W && (unsigned)y0 < (unsigned)Canvas::H) c.rows[y0][x0] = col;
        if (x0 == x1 && y0 == y1) break;
        const int e2 = 2 * err;
        if (e2 >= dy) err += dy, x0 += sx;
        if (e2 <= dx) err += dx, y0 += sy;
    }
}

// Per channel maximum of two RGB332 colours: light adding up without leaving the palette.
inline uint8_t max332(uint8_t a, uint8_t b) {
    int r = (a & 0xE0) > (b & 0xE0) ? (a & 0xE0) : (b & 0xE0);
    int g = (a & 0x1C) > (b & 0x1C) ? (a & 0x1C) : (b & 0x1C);
    int bl = (a & 0x03) > (b & 0x03) ? (a & 0x03) : (b & 0x03);
    return (uint8_t)(r | g | bl);
}

// ---------------------------------------------------------------------------
// Rain: a bus window at night. City lights blur behind the glass; drops run down it.

void drawRain(Canvas& c, const SaverFrame& f) {
    static const RGB LIGHTS[6] = {{255, 214, 160}, {255, 178, 80}, {255, 150, 50},
                                  {200, 214, 255}, {255, 90, 70},   {150, 240, 170}};
    const float t = cycle(f.ms, 1.0, 100000.0), pulse = cycle(f.ms, 0.35, 2 * PI_F);

    // A few distant points of light high up: windows, a radio mast.
    for (int i = 0; i < 10; i++) {
        const int x = (int)fmodf(hash01(i * 3 + 40) * 300 + t * 1.2f, 300.0f) - 20, y = 20 + (int)(hash01(i * 3 + 41) * 70) + f.oy;
        if (x >= 0 && x < Canvas::W - 1 && calm(f, x, y) > 0.8f) c.rows[y][x] = c.rows[y][x + 1] = 0x92;
    }
    // Out-of-focus street lights sliding past (nearer, bigger ones faster): flat discs whose
    // outer edge is dimmer, so they read soft without dithering.
    for (int i = 0; i < 13; i++) {
        const float r = 9 + hash01(i * 5 + 1) * 17, rx = r / DISPLAY_PIXEL_ASPECT;
        const float span = Canvas::W + 2 * rx + 40;
        const float cx = fmodf(hash01(i * 5 + 3) * span + t * (2.0f + r * 0.3f), span) - rx - 20;
        const float cy = 128 + hash01(i * 5 + 4) * 82 + f.oy;
        const RGB col = LIGHTS[(int)(hash01(i * 5 + 5) * 5.99f)];
        float glow = (0.45f + 0.3f * hash01(i * 7 + 9)) * (0.9f + 0.1f * fastSin(pulse + i * 1.7f));
        glow *= 0.3f + 0.7f * calm(f, (int)cx, (int)cy);
        const uint8_t core = toRGB332Clean(lerp(BLACK, col, (int)(glow * 256)));
        const uint8_t edge = toRGB332Clean(lerp(BLACK, col, (int)(glow * 0.55f * 256)));
        const int y0 = (int)ceilf(cy - r), y1 = (int)floorf(cy + r);
        for (int y = y0 < 0 ? 0 : y0; y <= y1 && y < Canvas::H; y++) {
            const float dy = (y - cy) / r;
            const float hw = rx * sqrtf(fmaxf(0.0f, 1 - dy * dy)), hwIn = hw - rx * 0.16f;
            const float hwCore = fabsf(dy) < 0.86f ? hwIn : -1;
            const int xa = (int)ceilf(cx - hw), xb = (int)floorf(cx + hw);
            uint8_t* row = c.rows[y];
            for (int x = xa < 0 ? 0 : xa; x <= xb && x < Canvas::W; x++)
                row[x] = max332(row[x], fabsf(x - cx) <= hwCore ? core : edge);
        }
    }

    // Beads of water on the glass, a few changing at a time.
    for (int i = 0; i < 64; i++) {
        const uint32_t period = 40000;
        const uint32_t slot = (f.ms + (uint32_t)(hash01(i * 3 + 11) * period)) / period;
        const uint32_t h = i * 2654435761u + slot * 40503u;
        const int x = (int)(hash01(h) * (Canvas::W - 2)), y = (int)(hash01(h + 1) * (Canvas::H - 1));
        if (calm(f, x, y) < 0.6f) continue;
        c.rows[y][x] = max332(c.rows[y][x], 0x49);
        c.rows[y][x + 1] = max332(c.rows[y][x + 1], 0x49);
    }

    // Drops that let go and run down, stopping and starting, leaving a wet trail.
    for (int d = 0; d < 9; d++) {
        const uint32_t period = 9000 + (uint32_t)(hash01(d * 3 + 1) * 8000);
        const uint32_t life = f.ms + (uint32_t)(hash01(d * 3 + 2) * period);
        const float a = (life % period) * 0.001f;  // seconds since it let go
        const uint32_t s = d * 7919u + (life / period) * 104729u;
        const float x0 = 10 + hash01(s) * (Canvas::W - 20), y0 = hash01(s + 1) * 100;
        const float speed = 20 + hash01(s + 2) * 24;
        const float head = y0 + speed * (a + 0.28f * sinf(a * 3.0f));  // never runs backwards
        auto xAt = [&](float y) { return x0 + 5.0f * (valueNoise(s + 3, y * 0.05f) - 0.5f); };
        for (int y = (int)y0; y < (int)head && y < Canvas::H; y++) {
            const float back = head - y;
            if (y < 0 || back > 90 || (back > 40 && (y & 1))) continue;
            const int x = (int)(xAt((float)y) + 0.5f);
            if (x < 0 || x >= Canvas::W || calm(f, x, y) < 0.3f) continue;
            c.rows[y][x] = max332(c.rows[y][x], back < 14 ? 0x92 : 0x49);
        }
        const int hx = (int)(xAt(head) + 0.5f), hy = (int)head;
        if (hx < 0 || hx >= Canvas::W - 1 || hy < 0 || hy >= Canvas::H - 1 || calm(f, hx, hy) < 0.3f) continue;
        c.rows[hy][hx] = 0xFF;
        c.rows[hy][hx + 1] = c.rows[hy + 1][hx] = c.rows[hy + 1][hx + 1] = 0xB6;
    }
}

// ---------------------------------------------------------------------------
// Contours: a survey map of a landscape that slowly reshapes itself.

// Landscape height, 0..1: two octaves of value noise, each changing over time. The lattice
// values are worked out once per frame, so a point costs two bilinear lookups.
struct Terrain {
    static constexpr int W1 = 7, H1 = 6, W2 = 14, H2 = 11;
    static constexpr float F1 = 0.014f * DISPLAY_PIXEL_ASPECT, G1 = 0.014f;
    static constexpr float F2 = 0.034f * DISPLAY_PIXEL_ASPECT, G2 = 0.034f;
    uint16_t a[H1 * W1], b[H2 * W2];

    explicit Terrain(float t) {
        fill(a, W1, H1, 31, t);
        fill(b, W2, H2, 37, t * 1.6f);
    }
    static void fill(uint16_t* out, int w, int h, uint32_t seed, float t) {
        const float ft = floorf(t);
        float u = t - ft;
        u = u * u * (3 - 2 * u);
        const uint32_t it = (uint32_t)(int32_t)ft;
        const uint32_t s0 = (seed + it * 7919u) * 0x9E3779B1u, s1 = (seed + (it + 1) * 7919u) * 0x9E3779B1u;
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                const uint32_t k = ((uint32_t)x * 0x85EBCA77u) ^ ((uint32_t)y * 0xC2B2AE3Du);
                const float v0 = hash01(s0 ^ k), v1 = hash01(s1 ^ k);
                out[y * w + x] = (uint16_t)((v0 + (v1 - v0) * u) * 65535);
            }
    }
    static float sample(const uint16_t* l, int w, float x, float y) {
        const int ix = (int)x, iy = (int)y;
        float u = x - ix, v = y - iy;
        u = u * u * (3 - 2 * u);
        v = v * v * (3 - 2 * v);
        const uint16_t* r0 = l + iy * w + ix;
        const uint16_t* r1 = r0 + w;
        const float top = r0[0] + (r0[1] - r0[0]) * u, bottom = r1[0] + (r1[1] - r1[0]) * u;
        return (top + (bottom - top) * v) * (1.0f / 65535);
    }
    float at(float x, float y) const {
        return 0.78f * sample(a, W1, x * F1, y * G1) + 0.22f * sample(b, W2, x * F2, y * G2);
    }
};

void drawContours(Canvas& c, const SaverFrame& f) {
    constexpr int CELL = 8, NX = Canvas::W / CELL + 1, NY = Canvas::H / CELL + 1;
    constexpr float INTERVAL = 0.06f;
    const float t = cycle(f.ms, 1.0 / 45, 100000.0);  // a new landscape every 45 s or so
    const RGB major = {236, 236, 236};

    const Terrain land(t);
    float prev[NX], cur[NX];
    for (int j = 0; j < NY; j++) {
        for (int i = 0; i < NX; i++) cur[i] = land.at((float)(i * CELL), (float)(j * CELL));
        for (int i = 0; j > 0 && i + 1 < NX; i++) {
            const float x0 = (float)(i * CELL), y0 = (float)((j - 1) * CELL), x1 = x0 + CELL, y1 = y0 + CELL;
            const float h00 = prev[i], h10 = prev[i + 1], h01 = cur[i], h11 = cur[i + 1];
            const float lo = fminf(fminf(h00, h10), fminf(h01, h11)), hi = fmaxf(fmaxf(h00, h10), fmaxf(h01, h11));
            const int k0 = (int)ceilf(lo / INTERVAL), k1 = (int)floorf(hi / INTERVAL);
            if (k0 > k1) continue;
            const float quiet = calm(f, (int)(x0 + CELL / 2), (int)(y0 + CELL / 2));
            if (quiet < 0.02f) continue;
            for (int k = k0; k <= k1; k++) {
                // Marching squares: where this level crosses the cell's four edges.
                const float lv = k * INTERVAL;
                float px[4], py[4];
                int n = 0;
                auto edge = [&](float ha, float hb, float ax, float ay, float bx, float by) {
                    if ((ha < lv) == (hb < lv)) return;
                    const float s = (lv - ha) / (hb - ha);
                    px[n] = ax + (bx - ax) * s, py[n] = ay + (by - ay) * s, n++;
                };
                edge(h00, h10, x0, y0, x1, y0);
                edge(h10, h11, x1, y0, x1, y1);
                edge(h11, h01, x1, y1, x0, y1);
                edge(h01, h00, x0, y1, x0, y0);
                for (int m = 0; m + 1 < n; m += 2) {
                    if (k % 5 == 0)  // index contours: bright and smooth
                        lineAA(c, px[m], py[m], px[m + 1], py[m + 1], major, (int)(256 * quiet));
                    else if (quiet > 0.25f)  // the rest: plain grey pixels
                        line1(c, (int)(px[m] + 0.5f), (int)(py[m] + 0.5f), (int)(px[m + 1] + 0.5f),
                              (int)(py[m + 1] + 0.5f), quiet > 0.6f ? 0x92 : 0x49);
                }
            }
        }
        memcpy(prev, cur, sizeof(cur));
    }

    // Spot heights, resurveyed now and then.
    const uint32_t slot = f.ms / 40000;
    for (int s = 0; s < 4; s++) {
        const int x = 20 + (int)(hash01(slot * 13 + s * 3 + 1) * (Canvas::W - 70));
        const int y = 24 + (int)(hash01(slot * 13 + s * 3 + 2) * (Canvas::H - 70));
        if (calm(f, x, y) < 1.0f) continue;
        char label[8], *d = label + sizeof(label) - 1;  // the height, without snprintf's stack
        *d = 0;
        for (int v = (int)(land.at((float)x, (float)y) * 2400); d == label + sizeof(label) - 1 || v; v /= 10)
            *--d = (char)('0' + v % 10);
        const int w = textWidth16(sora_micro, d) / 16;
        fillRect(c, x - 3, y - 3, w + 11, 8, 0);
        hline(c, x - 2, x + 2, y, 0xFF);
        vline(c, x, y - 2, y + 2, 0xFF);
        textAt(c, sora_micro, d, (float)(x + 5), y + 3, RGB{200, 200, 200});
    }

    // Fine print and a scale bar in a footer strip.
    const int by = Canvas::H - 9 + f.oy, bx = 14 + f.ox;
    fillRect(c, 0, by - 12, Canvas::W, Canvas::H - (by - 12), 0);
    hline(c, 8, Canvas::W - 9, by - 13, 0x49);
    textAt(c, sora_micro, "CONTOUR INTERVAL 100 M", (float)bx, by, RGB{150, 150, 150});
    for (int k = 0; k <= 4; k++) vline(c, Canvas::W - 74 + f.ox + k * 15, by - 6, by - (k % 2 ? 4 : 3), 0x92);
    hline(c, Canvas::W - 74 + f.ox, Canvas::W - 14 + f.ox, by - 3, 0x92);
    textRight(c, sora_micro, "2 KM", (float)(Canvas::W - 14 + f.ox), by + 7, RGB{150, 150, 150});
}

// ---------------------------------------------------------------------------
// Currents: a wind map. Fine streaks ride a slowly turning flow.

struct FlowWave {
    float kx, ky, w, a, ph;
};
const FlowWave FLOW[4] = {
    {0.021f, 0.012f, 0.050f, 1.00f, 0.0f},
    {-0.010f, 0.026f, -0.037f, 0.85f, 1.7f},
    {0.031f, -0.019f, 0.029f, 0.60f, 4.1f},
    {-0.024f, -0.033f, -0.061f, 0.45f, 2.6f},
};

// A steady breeze plus eddies: the curl of a sum of waves, so it neither gathers nor thins.
inline void flowAt(float x, float y, float t, float& vx, float& vy) {
    vx = 0.55f, vy = 0.0f;
    for (const FlowWave& w : FLOW) {
        const float k = fastCos(w.kx * x + w.ky * y + w.w * t + w.ph) * w.a * 40.0f;
        vx += k * w.ky;
        vy -= k * w.kx;
    }
}

void drawCurrents(Canvas& c, const SaverFrame& f) {
    constexpr int N = 110, STEPS = 34;
    constexpr float STEP = 3.2f, TAIL = 12.0f;
    constexpr uint32_t LIFE = 7000;
    const float t = cycle(f.ms, 1.0, 100000.0);
    const float worldW = Canvas::W * DISPLAY_PIXEL_ASPECT;
    for (int p = 0; p < N; p++) {
        const uint32_t life = f.ms + (uint32_t)(hash01(p * 2 + 1) * LIFE);
        const float age = (life % LIFE) / (float)LIFE;
        const uint32_t s = p * 7919u + (life / LIFE) * 104729u;
        float x = hash01(s) * (worldW + 60) - 60, y = hash01(s + 1) * Canvas::H;
        const float headAt = age * STEPS;
        const int last = (int)headAt + 1;
        const float env = age < 0.15f ? age / 0.15f : (age > 0.8f ? (1 - age) / 0.2f : 1.0f);
        float px = x / DISPLAY_PIXEL_ASPECT, py = y;
        for (int k = 1; k <= last && k <= STEPS; k++) {
            float vx, vy;
            flowAt(x, y, t, vx, vy);
            const float inv = STEP / sqrtf(vx * vx + vy * vy + 1e-4f);
            x += vx * inv, y += vy * inv;
            float nx = x / DISPLAY_PIXEL_ASPECT, ny = y;
            const float back = headAt - k;  // segments behind the head
            if (back < 0) {  // the head, part way along this step
                const float u = 1 + back;
                nx = px + (nx - px) * u, ny = py + (ny - py) * u;
            }
            if (back < TAIL) {
                const float b = env * (1 - (back < 0 ? 0 : back) / TAIL) * calm(f, (int)nx, (int)ny);
                if (back < 2 && b > 0.03f)  // the head: smooth
                    lineAA(c, px, py, nx, ny, RGB{255, 255, 255}, (int)(b * 256));
                else if (b > 0.12f)  // the tail: plain greys, dimming
                    line1(c, (int)(px + 0.5f), (int)(py + 0.5f), (int)(nx + 0.5f), (int)(ny + 0.5f),
                          b > 0.6f ? 0xB6 : (b > 0.33f ? 0x92 : 0x49));
            }
            px = nx, py = ny;
        }
    }
}

}  // namespace

SaverStyle saverStyle(int id) {
    switch (id) {
        case SAVER_ID_VINYL: return {62, RGB{255, 255, 255}, false};
        case SAVER_ID_SCOPE: return {Canvas::H / 2, RGB{200, 255, 210}, false};
        case SAVER_ID_TERMINAL: return {108, RGB{200, 240, 215}, false};
        case SAVER_ID_STATIC: return {Canvas::H / 2, RGB{240, 240, 245}, false};
        case SAVER_ID_NAVI: return {110, RGB{230, 232, 245}, false};
        case SAVER_ID_REDSKY: return {66, RGB{255, 232, 228}, false};
        case SAVER_ID_PSYCHE: return {Canvas::H / 2, RGB{210, 230, 220}, false};
        case SAVER_ID_CROSSING: return {50, RGB{235, 232, 245}, false};
        case SAVER_ID_RIDGES: return {52, RGB{255, 255, 255}, false};
        case SAVER_ID_VFD: return {70, RGB{110, 255, 222}, true};
        case SAVER_ID_WIRED: return {104, RGB{232, 234, 242}, false};
        case SAVER_ID_HAZE: return {116, RGB{38, 42, 98}, false};
        case SAVER_ID_RAIN: return {74, RGB{255, 240, 226}, false};
        case SAVER_ID_CONTOURS: return {Canvas::H / 2, RGB{240, 240, 240}, false};
        default: return {Canvas::H / 2, RGB{255, 255, 255}, false};  // waves, sphere
    }
}

void drawSaver(int id, Canvas& c, const SaverFrame& f) {
    switch (id) {
        case SAVER_ID_SPHERE: drawSphere(c, f); break;
        case SAVER_ID_VINYL: drawVinyl(c, f); break;
        case SAVER_ID_RIDGES: drawRidges(c, f); break;
        case SAVER_ID_NETWORK: drawNetwork(c, f); break;
        case SAVER_ID_TUNNEL: drawTunnel(c, f); break;
        case SAVER_ID_SCOPE: drawScope(c, f); break;
        case SAVER_ID_TERMINAL: drawTerminal(c, f); break;
        case SAVER_ID_STATIC: drawStatic(c, f); break;
        case SAVER_ID_NAVI: drawNavi(c, f); break;
        case SAVER_ID_REDSKY: drawRedSky(c, f); break;
        case SAVER_ID_PSYCHE: drawPsyche(c, f); break;
        case SAVER_ID_CROSSING: drawCrossing(c, f); break;
        case SAVER_ID_VFD: drawVfd(c, f); break;
        case SAVER_ID_WIRED: drawWired(c, f); break;
        case SAVER_ID_HAZE: drawHaze(c, f); break;
        case SAVER_ID_RAIN: drawRain(c, f); break;
        case SAVER_ID_CONTOURS: drawContours(c, f); break;
        case SAVER_ID_CURRENTS: drawCurrents(c, f); break;
        default: drawWaves(c, f); break;
    }
}

void drawSnow(Canvas& c, uint32_t ms) {
    // Black to white, so every pixel of the LCD keeps swinging.
    static const uint8_t SNOW[8] = {0x00, 0x00, 0x00, 0x49, 0x92, 0xB6, 0xFF, 0xFF};
    uint32_t seed = (ms / 33) * 2654435761u + 977u;
    for (int y = 0; y < Canvas::H; y++) {
        uint8_t* row = c.rows[y];
        for (int x = 0; x < Canvas::W; x += 4) {
            seed ^= seed << 13;
            seed ^= seed >> 17;
            seed ^= seed << 5;
            uint32_t word = SNOW[seed & 7] | (SNOW[(seed >> 3) & 7] << 8) | (SNOW[(seed >> 6) & 7] << 16) |
                            ((uint32_t)SNOW[(seed >> 9) & 7] << 24);
            memcpy(row + x, &word, 4);
        }
    }
}

void drawSaverOverlay(int id, Canvas& c, const SaverFrame& f) {
    if (id == SAVER_ID_WIRED) glitchWired(c, f);
}
