#include "cover.h"

// TJpgDec (via esp32-camera in the Arduino core): reads the HTTPS stream directly.
#include "esp_jpg_decode.h"

#include "https.h"
#include "render/art.h"

namespace {

struct Source {
    ArtBuilder* art;
    uint32_t* dst;
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
        if (x == 0 && y == 0) src->art->begin(src->dst, w, h);
        return true;
    }
    src->art->block(data, x, y, x + w - 1, y + h - 1);
    return true;
}

}  // namespace

bool fetchCover(const String& url, int imageWidth, uint32_t* dst, ScenePalette& pal) {
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
    int code = https.request("GET", host.c_str(), path.c_str());
    uint32_t connected = millis();
    if (code != 200) {
        Serial.printf("Cover download: HTTP %d %s\n", code, code < 0 ? https.lastError() : "");
        delete builder;
        return false;
    }
    Source src{builder, dst};
    int len = https.contentLength();
    esp_err_t err = esp_jpg_decode(len > 0 ? len : 0, (jpg_scale_t)scale, jpegRead, jpegWrite, &src);
    builder->finish();
    if (err == ESP_OK) pal = pickPalette(builder->stats());
    delete builder;
    https.close();  // the next request goes to Spotify's API host anyway

    Serial.printf("Cover %d px (%d KB) decoded at 1/%d: %s. Connect %u ms, download+decode %u ms, lowest heap %u\n",
                  imageWidth, len / 1024, 1 << scale, esp_err_to_name(err), (unsigned)(connected - started),
                  (unsigned)(millis() - connected), (unsigned)lowestHeap);
    return err == ESP_OK;
}
