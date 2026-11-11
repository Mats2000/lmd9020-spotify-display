#include "log.h"
#include "net.h"

#include <MD5Builder.h>
#include <WiFi.h>
#include <esp_ota_ops.h>
#include <lwip/sockets.h>
#include <mbedtls/sha256.h>
#include <stdarg.h>
#include <time.h>

#include "config.h"
#include "cover.h"
#include "https.h"
#include "prefetch.h"
#include "render/text.h"
#include "spotify.h"

#if __has_include("secrets.h")
#include "secrets.h"
#else
#error "Copy include/secrets.example.h to include/secrets.h and fill it in (see README)."
#endif

namespace {

SemaphoreHandle_t lock;
NowPlaying shared;
const Scene* scene;
SpotifyClient spotify;

// One cover buffer (RAM is tight): the old cover fades out before the new one decodes into it.
uint32_t* art;

void setStatus(const char* fmt, ...) {
    char buf[sizeof(shared.status)];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    xSemaphoreTake(lock, portMAX_DELAY);
    bool changed = strcmp(buf, shared.status) != 0;
    if (changed) copyUtf8(shared.status, buf, sizeof(shared.status));
    xSemaphoreGive(lock);
    if (changed && buf[0]) say("Status: %s\n", buf);
}

void setPlaying(bool playing, bool paused) {
    xSemaphoreTake(lock, portMAX_DELAY);
    shared.playing = playing;
    shared.paused = paused;
    xSemaphoreGive(lock);
}

// Shows the track and fetches its cover; false if the cover couldn't be had (a stand-in shows).
bool loadTrack(const Track& t) {
    xSemaphoreTake(lock, portMAX_DELAY);
    copyUtf8(shared.title, t.title.c_str(), sizeof(shared.title));
    copyUtf8(shared.artist, t.artist.c_str(), sizeof(shared.artist));
    shared.art = nullptr;
    shared.version++;
    xSemaphoreGive(lock);

    // The scene needs a frame or so to notice, then FADE_OUT_MS to let go.
    int waited = 0;
    while (scene->artInUse() == art && waited < 2000) {
        vTaskDelay(pdMS_TO_TICKS(10));
        waited += 10;
    }
    vTaskDelay(pdMS_TO_TICKS(100));  // the video output lets go of the cover's palette at the next frame

    say("Now: %s / %s (heap %u, largest %u, net stack left %u)\n", t.title.c_str(),
                  t.artist.c_str(), heap_caps_get_free_size(MALLOC_CAP_8BIT), heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
                  uxTaskGetStackHighWaterMark(nullptr));
    ScenePalette pal;
    int colours = 0;
    bool mine = prefetch::heldFor(t);
    bool ok = mine && prefetch::load(t, art, pal, colours);
    // Used: the slot is for the song after this one now. Someone else's (after a restart, say) stays
    // until the queue says otherwise.
    if (mine) prefetch::forget("this song began");
    if (!ok) ok = fetchCover(t.imageUrl, t.imageWidth, t.thumbUrl, t.thumbWidth, art, pal, colours);
    if (!ok) {  // one more try, the next size down if there is one: some sizes won't decode (progressive)
        vTaskDelay(pdMS_TO_TICKS(500));
        bool small = t.smallUrl.length() > 0;
        ok = fetchCover(small ? t.smallUrl : t.imageUrl, small ? t.smallWidth : t.imageWidth, t.thumbUrl, t.thumbWidth, art, pal, colours);
    }
    if (!ok) {
        pal = defaultPalette();
        fillPlaceholderArt(art, pal);
        colours = 0;
    }

    say("  net stack left after the cover: %u\n", uxTaskGetStackHighWaterMark(nullptr));
    xSemaphoreTake(lock, portMAX_DELAY);
    shared.art = art;
    shared.artColours = colours;
    shared.pal = pal;
    xSemaphoreGive(lock);
    return ok;
}

bool placeholder(const char* s) { return strncmp(s, "PASTE", 5) == 0 || !s[0]; }

volatile uint32_t beatMs, spotifyMs;  // see netSinceBeat(), netSinceSpotify()

// An update over Wi-Fi asked for by tools/ota.sh (a packet "LMDU size md5 token port" on the
// visualizer's port): fetched from the asker on `port`, if token is SHA-256 of
// "OTA_PASSWORD:md5". Nothing is allocated for it until then.
struct UpdateRequest {
    volatile bool pending = false;
    uint32_t ip = 0, size = 0;
    uint16_t port = 0;
    char md5[33] = "", token[65] = "";
} update;

void runUpdate() {
    UpdateRequest r;
    r.ip = update.ip, r.size = update.size, r.port = update.port;
    memcpy(r.md5, update.md5, sizeof(r.md5));
    memcpy(r.token, update.token, sizeof(r.token));
    update.pending = false;
#ifdef OTA_PASSWORD
    if (strncmp(OTA_PASSWORD, "PASTE", 5) == 0) return;
    char proof[160];
    snprintf(proof, sizeof(proof), "%s:%s", OTA_PASSWORD, r.md5);
    uint8_t sum[32];
    mbedtls_sha256_ret((const uint8_t*)proof, strlen(proof), sum, 0);
    char hex[65];
    for (int i = 0; i < 32; i++) snprintf(hex + 2 * i, 3, "%02x", sum[i]);
    if (strcmp(hex, r.token) != 0) {
        say("%s\n", "Update over Wi-Fi refused: wrong password");
        return;
    }
    const esp_partition_t* part = esp_ota_get_next_update_partition(nullptr);
    if (!part || r.size == 0 || r.size > part->size) {
        say("%s\n", "Update over Wi-Fi refused: no room for it");
        return;
    }
    say("Update over Wi-Fi: %u bytes\n", (unsigned)r.size);
    https.close();
    WiFiClient c;
    esp_ota_handle_t h;
    if (!c.connect(IPAddress(r.ip), r.port, 5000) || esp_ota_begin(part, r.size, &h) != ESP_OK) {
        say("%s\n", "Update over Wi-Fi failed to start");
        return;
    }
    MD5Builder md5;
    md5.begin();
    uint8_t buf[1024];
    uint32_t got = 0, heard = millis();
    while (got < r.size && millis() - heard < 10000) {
        const int n = c.read(buf, r.size - got < sizeof(buf) ? r.size - got : sizeof(buf));
        if (n <= 0) {
            if (!c.connected() && !c.available()) break;
            delay(2);
            continue;
        }
        if (esp_ota_write(h, buf, n) != ESP_OK) break;
        md5.add(buf, n);
        got += n;
        heard = beatMs = millis();
    }
    md5.calculate();
    if (got != r.size || md5.toString() != r.md5) {
        esp_ota_abort(h);
        say("Update over Wi-Fi failed (%u of %u bytes)\n", (unsigned)got, (unsigned)r.size);
        c.print("FAIL\n");
        return;
    }
    if (esp_ota_end(h) != ESP_OK || esp_ota_set_boot_partition(part) != ESP_OK) {
        say("%s\n", "Update over Wi-Fi failed to finish");
        c.print("FAIL\n");
        return;
    }
    say("%s\n", "Update over Wi-Fi: done, restarting");
    c.print("OK\n");
    c.flush();
    delay(300);
    esp_restart();
#endif
}

void netTask(void*) {
    beatMs = spotifyMs = millis();
    WiFi.mode(WIFI_STA);
#if VISUALIZER
    WiFi.setSleep(false);  // in power save, packets would arrive in bursts at each beacon
#endif
    WiFi.setHostname("lmd9020-spotify");
    WiFi.setAutoReconnect(true);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    bool reported = false;
    configTzTime(TIMEZONE, "pool.ntp.org", "time.google.com", "time.cloudflare.com");

    if (placeholder(SPOTIFY_CLIENT_ID) || placeholder(SPOTIFY_REFRESH_TOKEN)) {
        // Still useful as a clock until Spotify is set up.
        for (;;) {
            if (WiFi.status() != WL_CONNECTED)
                setStatus("Connecting to Wi-Fi “%s”…", WIFI_SSID);
            else
                setStatus("Add Spotify keys: run tools/spotify_auth.py");
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
    spotify.begin();
    if (!prefetch::begin()) say("%s\n", "No flash partition for the next cover");

    String currentKey;
    int coverRetries = 0;  // left for the current track's cover, if it didn't come
    uint32_t coverRetryAt = 0;
    uint32_t lastPlayingMs = 0;
    bool hasPlayed = false;
    bool reallyPlaying = false;
    int failures = 0;
    String askedFor;  // the song the queue was last asked about
    bool askedLate = false;
    Track wanted;  // the next song, once the slot is erased for its cover
    bool haveWanted = false;

    for (;;) {
        beatMs = millis();
        if (update.pending) runUpdate();
        if (WiFi.status() != WL_CONNECTED) {
            setStatus("Connecting to Wi-Fi “%s”…", WIFI_SSID);
            setPlaying(false, true);
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }
        if (!reported) {
            reported = true;
            say("Wi-Fi up, %s. Heap %u, largest %u\n", WiFi.localIP().toString().c_str(),
                          heap_caps_get_free_size(MALLOC_CAP_8BIT), heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
        }
        // The clock is needed before HTTPS anyway (certificate dates).
        if (time(nullptr) < 1700000000) {
            setStatus("Setting the clock…");
            vTaskDelay(pdMS_TO_TICKS(250));
            continue;
        }

        Track track;
        uint32_t retryMs = 0;
        uint32_t wait = POLL_IDLE_MS;
        SpotifyResult r = spotify.current(track, retryMs);
        if (r != SpotifyResult::NetError) spotifyMs = millis();  // it answered, whatever it said
        switch (r) {
            case SpotifyResult::Item:
                failures = 0;
                setStatus("");
                if (track.key != currentKey) {
                    coverRetries = loadTrack(track) ? 0 : 3;
                    coverRetryAt = millis() + 10000;
                    currentKey = track.key;
                } else if (coverRetries > 0 && (int32_t)(millis() - coverRetryAt) >= 0) {
                    // The cover didn't come: try again a little later, rather than for good.
                    say("%s\n", "Trying the cover again");
                    coverRetries = loadTrack(track) ? 0 : coverRetries - 1;
                    coverRetryAt = millis() + 30000;
                }
                reallyPlaying = track.playing;
                if (track.playing) {
                    lastPlayingMs = millis();
                    hasPlayed = true;
                    wait = POLL_PLAYING_MS;
                    // Ask again just as the song should end, and look at the queue when the song
                    // starts and once more near its end (it may have changed).
                    uint32_t left = track.durationMs > track.progressMs ? track.durationMs - track.progressMs : 0;
                    if (track.durationMs && left + 400 < wait) wait = left + 400 < 800 ? 800 : left + 400;
                    bool late = track.durationMs && left < PREFETCH_LATE_MS;
                    if (askedFor != currentKey || (late && !askedLate)) {
                        if (askedFor != currentKey) askedLate = false;
                        askedFor = currentKey;
                        askedLate = askedLate || late;
                        Track n;
                        SpotifyResult q = spotify.next(n);
                        say("Queue: %s next %s\n", q == SpotifyResult::Item ? "ok" : "nothing", q == SpotifyResult::Item ? n.key.c_str() : "");
                        if (q == SpotifyResult::Item && n.key != currentKey && !prefetch::heldFor(n)) {
                            prefetch::forget("the queue changed");
                            wanted = n;
                            haveWanted = true;
                        }
                    }
                }
                if (haveWanted && wanted.key == currentKey) haveWanted = false;
                if (haveWanted && prefetch::clean()) {
                    haveWanted = false;
                    prefetch::store(wanted);
                }
                break;
            case SpotifyResult::Nothing:
                reallyPlaying = false;
                failures = 0;
                setStatus("");
                break;
            case SpotifyResult::AuthError:
                setStatus("Spotify login rejected: rerun tools/spotify_auth.py");
                wait = 60000;
                break;
            case SpotifyResult::RateLimited:
                wait = retryMs;
                break;
            case SpotifyResult::NetError:
                // Back off: 2, 4, 8... up to 60 s, so a bad patch can't snowball.
                if (++failures >= 4) setStatus("Can’t reach Spotify…");
                wait = 2000u << (failures < 5 ? failures - 1 : 5);
                if (wait > 60000) wait = 60000;
                say("  heap %u, largest %u, lowest ever %u\n", heap_caps_get_free_size(MALLOC_CAP_8BIT),
                              heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
                              heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT));
                break;
        }

        // Stay on the cover through short pauses and track skips.
        setPlaying(hasPlayed && millis() - lastPlayingMs < PAUSE_GRACE_MS, !reallyPlaying);
        // Meanwhile, the next cover's slot is erased a sector at a time (each pauses the render core).
        for (const uint32_t until = millis() + wait; (int32_t)(until - millis()) > 0;) {
            prefetch::eraseStep();
            int32_t left = (int32_t)(until - millis());
            vTaskDelay(pdMS_TO_TICKS(left < 150 ? (left > 1 ? left : 1) : 150));
        }
    }
}

}  // namespace

#if VISUALIZER
// Packets from tools/showcontrol: "LMDV", version 1, flags (bit 0: music playing), level,
// beat, 16 bands (low to high, each 0..255), then the visualizer style (a VizStyle) and colour scheme (a ColourScheme), 64 samples of the waveform and SCHEME_ONE's colour. Every 2 s the display announces itself
// ("LMD9020 1") by broadcast on the next port, so the Mac can find it.
void vizTask(void*) {
    int s = -1;
    uint32_t lastAnnounce = 0;
    for (;;) {
        if (WiFi.status() != WL_CONNECTED) {
            if (s >= 0) close(s), s = -1;
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }
        if (s < 0) {
            s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            if (s < 0) {
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }
            int yes = 1;
            setsockopt(s, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes));
            timeval tv = {0, 200000};
            setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            sockaddr_in a = {};
            a.sin_family = AF_INET;
            a.sin_port = htons(VISUALIZER_PORT);
            a.sin_addr.s_addr = htonl(INADDR_ANY);
            bind(s, (sockaddr*)&a, sizeof(a));
        }
        if (millis() - lastAnnounce > 2000) {
            static const char HELLO[] = "LMD9020 1";
            sockaddr_in b = {};
            b.sin_family = AF_INET;
            b.sin_port = htons(VISUALIZER_PORT + 1);
            b.sin_addr.s_addr = htonl(INADDR_BROADCAST);
            sendto(s, HELLO, sizeof(HELLO) - 1, 0, (sockaddr*)&b, sizeof(b));
            lastAnnounce = millis();
        }
        uint8_t buf[160];
        sockaddr_in from = {};
        socklen_t fromLen = sizeof(from);
        int len = recvfrom(s, buf, sizeof(buf) - 1, 0, (sockaddr*)&from, &fromLen);
        if (len > 5 && memcmp(buf, "LMDU ", 5) == 0 && !update.pending) {  // tools/ota.sh asking
            buf[len] = 0;
            unsigned size = 0, port = 0;
            if (sscanf((const char*)buf + 5, "%u %32s %64s %u", &size, update.md5, update.token, &port) == 4) {
                update.ip = from.sin_addr.s_addr, update.size = size, update.port = (uint16_t)port;
                update.pending = true;
            }
            continue;
        }
        if (len == 5 && memcmp(buf, "LMDF", 4) == 0) {  // the Mac's Flash, resent every 50 ms while held
            xSemaphoreTake(lock, portMAX_DELAY);
            shared.audio.flashUntil = buf[4] ? (millis() + 400) | 1 : 0;
            xSemaphoreGive(lock);
            continue;
        }
        if (len >= 24 && memcmp(buf, "LMDV", 4) == 0 && buf[4] == 1) {
            logTo(from.sin_addr.s_addr);
            xSemaphoreTake(lock, portMAX_DELAY);
            shared.audio.active = buf[5] & 1;
            shared.audio.level = buf[6];
            shared.audio.beat = buf[7];
            memcpy(shared.audio.band, buf + 8, sizeof(shared.audio.band));
            shared.audio.style = len >= 25 ? buf[24] : 0;
            shared.audio.scheme = len >= 26 ? buf[25] : 0;
            if (len >= 26 + 64) memcpy(shared.audio.wave, buf + 26, 64);
            else memset(shared.audio.wave, 0, 64);
            if (len >= 26 + 64 + 3) memcpy(shared.audio.one, buf + 26 + 64, 3);
            shared.audio.atMs = millis() | 1;
            xSemaphoreGive(lock);
        }
    }
}
#endif

void netStart(const Scene* s) {
    scene = s;
    lock = xSemaphoreCreateMutex();
    logStart();
    art = (uint32_t*)heap_caps_malloc(ART_ALLOC_WORDS * 4, MALLOC_CAP_EXEC);  // IRAM, see art.h
    if (!art) {
        say("%s\n", "Cover buffer didn't fit in IRAM; using ordinary RAM");
        art = (uint32_t*)malloc(ART_ALLOC_WORDS * 4);
    }
    if (!art) {
        say("%s\n", "Out of memory for the cover art buffer");
        abort();
    }
    xTaskCreatePinnedToCore(netTask, "net", 10240, nullptr, 1, nullptr, 0);  // peaks near 8.7 KB
#if VISUALIZER
    xTaskCreatePinnedToCore(vizTask, "viz", 2304, nullptr, 1, nullptr, 0);
#endif
}

uint32_t netSinceBeat() { return millis() - beatMs; }
uint32_t netSinceSpotify() { return millis() - spotifyMs; }

void netSnapshot(NowPlaying& out) {
    xSemaphoreTake(lock, portMAX_DELAY);
    out = shared;
    xSemaphoreGive(lock);
}
