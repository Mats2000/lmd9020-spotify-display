#pragma once

#include <atomic>

#include "art.h"
#include "canvas.h"
#include "color.h"
#include "fakeaudio.h"
#include "font.h"

// The latest from Spotify, copied to the render loop every frame.
struct NowPlaying {
    uint32_t version = 0;          // bumped whenever the track (and its art) changes
    bool playing = false;          // false shows the screensaver
    bool paused = false;           // playing is held through short pauses; this says if it is one
    char title[128] = "";
    char artist[128] = "";
    const uint32_t* art = nullptr;  // ART_WORDS of packed RGB332 (see art.h), owned by the network side
    int artColours = 0;             // > 0: art indexes its own palette instead (see buildArtPalette)
    ScenePalette pal = defaultPalette();
    char status[72] = "";          // shown on the screensaver while something needs attention
};

struct ClockTime {
    bool valid;  // false until NTP has answered
    int hour, minute;
    int millis;  // within the current second
    int wday, mon, mday;  // 0 = Sunday, 0 = January, 1..31
};

// Where the cover is on screen, when it has a palette of its own: the video output draws that
// rectangle with artPalette(art) at `level` brightness.
struct CoverRegion {
    bool on = false;
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    const uint32_t* art = nullptr;
    int colours = 0;
    int level = 256;
    uint32_t version = 0;  // changes with the cover
};

// Draws a frame: the now-playing card or a screensaver, with fades between them.
class Scene {
public:
    void render(Canvas& c, const NowPlaying& np, const ClockTime& clock, uint32_t nowMs);

    // The cover buffer on screen; the network task must not decode into it.
    const uint32_t* artInUse() const { return artInUse_.load(); }

    // false fades to black (before sleep); true fades back in.
    void setAwake(bool awake) { awake_ = awake; }
    bool dark() const { return level_ == 0; }

    const CoverRegion& coverRegion() const { return region_; }

    // Full-screen TV snow whatever is playing (panel refresh: the BOOT button, before sleep).
    void holdSnow(bool on) { snowHeld_ = on; }

    // Show one screensaver (a SaverId) instead of rotating; for tools/preview.
    void pinSaver(int id) { pinnedSaver_ = id; }

private:
    struct Marquee {
        int width16;
        uint32_t start;
    };

    int saverFor(uint32_t ms) const;
    void adopt(const NowPlaying& np, uint32_t key, uint32_t ms);
    void drawNowPlaying(Canvas& c, uint32_t ms);
    void drawLoading(Canvas& c, const NowPlaying& np, uint32_t ms);
    struct Offset {
        int x, y;
    };
    void drawCaption(Canvas& c, const char* title, const char* artist, const Marquee& m, Offset d,
                     RGB titleColor, RGB artistColor, bool precise, uint32_t ms);
    static int lineWidth16(const char* title, const char* artist);
    void drawIdle(Canvas& c, const NowPlaying& np, const ClockTime& clock, uint32_t ms);

    uint32_t shownKey_ = 0;  // track version, or IDLE_KEY | saver id; 0 = nothing yet
    char title_[128] = "";
    char artist_[128] = "";
    const uint32_t* art_ = nullptr;
    int artColours_ = 0;
    CoverRegion region_;
    ScenePalette pal_ = defaultPalette();  // kept after a track ends to tint the waves
    Marquee lineMarquee_{};

    bool paused_ = false;
    bool heardMusic_ = false;
    bool snowHeld_ = false;
    uint32_t snowUntil_ = 0;  // snow after the music stops, until then
    uint32_t loadingSince_ = 0;
    int level_ = 0;  // overall brightness, 0..256, for the fades
    int fadeInMs_ = 1600;
    bool awake_ = true;
    uint32_t lastMs_ = 0;
    bool started_ = false;

    // Screensaver rotation: each idle spell picks up where the last left off.
    bool idle_ = false;
    uint32_t idleSince_ = 0;
    uint32_t rotation_ = 0;
    int pinnedSaver_ = -1;
    int saver_ = 0;
    FakeAudio audio_;

    std::atomic<const uint32_t*> artInUse_{nullptr};
};
