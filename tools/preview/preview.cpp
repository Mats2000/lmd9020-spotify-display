// Renders one frame with the firmware's scene code. Driven by preview.py.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "render/art.h"
#include "render/scene.h"

#include <math.h>

static uint8_t buffers[2][Canvas::H][Canvas::W];  // drawn alternately, like the firmware's two
static uint32_t art[ART_ALLOC_WORDS];

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "see preview.py\n");
        return 2;
    }
    uint8_t* rows[2][Canvas::H];
    for (int b = 0; b < 2; b++)
        for (int y = 0; y < Canvas::H; y++) rows[b][y] = buffers[b][y];
    Canvas canvas{rows[0]};
    const bool withAudio = getenv("PREVIEW_AUDIO") != nullptr;  // a made-up song for the visualizer

    NowPlaying np;
    ClockTime clock{true, 9, 41, 200, 6, 9, 4};  // Sat 4 Oct
    uint32_t ms;
    Scene scene;

    if (!strcmp(argv[2], "idle") && argc >= 8) {
        clock.hour = atoi(argv[3]);
        clock.minute = atoi(argv[4]);
        clock.valid = clock.hour >= 0;
        scene.pinSaver(atoi(argv[5]));
        if (strcmp(argv[6], "-")) snprintf(np.status, sizeof(np.status), "%s", argv[6]);
        ms = (uint32_t)atol(argv[7]);
    } else if (!strcmp(argv[2], "playing") && argc >= 9) {
        int w = atoi(argv[4]), h = atoi(argv[5]);
        uint8_t* rgb = (uint8_t*)malloc((size_t)w * h * 3);
        FILE* f = fopen(argv[3], "rb");
        if (!f || fread(rgb, 3, (size_t)w * h, f) != (size_t)w * h) {
            fprintf(stderr, "can't read %s\n", argv[3]);
            return 1;
        }
        fclose(f);

        // The cover's own colours, from a 64 px thumbnail (<raw>.thumb) as Spotify serves one.
        int colours = 0;
        char thumbPath[512];
        snprintf(thumbPath, sizeof(thumbPath), "%s.thumb", argv[3]);
        if (FILE* t = fopen(thumbPath, "rb")) {
            static uint8_t thumb[64 * 64 * 3];
            if (fread(thumb, 3, 64 * 64, t) == 64 * 64) {
                for (int i = 0; i < 64 * 64; i++)
                    art[i] = (uint32_t)thumb[i * 3] << 16 | thumb[i * 3 + 1] << 8 | thumb[i * 3 + 2];
                colours = buildArtPalette(art, 64 * 64);
            }
            fclose(t);
        }

        // Feed it through in 16x16 blocks, the way the JPEG decoder does.
        ArtBuilder builder;
        builder.begin(art, w, h, colours);
        static uint8_t block[16 * 16 * 3];
        for (int by = 0; by < h; by += 16) {
            for (int bx = 0; bx < w; bx += 16) {
                int x1 = bx + 15 < w ? bx + 15 : w - 1, y1 = by + 15 < h ? by + 15 : h - 1;
                int bw = x1 - bx + 1, n = 0;
                for (int y = by; y <= y1; y++, n += bw)
                    memcpy(block + n * 3, rgb + ((size_t)y * w + bx) * 3, (size_t)bw * 3);
                builder.block(block, bx, by, x1, y1);
            }
        }
        builder.finish();
        np.pal = pickPalette(builder.stats());
        np.art = art;
        np.artColours = colours;
        np.version = 1;
        np.playing = true;
        snprintf(np.title, sizeof(np.title), "%s", argv[6]);
        snprintf(np.artist, sizeof(np.artist), "%s", argv[7]);
        ms = (uint32_t)atol(argv[8]);
        printf("background #%02X%02X%02X (RGB332 0x%02X), title #%02X%02X%02X, artist #%02X%02X%02X\n",
               np.pal.bgRGB.r, np.pal.bgRGB.g, np.pal.bgRGB.b, np.pal.bg, np.pal.title.r,
               np.pal.title.g, np.pal.title.b, np.pal.artist.r, np.pal.artist.g, np.pal.artist.b);
    } else {
        fprintf(stderr, "bad arguments; see preview.py\n");
        return 2;
    }

    // Render every frame from power-on (30 a second, as the firmware does) so fades, scrolling
    // and the visualizer's feedback match the monitor.
    auto song = [&](uint32_t t) {  // 120 BPM: kick, snare, hats, bass, pads
        float beat = t / 500.0f, b = beat - floorf(beat);
        float kick = expf(-b * 7), snare = fmodf(beat, 2.0f) >= 1 ? expf(-fmodf(beat - 1, 2.0f) * 6) : 0;
        float hat = expf(-(beat * 2 - floorf(beat * 2)) * 14), pad = 0.5f + 0.4f * sinf(t * 0.0009f);
        static const float BASS[4] = {0.8f, 0.6f, 0.9f, 0.7f};
        float bass = BASS[(int)beat % 4];
        for (int i = 0; i < 16; i++) {
            float x = i / 15.0f;
            float v = kick * fmaxf(0, 1 - x * 4) + bass * expf(-powf((x - 0.15f) / 0.08f, 2)) * 0.7f +
                      pad * expf(-powf((x - 0.45f) / 0.15f, 2)) * 0.55f + snare * expf(-powf((x - 0.55f) / 0.2f, 2)) * 0.8f +
                      hat * fmaxf(0, (x - 0.7f) / 0.3f) * 0.75f;
            np.audio.band[i] = (uint8_t)(fminf(1, v) * 255);
        }
        np.audio.beat = (uint8_t)(kick * 255);
        np.audio.active = true;
        np.audio.atMs = t | 1;
        if (const char* style = getenv("PREVIEW_STYLE")) np.audio.style = (uint8_t)atoi(style);
    };
    int frame = 0;
    auto renderAt = [&](uint32_t t) {
        canvas.rows = rows[frame & 1];
        canvas.shown = rows[(frame + 1) & 1];
        if (withAudio && np.playing) song(t);
        scene.render(canvas, np, clock, t);
        frame++;
    };
    // PREVIEW_DUMP=prefix also writes every frame from PREVIEW_DUMP_FROM ms on, for animations.
    const char* dump = getenv("PREVIEW_DUMP");
    const uint32_t dumpFrom = getenv("PREVIEW_DUMP_FROM") ? (uint32_t)atol(getenv("PREVIEW_DUMP_FROM")) : 0;
    int dumped = 0;
    for (uint32_t t = 0; t + 33 < ms; t += 33) {
        renderAt(t);
        if (dump && t >= dumpFrom) {
            uint8_t (*fb)[Canvas::W] = buffers[(frame + 1) & 1];
            char path[512];
            snprintf(path, sizeof(path), "%s%04d.ppm", dump, dumped++);
            if (FILE* o = fopen(path, "wb")) {
                fprintf(o, "P6\n%d %d\n255\n", Canvas::W, Canvas::H);
                const CoverRegion& r = scene.coverRegion();
                const GlowRegion& g = scene.glowRegion();
                for (int y = 0; y < Canvas::H; y++)
                    for (int x = 0; x < Canvas::W; x++) {
                        RGB p = fromRGB332(fb[y][x]);
                        if (g.on) p = glowColour(g.look, fb[y][x]);
                        if (r.on && x >= r.x0 && x < r.x1 && y >= r.y0 && y < r.y1) {
                            uint32_t c = artPalette(r.art)[fb[y][x]];
                            p = RGB{(uint8_t)(c >> 16), (uint8_t)(c >> 8), (uint8_t)c};
                        }
                        fputc(p.r, o), fputc(p.g, o), fputc(p.b, o);
                    }
                fclose(o);
            }
        }
    }
    renderAt(ms);
    uint8_t (*framebuffer)[Canvas::W] = buffers[(frame + 1) & 1];  // the one just drawn

    FILE* out = fopen(argv[1], "wb");
    fprintf(out, "P6\n%d %d\n255\n", Canvas::W, Canvas::H);
    const CoverRegion& region = scene.coverRegion();
    for (int y = 0; y < Canvas::H; y++)
        for (int x = 0; x < Canvas::W; x++) {
            RGB p = fromRGB332(framebuffer[y][x]);
            const GlowRegion& glow = scene.glowRegion();
            if (glow.on) {
                RGB g = glowColour(glow.look, framebuffer[y][x]);
                p = RGB{(uint8_t)(g.r * glow.level >> 8), (uint8_t)(g.g * glow.level >> 8), (uint8_t)(g.b * glow.level >> 8)};
            }
            if (region.on && x >= region.x0 && x < region.x1 && y >= region.y0 && y < region.y1) {
                uint32_t c = artPalette(region.art)[framebuffer[y][x]];
                p = RGB{(uint8_t)(((c >> 16) & 255) * region.level >> 8), (uint8_t)(((c >> 8) & 255) * region.level >> 8),
                        (uint8_t)((c & 255) * region.level >> 8)};
            }
            fputc(p.r, out), fputc(p.g, out), fputc(p.b, out);
        }
    fclose(out);
    return 0;
}
