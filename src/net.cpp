#include "net.h"

#include <WiFi.h>
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

void loadTrack(const Track& t) {
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
}

bool placeholder(const char* s) { return strncmp(s, "PASTE", 5) == 0 || !s[0]; }

void netTask(void*) {
    WiFi.mode(WIFI_STA);
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
                    loadTrack(track);
                    currentKey = track.key;
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
}

void netSnapshot(NowPlaying& out) {
    xSemaphoreTake(lock, portMAX_DELAY);
    out = shared;
    xSemaphoreGive(lock);
}
