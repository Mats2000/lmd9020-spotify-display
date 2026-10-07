#include "net.h"

#include <WiFi.h>
#include <lwip/sockets.h>
#include <stdarg.h>
#include <time.h>

#include "config.h"
#include "cover.h"
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
    if (changed && buf[0]) Serial.printf("Status: %s\n", buf);
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

    Serial.printf("Now: %s / %s (heap %u, largest %u, net stack left %u)\n", t.title.c_str(),
                  t.artist.c_str(), heap_caps_get_free_size(MALLOC_CAP_8BIT), heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
                  uxTaskGetStackHighWaterMark(nullptr));
    ScenePalette pal;
    int colours = 0;
    bool ok = fetchCover(t.imageUrl, t.imageWidth, t.thumbUrl, t.thumbWidth, art, pal, colours);
    if (!ok) {  // one more try: a dropped download is usually a one-off
        vTaskDelay(pdMS_TO_TICKS(500));
        ok = fetchCover(t.imageUrl, t.imageWidth, t.thumbUrl, t.thumbWidth, art, pal, colours);
    }
    if (!ok) {
        pal = defaultPalette();
        fillPlaceholderArt(art, pal);
        colours = 0;
    }

    Serial.printf("  net stack left after the cover: %u\n", uxTaskGetStackHighWaterMark(nullptr));
    xSemaphoreTake(lock, portMAX_DELAY);
    shared.art = art;
    shared.artColours = colours;
    shared.pal = pal;
    xSemaphoreGive(lock);
    return ok;
}

bool placeholder(const char* s) { return strncmp(s, "PASTE", 5) == 0 || !s[0]; }

void netTask(void*) {
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

    String currentKey;
    int coverRetries = 0;  // left for the current track's cover, if it didn't come
    uint32_t coverRetryAt = 0;
    uint32_t lastPlayingMs = 0;
    bool hasPlayed = false;
    bool reallyPlaying = false;
    int failures = 0;

    for (;;) {
        if (WiFi.status() != WL_CONNECTED) {
            setStatus("Connecting to Wi-Fi “%s”…", WIFI_SSID);
            setPlaying(false, true);
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }
        if (!reported) {
            reported = true;
            Serial.printf("Wi-Fi up, %s. Heap %u, largest %u\n", WiFi.localIP().toString().c_str(),
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
                    Serial.println("Trying the cover again");
                    coverRetries = loadTrack(track) ? 0 : coverRetries - 1;
                    coverRetryAt = millis() + 30000;
                }
                reallyPlaying = track.playing;
                if (track.playing) {
                    lastPlayingMs = millis();
                    hasPlayed = true;
                    wait = POLL_PLAYING_MS;
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
                Serial.printf("  heap %u, largest %u, lowest ever %u\n", heap_caps_get_free_size(MALLOC_CAP_8BIT),
                              heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
                              heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT));
                break;
        }

        // Stay on the cover through short pauses and track skips.
        setPlaying(hasPlayed && millis() - lastPlayingMs < PAUSE_GRACE_MS, !reallyPlaying);
        vTaskDelay(pdMS_TO_TICKS(wait));
    }
}

}  // namespace

#if VISUALIZER
// Packets from tools/visualizer: "LMDV", version 1, flags (bit 0: music playing), level,
// beat, 16 bands (low to high, each 0..255), then the visualizer style (a VizStyle). Every 2 s the display announces itself
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
        uint8_t buf[32];
        int len = recv(s, buf, sizeof(buf), 0);
        if (len >= 24 && memcmp(buf, "LMDV", 4) == 0 && buf[4] == 1) {
            xSemaphoreTake(lock, portMAX_DELAY);
            shared.audio.active = buf[5] & 1;
            shared.audio.level = buf[6];
            shared.audio.beat = buf[7];
            memcpy(shared.audio.band, buf + 8, sizeof(shared.audio.band));
            shared.audio.style = len >= 25 ? buf[24] : 0;
            shared.audio.atMs = millis() | 1;
            xSemaphoreGive(lock);
        }
    }
}
#endif

void netStart(const Scene* s) {
    scene = s;
    lock = xSemaphoreCreateMutex();
    art = (uint32_t*)heap_caps_malloc(ART_ALLOC_WORDS * 4, MALLOC_CAP_EXEC);  // IRAM, see art.h
    if (!art) {
        Serial.println("Cover buffer didn't fit in IRAM; using ordinary RAM");
        art = (uint32_t*)malloc(ART_ALLOC_WORDS * 4);
    }
    if (!art) {
        Serial.println("Out of memory for the cover art buffer");
        abort();
    }
    xTaskCreatePinnedToCore(netTask, "net", 9728, nullptr, 1, nullptr, 0);  // peaks near 7 KB
#if VISUALIZER
    xTaskCreatePinnedToCore(vizTask, "viz", 2304, nullptr, 1, nullptr, 0);
#endif
}

void netSnapshot(NowPlaying& out) {
    xSemaphoreTake(lock, portMAX_DELAY);
    out = shared;
    xSemaphoreGive(lock);
}
