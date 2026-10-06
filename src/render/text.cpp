#include "text.h"

// Nearest-colour blend, cached.
static uint8_t blendPrecise(uint8_t dst, RGB c, int alpha) {
    struct Entry {
        uint64_t key;
        uint8_t out;
    };
    static Entry cache[256];
    int level = alpha >> 3;  // 33 steps is plenty
    uint64_t key = ((uint64_t)dst << 40) | ((uint64_t)level << 32) | ((uint64_t)c.r << 16) | (c.g << 8) | c.b;
    key |= 1ull << 63;  // so an empty slot never matches
    Entry& e = cache[(dst * 31 + level * 7 + c.r + c.g * 3 + c.b * 5) & 255];
    if (e.key != key) {
        e.key = key;
        e.out = nearestRGB332(lerp(fromRGB332(dst), c, level >= 32 ? 256 : level << 3));
    }
    return e.out;
}

static uint32_t nextCodepoint(const char*& s) {
    const uint8_t* p = (const uint8_t*)s;
    uint32_t c = *p++;
    int extra = 0;
    if (c >= 0xF0)
        c &= 0x07, extra = 3;
    else if (c >= 0xE0)
        c &= 0x0F, extra = 2;
    else if (c >= 0xC0)
        c &= 0x1F, extra = 1;
    else if (c >= 0x80)
        c = 0xFFFD;  // stray continuation byte
    for (; extra > 0; extra--) {
        if ((*p & 0xC0) != 0x80) {
            c = 0xFFFD;
            break;
        }
        c = (c << 6) | (*p++ & 0x3F);
    }
    s = (const char*)p;
    return c;
}

static const Glyph* findGlyph(const Font& f, uint32_t code) {
    int lo = 0, hi = f.count - 1;
    while (lo <= hi) {
        int mid = (lo + hi) >> 1;
        uint16_t c = f.glyphs[mid].code;
        if (c == code) return &f.glyphs[mid];
        if (c < code)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return nullptr;
}

static const Glyph* glyphFor(const Font& f, uint32_t code) {
    if (code == 0xA0) code = ' ';  // no-break space
    const Glyph* g = code <= 0xFFFF ? findGlyph(f, code) : nullptr;
    // Sora is Latin-only; anything else (CJK, Cyrillic, emoji) shows as '?'.
    return g ? g : findGlyph(f, '?');
}

int textWidth16(const Font& font, const char* s) {
    int w = 0;
    while (*s) {
        const Glyph* g = glyphFor(font, nextCodepoint(s));
        if (g) w += g->advance;
    }
    return w;
}

int drawText(Canvas& c, const Font& font, const char* s, int x16, int baseline,
             const TextStyle& st) {
    const uint8_t solid = toRGB332(st.color);
    const bool plain = st.opacity >= 256 && st.fadeLeft == 0 && st.fadeRight == 0;
    while (*s) {
        const Glyph* g = glyphFor(font, nextCodepoint(s));
        if (!g) continue;
        int gx = ((x16 + 8) >> 4) + g->left;
        int gy = baseline + g->top;
        int stride = (g->width + 1) >> 1;
        const uint8_t* bits = font.bitmap + g->offset;

        if (gx + g->width > st.clipLeft && gx < st.clipRight) {
            for (int row = 0; row < g->height; row++) {
                int y = gy + row;
                if (y < 0 || y >= Canvas::H) continue;
                for (int col = 0; col < g->width; col++) {
                    int x = gx + col;
                    if (x < st.clipLeft || x >= st.clipRight) continue;
                    uint8_t b = bits[row * stride + (col >> 1)];
                    int a = (col & 1) ? (b & 15) : (b >> 4);
                    if (!a) continue;
                    if (st.ramp > 0) {
                        int alpha = a * st.opacity / 15;
                        if (st.fadeLeft > 0 && x - st.clipLeft < st.fadeLeft)
                            alpha = alpha * (x - st.clipLeft + 1) / (st.fadeLeft + 1);
                        if (st.fadeRight > 0 && st.clipRight - 1 - x < st.fadeRight)
                            alpha = alpha * (st.clipRight - x) / (st.fadeRight + 1);
                        int level = (alpha * st.rampLevels + 128) >> 8;
                        if (level > st.rampLevels) level = st.rampLevels;
                        if (level > 0) c.rows[y][x] = (uint8_t)(st.ramp + level - 1);
                        continue;
                    }
                    if (a == 15 && plain) {  // the solid interior: no blending needed
                        c.rows[y][x] = solid;
                        continue;
                    }
                    int alpha = a * st.opacity / 15;
                    if (st.fadeLeft > 0 && x - st.clipLeft < st.fadeLeft)
                        alpha = alpha * (x - st.clipLeft + 1) / (st.fadeLeft + 1);
                    if (st.fadeRight > 0 && st.clipRight - 1 - x < st.fadeRight)
                        alpha = alpha * (st.clipRight - x) / (st.fadeRight + 1);
                    if (st.precise && alpha > 0)
                        c.rows[y][x] = blendPrecise(c.rows[y][x], st.color, alpha);
                    else
                        c.blend(x, y, st.color, alpha);
                }
            }
        }
        x16 += g->advance;
    }
    return x16;
}

void copyUtf8(char* dst, const char* src, int size) {
    int n = 0;
    while (src[n] && n < size - 1) n++;
    // Back off so we don't cut a multi-byte character in half.
    if (src[n]) {
        while (n > 0 && ((uint8_t)src[n] & 0xC0) == 0x80) n--;
    }
    memcpy(dst, src, n);
    dst[n] = 0;
}
