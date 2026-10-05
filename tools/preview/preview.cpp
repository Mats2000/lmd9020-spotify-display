// Renders one frame with the firmware's scene code. Driven by preview.py.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "render/art.h"
#include "render/scene.h"

static uint8_t framebuffer[Canvas::H][Canvas::W];
static uint32_t art[ART_WORDS];

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "see preview.py\n");
        return 2;
    }
    uint8_t* rows[Canvas::H];
    for (int y = 0; y < Canvas::H; y++) rows[y] = framebuffer[y];
    Canvas canvas{rows};

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

        // Feed it through in 16x16 blocks, the way the JPEG decoder does.
        ArtBuilder builder;
        builder.begin(art, w, h);
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

    // Render every frame from power-on so fades and scrolling match the monitor.
    for (uint32_t t = 0; t + 16 < ms; t += 16) scene.render(canvas, np, clock, t);
    scene.render(canvas, np, clock, ms);

    FILE* out = fopen(argv[1], "wb");
    fprintf(out, "P6\n%d %d\n255\n", Canvas::W, Canvas::H);
    for (int y = 0; y < Canvas::H; y++)
        for (int x = 0; x < Canvas::W; x++) {
            RGB p = fromRGB332(framebuffer[y][x]);
            fputc(p.r, out), fputc(p.g, out), fputc(p.b, out);
        }
    fclose(out);
    return 0;
}
