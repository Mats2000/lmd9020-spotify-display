#include "log.h"
#include "prefetch.h"

#include <esp_partition.h>

#include "cover.h"
#include "https.h"

namespace prefetch {
namespace {

constexpr uint32_t MAGIC = 0x3254584E;  // "NXT2" (with the song's name)
constexpr uint32_t SECTOR = 4096;
constexpr uint32_t DATA = SECTOR;  // the header has the first sector to itself, written last

struct Header {
    uint32_t magic;
    uint32_t thumbLen, imageLen;
    int32_t thumbWidth, imageWidth;
    char key[120];
    char name[120];  // "title\nartist": the queue can name a song by another id than it plays as
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
    static uint8_t buf[512];  // not on the network task's stack, which a download already fills
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
    held.name[sizeof(held.name) - 1] = 0;
    if (held.magic != MAGIC || held.thumbLen + held.imageLen > part->size - DATA) held.magic = 0;
    erasedTo = 0;
    return true;
}

bool holds(const String& key) { return part && held.magic == MAGIC && key == held.key; }

static bool isFor(const Track& t) {
    return holds(t.key) || (part && held.magic == MAGIC && held.name[0] && (t.title + "\n" + t.artist) == held.name);
}

bool heldFor(const Track& t) { return isFor(t); }

bool load(const Track& t, uint32_t* art, ScenePalette& pal, int& colours) {
    if (!isFor(t)) return false;
    return decodeCover(part, DATA, held.thumbLen, held.thumbWidth, DATA + ((held.thumbLen + 3) & ~3u), held.imageLen,
                       held.imageWidth, art, pal, colours);
}

void forget(const char* why) {
    if (held.magic == MAGIC) say("Next cover dropped (%s): it was %s\n", why, held.key);
    held.magic = 0;
}

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
        say("%s\n", "Next cover: download failed");
        return false;
    }
    h.magic = MAGIC;
    strlcpy(h.key, t.key.c_str(), sizeof(h.key));
    strlcpy(h.name, (t.title + "\n" + t.artist).c_str(), sizeof(h.name));
    if (esp_partition_write(part, 0, &h, sizeof(h)) != ESP_OK) return false;
    held = h;
    say("Next cover ready: %s (%u px, %u KB, %u ms, net stack left %u)\n", t.title.c_str(), (unsigned)h.imageWidth,
        (unsigned)(h.imageLen / 1024), (unsigned)(millis() - started), (unsigned)uxTaskGetStackHighWaterMark(nullptr));
    return true;
}

}  // namespace prefetch
