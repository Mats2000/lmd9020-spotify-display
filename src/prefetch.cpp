#include "prefetch.h"

#include <esp_partition.h>
#include <esp_spi_flash.h>

#include "cover.h"
#include "https.h"

namespace prefetch {
namespace {

constexpr uint32_t MAGIC = 0x4E585443;  // "NXTC"
constexpr uint32_t SECTOR = 4096;
constexpr uint32_t DATA = SECTOR;  // the header has the first sector to itself, written last

struct Header {
    uint32_t magic;
    uint32_t thumbLen, imageLen;
    int32_t thumbWidth, imageWidth;
    char key[120];
};

const esp_partition_t* part;
uint32_t sectors;
uint32_t erasedTo;  // sectors [0, erasedTo) are known erased (while storing: from the write point on)
Header held;        // what the slot holds (magic 0: nothing)
bool spoiled;       // a failed download wrote part of itself: nothing more fits there

bool eraseUpTo(uint32_t end) {
    while (erasedTo < end) {
        if (esp_partition_erase_range(part, erasedTo * SECTOR, SECTOR) != ESP_OK) return false;
        erasedTo++;
    }
    return true;
}

// Downloads url into the slot at offset; returns the bytes written, 0 on failure or if it doesn't
// fit in `room`.
uint32_t download(const String& url, uint32_t offset, uint32_t room) {
    String host, path;
    if (!splitUrl(url, host, path) || https.request("GET", host.c_str(), path.c_str()) != 200) return 0;
    int len = https.contentLength();
    if (len > (int)room) {
        https.close();
        return 0;
    }
    uint8_t buf[512];
    uint32_t got = 0;
    for (;;) {
        int n = https.read(buf, sizeof(buf));
        if (n == 0) break;
        if (n < 0 || got + n > room) {
            https.close();
            spoiled = got > 0;
            return 0;
        }
        if (!eraseUpTo((offset + got + n + SECTOR - 1) / SECTOR) ||
            esp_partition_write(part, offset + got, buf, n) != ESP_OK) {
            https.close();
            spoiled = true;
            return 0;
        }
        got += n;
    }
    if (len >= 0 && got != (uint32_t)len) {
        spoiled = got > 0;
        return 0;
    }
    return got;
}

}  // namespace

bool begin() {
    part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, nullptr);
    if (!part) return false;
    sectors = part->size / SECTOR;
    esp_partition_read(part, 0, &held, sizeof(held));
    held.key[sizeof(held.key) - 1] = 0;
    if (held.magic != MAGIC || held.thumbLen + held.imageLen > part->size - DATA) held.magic = 0;
    erasedTo = 0;
    return true;
}

bool holds(const String& key) { return part && held.magic == MAGIC && key == held.key; }

bool load(const Track& t, uint32_t* art, ScenePalette& pal, int& colours) {
    if (!holds(t.key)) return false;
    const void* map;
    spi_flash_mmap_handle_t handle;
    uint32_t len = DATA + ((held.thumbLen + 3) & ~3u) + held.imageLen;
    if (esp_partition_mmap(part, 0, len, SPI_FLASH_MMAP_DATA, &map, &handle) != ESP_OK) return false;
    const uint8_t* base = (const uint8_t*)map + DATA;
    bool ok = decodeCover(base, held.thumbLen, held.thumbWidth, base + ((held.thumbLen + 3) & ~3u), held.imageLen,
                          held.imageWidth, art, pal, colours);
    spi_flash_munmap(handle);
    return ok;
}

void forget() { held.magic = 0; }

bool eraseStep() {
    if (!part || held.magic == MAGIC || erasedTo >= sectors) return false;
    return eraseUpTo(erasedTo + 1);
}

bool clean() { return part && held.magic == 0 && erasedTo >= sectors; }

bool store(const Track& t) {
    if (!part || t.imageUrl.isEmpty()) return false;
    held.magic = 0;
    if (!eraseUpTo(1)) return false;  // the header first: nothing stale can be read meanwhile
    struct Written {
        ~Written() { erasedTo = 0; }  // the next store starts from a fresh erase
    } written;
    uint32_t started = millis();
    const uint32_t room = part->size - DATA;

    Header h = {};
    h.thumbWidth = t.thumbWidth;
    spoiled = false;
    h.thumbLen = t.thumbUrl.length() ? download(t.thumbUrl, DATA, room / 8) : 0;
    if (spoiled) return false;
    uint32_t imageAt = DATA + ((h.thumbLen + 3) & ~3u);
    h.imageWidth = t.imageWidth;
    h.imageLen = download(t.imageUrl, imageAt, part->size - imageAt);
    if (!h.imageLen && !spoiled && t.smallUrl.length()) {  // too big for the slot: the next size down
        h.imageWidth = t.smallWidth;
        h.imageLen = download(t.smallUrl, imageAt, part->size - imageAt);
    }
    https.close();
    if (!h.imageLen) {
        Serial.println("Next cover: download failed");
        return false;
    }
    h.magic = MAGIC;
    strlcpy(h.key, t.key.c_str(), sizeof(h.key));
    if (esp_partition_write(part, 0, &h, sizeof(h)) != ESP_OK) return false;
    held = h;
    Serial.printf("Next cover ready: %s (%u px, %u KB, %u ms)\n", t.title.c_str(), (unsigned)h.imageWidth,
                  (unsigned)(h.imageLen / 1024), (unsigned)(millis() - started));
    return true;
}

}  // namespace prefetch
