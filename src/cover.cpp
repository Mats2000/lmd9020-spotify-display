#include "cover.h"

// TJpgDec (via esp32-camera in the Arduino core): reads the HTTPS stream directly.
#include "esp_jpg_decode.h"

#include "https.h"
#include "render/art.h"

namespace {

struct Source {
    ArtBuilder* art;
    uint32_t* dst;
    int colours;
};

uint32_t lowestHeap;  // during the decode, for the log

// Returns `len` bytes unless the body ends; buf == nullptr means skip them.
size_t jpegRead(void*, size_t, uint8_t* buf, size_t len) {
    uint32_t free8 = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    if (free8 < lowestHeap) lowestHeap = free8;
    uint8_t scratch[64];
    size_t got = 0;
    while (got < len) {
        size_t want = len - got;
        int n = buf ? https.read(buf + got, want) : https.read(scratch, want < sizeof(scratch) ? want : sizeof(scratch));
        if (n <= 0) break;
        got += n;
    }
    return got;
}

// Called with data == nullptr first (output size) and last (done).
bool jpegWrite(void* arg, uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint8_t* data) {
    Source* src = (Source*)arg;
    if (!data) {
        if (x == 0 && y == 0) src->art->begin(src->dst, w, h, src->colours);
        return true;
    }
    src->art->block(data, x, y, x + w - 1, y + h - 1);
    return true;
}

// The thumbnail's pixels, as 0x00RRGGBB words at the start of the cover buffer.
struct Sample {
    uint32_t* dst;
    int w, h, cap;
};

bool sampleWrite(void* arg, uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint8_t* data) {
    Sample* s = (Sample*)arg;
    if (!data) {
        if (x == 0 && y == 0) s->w = w, s->h = h;
        return s->w * s->h <= s->cap;
    }
    volatile uint32_t* px = s->dst;  // IRAM: whole words only
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) {
            const uint8_t* p = data + (j * w + i) * 3;
            if (x + i < s->w && y + j < s->h) px[(y + j) * s->w + x + i] = (uint32_t)p[0] << 16 | p[1] << 8 | p[2];
        }
    return true;
}

// Picks the cover's own colours from a small copy of it. Returns how many (0: none).
int fetchCoverPalette(const String& url, int width, uint32_t* dst) {
    String host, path;
    if (!splitUrl(url, host, path)) return 0;
    int scale = 0;
    if (width <= 0) width = 64;
    while (scale < 3 && (width >> scale) > 80) scale++;
    if (https.request("GET", host.c_str(), path.c_str()) != 200) return 0;
    Sample s{dst, 0, 0, ART_WORDS - 2 * ART_PALETTE_WORDS};
    int len = https.contentLength();
    esp_err_t err = esp_jpg_decode(len > 0 ? len : 0, (jpg_scale_t)scale, jpegRead, sampleWrite, &s);
    if (err != ESP_OK || s.w * s.h < 16 || s.w * s.h > s.cap) return 0;
    return buildArtPalette(dst, s.w * s.h);
}

}  // namespace

bool fetchCover(const String& url, int imageWidth, const String& thumbUrl, int thumbWidth, uint32_t* dst,
                ScenePalette& pal, int& colours) {
    String host, path;
    if (!splitUrl(url, host, path)) return false;

    // Largest power-of-two reduction that keeps the cover at least ART_H tall.
    int scale = 0;
    if (imageWidth <= 0) imageWidth = 640;
    while (scale < 3 && (imageWidth >> (scale + 1)) >= ART_H) scale++;

    // Allocate decode buffers before the download, while the heap is unfragmented.
    ArtBuilder* builder = new ArtBuilder;
    if (!builder) return false;
    builder->reserve(imageWidth >> scale, 8);  // a band is at most 8 rows at these scales

    uint32_t started = millis();
    lowestHeap = UINT32_MAX;
    colours = thumbUrl.length() ? fetchCoverPalette(thumbUrl, thumbWidth, dst) : 0;
    uint32_t paletteMs = millis() - started;
    started = millis();
    int code = https.request("GET", host.c_str(), path.c_str());
    uint32_t connected = millis();
    if (code != 200) {
        Serial.printf("Cover download: HTTP %d %s\n", code, code < 0 ? https.lastError() : "");
        delete builder;
        return false;
    }
    Source src{builder, dst, colours};
    int len = https.contentLength();
    esp_err_t err = esp_jpg_decode(len > 0 ? len : 0, (jpg_scale_t)scale, jpegRead, jpegWrite, &src);
    builder->finish();
    if (err == ESP_OK) pal = pickPalette(builder->stats());
    delete builder;
    https.close();  // the next request goes to Spotify's API host anyway

    Serial.printf("Cover %d px (%d KB) decoded at 1/%d: %s. %d colours (%u ms), connect %u ms, "
                  "download+decode %u ms, lowest heap %u\n",
                  imageWidth, len / 1024, 1 << scale, esp_err_to_name(err), colours, (unsigned)paletteMs,
                  (unsigned)(connected - started), (unsigned)(millis() - connected), (unsigned)lowestHeap);
    return err == ESP_OK;
}
