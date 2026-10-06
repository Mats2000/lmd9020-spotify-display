#include "color.h"

#include <math.h>
#include <string.h>

RGB hsv(float h, float s, float v) {
    h = fmodf(h, 360.0f);
    if (h < 0) h += 360.0f;
    float c = v * s;
    float x = c * (1.0f - fabsf(fmodf(h / 60.0f, 2.0f) - 1.0f));
    float m = v - c;
    float r, g, b;
    switch ((int)(h / 60.0f)) {
        case 0: r = c, g = x, b = 0; break;
        case 1: r = x, g = c, b = 0; break;
        case 2: r = 0, g = c, b = x; break;
        case 3: r = 0, g = x, b = c; break;
        case 4: r = x, g = 0, b = c; break;
        default: r = c, g = 0, b = x; break;
    }
    return RGB{(uint8_t)lroundf((r + m) * 255), (uint8_t)lroundf((g + m) * 255),
               (uint8_t)lroundf((b + m) * 255)};
}

static float linear(uint8_t c) { return powf(c / 255.0f, 2.2f); }

float luminance(RGB c) {
    return 0.2126f * linear(c.r) + 0.7152f * linear(c.g) + 0.0722f * linear(c.b);
}

// Distance as composite shows it: brightness error, plus colour error weighted by chromaWeight.
static long compositeDistance(RGB a, RGB b, int chromaWeight) {
    long dr = a.r - b.r, dg = a.g - b.g, db = a.b - b.b;
    long dy = (77 * dr + 150 * dg + 29 * db) >> 8;
    long du = (126 * (db - dy)) >> 8, dv = (224 * (dr - dy)) >> 8;
    return dy * dy + chromaWeight * (du * du + dv * dv);
}

static uint8_t nearest(RGB c, int chromaWeight) {
    uint8_t best = 0;
    long bestD = 0x7fffffff;
    for (int i = 0; i < 256; i++) {
        long d = compositeDistance(c, fromRGB332((uint8_t)i), chromaWeight);
        if (d < bestD) bestD = d, best = (uint8_t)i;
    }
    return best;
}

uint8_t nearestRGB332(RGB c) { return nearest(c, 3); }

// RGB332 has no true greys between black and white; these are the least tinted ones.
GreyTable::GreyTable() {
    for (int v = 0; v < 256; v++) entry[v] = nearest(RGB{(uint8_t)v, (uint8_t)v, (uint8_t)v}, 6);
}
GreyTable GREY332;

void ArtStats::reset() { memset(this, 0, sizeof(*this)); }

void ArtStats::add(uint8_t r, uint8_t g, uint8_t b) {
    int mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
    int mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
    int ch = mx - mn;
    count++;
    lumSum += (0.2126f * r + 0.7152f * g + 0.0722f * b) / 255.0f;
    chromaSum += ch / 255.0f;
    if (ch < 24 || mx < 40) return;  // greys and near-black carry no usable hue

    float h;
    if (mx == r)
        h = 60.0f * (g - b) / ch;
    else if (mx == g)
        h = 60.0f * (b - r) / ch + 120.0f;
    else
        h = 60.0f * (r - g) / ch + 240.0f;
    if (h < 0) h += 360.0f;
    int bin = (int)(h / (360.0f / BINS)) % BINS;

    // Weight by chroma squared so a few vivid pixels outvote a lot of mud.
    float c = ch / 255.0f, w = c * c;
    weight[bin] += w;
    sat[bin] += w * ch / mx;
    val[bin] += w * mx / 255.0f;
}

static ScenePalette finish(RGB bg, float hue) {
    ScenePalette p;
    p.bg = nearestRGB332(bg);
    p.bgRGB = fromRGB332(p.bg);
    if (luminance(p.bgRGB) < 0.40f) {
        p.title = RGB{255, 255, 255};
        p.artist = lerp(p.bgRGB, p.title, 184);  // ~72% white
    } else {
        p.title = hsv(hue, 0.5f, 0.12f);
        p.artist = lerp(p.bgRGB, p.title, 184);
    }
    p.accent = hsv(hue, 0.30f, 1.0f);
    p.hue = hue;
    return p;
}

ScenePalette defaultPalette() {
    ScenePalette p = finish(RGB{40, 48, 66}, 220.0f);
    p.accent = RGB{196, 212, 255};
    return p;
}

ScenePalette pickPalette(const ArtStats& s) {
    if (s.count == 0) return defaultPalette();

    float meanChroma = s.chromaSum / s.count;
    float meanLum = s.lumSum / s.count;
    float total = 0;
    for (int i = 0; i < ArtStats::BINS; i++) total += s.weight[i];

    // Greyscale covers have no hue to complement: use a contrasting neutral.
    if (meanChroma < 0.06f || total < s.count * 0.004f) {
        // Both greys are exact RGB332 colours, so they stay neutral.
        ScenePalette p = finish(meanLum > 0.32f ? RGB{73, 73, 85} : RGB{109, 109, 85}, 220.0f);
        p.accent = RGB{210, 214, 222};
        p.colourful = 0.15f;
        return p;
    }

    // Dominant hue: heaviest (smoothed) bin, refined by a circular mean.
    const int B = ArtStats::BINS;
    int best = 0;
    float bestW = -1;
    for (int i = 0; i < B; i++) {
        float w = s.weight[i] + 0.5f * (s.weight[(i + B - 1) % B] + s.weight[(i + 1) % B]);
        if (w > bestW) bestW = w, best = i;
    }
    float cx = 0, cy = 0, ws = 0, ss = 0;
    for (int d = -1; d <= 1; d++) {
        int j = (best + d + B) % B;
        float a = (j + 0.5f) * (2.0f * (float)M_PI / B);
        cx += s.weight[j] * cosf(a);
        cy += s.weight[j] * sinf(a);
        ws += s.weight[j];
        ss += s.sat[j];
    }
    float domHue = atan2f(cy, cx) * 180.0f / (float)M_PI;
    float domSat = ws > 0 ? ss / ws : 0.5f;

    float hue = domHue + 180.0f;
    float sat = domSat * 0.85f;
    sat = sat < 0.30f ? 0.30f : (sat > 0.62f ? 0.62f : sat);

    // Brightness that puts every hue at the same perceived lightness.
    const float target = 0.15f;
    float lo = 0.15f, hi = 1.0f;
    for (int i = 0; i < 16; i++) {
        float mid = 0.5f * (lo + hi);
        if (luminance(hsv(hue, sat, mid)) < target)
            lo = mid;
        else
            hi = mid;
    }
    return finish(hsv(hue, sat, 0.5f * (lo + hi)), hue);
}

GlowLook glowLook(const ScenePalette& p, int amount, int style) {
    const float own = p.hue + 180, other = p.hue, k = p.colourful;
    GlowLook g;
    g.stops[0] = RGB{0, 0, 0};
    if (style == 1) {  // VIZ_SPECTRUM: filtered phosphor, ice white, whatever the cover
        g.stops[1] = RGB{40, 87, 106};
        g.stops[2] = RGB{120, 188, 210};
        g.stops[3] = RGB{197, 235, 245};
        g.stops[4] = RGB{238, 250, 252};
    } else {
        g.stops[1] = hsv(own, 0.85f * k, 0.30f);    // deep, in the cover's colour
        g.stops[2] = hsv(other, 0.55f * k, 0.50f);  // its complement through the middle
        g.stops[3] = hsv(own, 0.70f * k, 0.85f);
        g.stops[4] = hsv(own + 20, 0.20f * k, 1.0f);  // nearly white at the hottest
    }
    g.bg = p.bgRGB;
    g.title = p.title;
    g.artist = p.artist;
    g.amount = amount;
    return g;
}

RGB glowColour(const GlowLook& g, int index) {
    static const float AT[5] = {0.0f, 0.30f, 0.60f, 0.85f, 1.0f};
    const RGB base = lerp(g.bg, g.stops[0], g.amount);
    if (index >= GLOW_TITLE) {  // text over the darkest glow, fading in from the plain card's
        const bool title = index < GLOW_ARTIST;
        const int k = index - (title ? GLOW_TITLE : GLOW_ARTIST);
        const RGB ink = lerp(title ? g.title : g.artist, title ? RGB{250, 246, 240} : RGB{214, 208, 200}, g.amount);
        return lerp(base, ink, (k + 1) * 256 / GLOW_TEXT_LEVELS);
    }
    const float u = (float)index / (GLOW_LEVELS - 1);
    int s = 0;
    while (s < 3 && u > AT[s + 1]) s++;
    const RGB c = lerp(g.stops[s], g.stops[s + 1], (int)((u - AT[s]) / (AT[s + 1] - AT[s]) * 256));
    return lerp(g.bg, c, g.amount);
}
