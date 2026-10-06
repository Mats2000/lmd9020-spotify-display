// The app's live preview: the firmware's scene code, fed what the display would be fed.
#include <stdio.h>
#include <string.h>

#include "DemoBridge.h"
#include "render/art.h"
#include "render/scene.h"

namespace {

uint8_t buffers[2][Canvas::H][Canvas::W];  // drawn alternately, like the firmware's two
uint8_t* rows[2][Canvas::H];
uint32_t art[ART_ALLOC_WORDS];
Scene scene;
NowPlaying np;
int frame = 0;
bool ready = false;

void setup() {
    if (ready) return;
    for (int b = 0; b < 2; b++)
        for (int y = 0; y < Canvas::H; y++) rows[b][y] = buffers[b][y];
    ready = true;
}

}  // namespace

extern "C" void lmd_demo_set_track(const uint8_t* cover, const uint8_t* thumb, const char* title, const char* artist) {
    setup();
    int colours = 0;
    if (thumb) {
        for (int i = 0; i < 64 * 64; i++)
            art[i] = (uint32_t)thumb[i * 3] << 16 | thumb[i * 3 + 1] << 8 | thumb[i * 3 + 2];
        colours = buildArtPalette(art, 64 * 64);
    }
    if (cover) {
        // In 16x16 blocks, the way the JPEG decoder hands them over.
        ArtBuilder builder;
        builder.begin(art, 320, 320, colours);
        static uint8_t block[16 * 16 * 3];
        for (int by = 0; by < 320; by += 16)
            for (int bx = 0; bx < 320; bx += 16) {
                for (int y = 0; y < 16; y++) memcpy(block + y * 48, cover + ((by + y) * 320 + bx) * 3, 48);
                builder.block(block, bx, by, bx + 15, by + 15);
            }
        builder.finish();
        np.pal = pickPalette(builder.stats());
        np.artColours = colours;
    } else {
        np.pal = defaultPalette();
        fillPlaceholderArt(art, np.pal);
        np.artColours = 0;
    }
    np.art = art;
    np.version++;
    snprintf(np.title, sizeof(np.title), "%s", title ? title : "");
    snprintf(np.artist, sizeof(np.artist), "%s", artist ? artist : "");
}

extern "C" void lmd_demo_set_playing(int playing, int paused) {
    setup();
    np.playing = playing != 0;
    np.paused = paused != 0;
}

extern "C" void lmd_demo_render(const uint8_t* bands, uint8_t beat, int active, int style, uint32_t ms, int hour,
                                int minute, int wday, int mon, int mday, uint8_t* out) {
    setup();
    if (bands) memcpy(np.audio.band, bands, sizeof(np.audio.band));
    np.audio.beat = beat;
    np.audio.style = (uint8_t)style;
    np.audio.active = active != 0;
    if (active) np.audio.atMs = ms | 1;
    ClockTime clock{true, hour, minute, (int)(ms % 1000), wday, mon, mday};

    Canvas canvas{rows[frame & 1]};
    canvas.shown = rows[(frame + 1) & 1];
    scene.render(canvas, np, clock, ms);
    frame++;

    // What the video output would make of it: RGB332, the cover's palette, the glow's.
    const CoverRegion& region = scene.coverRegion();
    const GlowRegion& glow = scene.glowRegion();
    uint8_t (*fb)[Canvas::W] = buffers[(frame + 1) & 1];
    for (int y = 0; y < Canvas::H; y++)
        for (int x = 0; x < Canvas::W; x++) {
            RGB p;
            if (region.on && x >= region.x0 && x < region.x1 && y >= region.y0 && y < region.y1) {
                uint32_t c = artPalette(region.art)[fb[y][x]];
                p = RGB{(uint8_t)(((c >> 16) & 255) * region.level >> 8), (uint8_t)(((c >> 8) & 255) * region.level >> 8),
                        (uint8_t)((c & 255) * region.level >> 8)};
            } else if (glow.on) {
                RGB g = glowColour(glow.look, fb[y][x]);
                p = RGB{(uint8_t)(g.r * glow.level >> 8), (uint8_t)(g.g * glow.level >> 8), (uint8_t)(g.b * glow.level >> 8)};
            } else {
                p = fromRGB332(fb[y][x]);
            }
            uint8_t* o = out + (y * Canvas::W + x) * 3;
            o[0] = p.r, o[1] = p.g, o[2] = p.b;
        }
}
