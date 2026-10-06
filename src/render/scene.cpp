#include "scene.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "draw.h"
#include "fonts.h"
#include "savers.h"
#include "text.h"
#include "visualizer.h"

namespace {

constexpr int SLEEP_FADE_MS = 3000;
constexpr int GLOW_FADE_MS = 1200;  // the card to the visualizer and back
constexpr uint32_t IDLE_KEY = 0x80000000u;
constexpr uint32_t LOADING_KEY = 0x40000000u;  // playing, cover still downloading: black
constexpr uint32_t SNOW_KEY = 0x20000000u;     // full-screen TV snow, to clear the LCD

// ---- Now playing ----
constexpr int ART_Y = 14;                      // cover top
constexpr int ART_X = (Canvas::W - ART_W) / 2;
constexpr int LINE_GAP = 7;                    // cover bottom to the text line's cap top
constexpr int TEXT_LEFT = 18;
constexpr int TEXT_RIGHT = Canvas::W - 18;
const char* const SEPARATOR = "  \xC2\xB7  ";   // a middle dot between title and artist

// Titles too wide for the screen hold, then scroll round as a loop.
constexpr uint32_t MARQUEE_HOLD_MS = 2500;
constexpr int MARQUEE_GAP_PX = 40;
constexpr int MARQUEE_PX_PER_S = 24;
constexpr int MARQUEE_FADE_PX = 12;

constexpr int STATUS_BASELINE = 222;

// Particle scenes alternate with the cyber ones.
const int SAVERS[] = {
#if SAVER_WAVES
    SAVER_ID_WAVES,
#endif
#if SAVER_RAIN
    SAVER_ID_RAIN,
#endif
#if SAVER_STATIC
    SAVER_ID_STATIC,
#endif
#if SAVER_SPHERE
    SAVER_ID_SPHERE,
#endif
#if SAVER_NETWORK
    SAVER_ID_NETWORK,
#endif
#if SAVER_VINYL
    SAVER_ID_VINYL,
#endif
#if SAVER_NAVI
    SAVER_ID_NAVI,
#endif
#if SAVER_RIDGES
    SAVER_ID_RIDGES,
#endif
#if SAVER_CONTOURS
    SAVER_ID_CONTOURS,
#endif
#if SAVER_WIRED
    SAVER_ID_WIRED,
#endif
#if SAVER_TUNNEL
    SAVER_ID_TUNNEL,
#endif
#if SAVER_CURRENTS
    SAVER_ID_CURRENTS,
#endif
#if SAVER_REDSKY
    SAVER_ID_REDSKY,
#endif
#if SAVER_SCOPE
    SAVER_ID_SCOPE,
#endif
#if SAVER_PSYCHE
    SAVER_ID_PSYCHE,
#endif
#if SAVER_TERMINAL
    SAVER_ID_TERMINAL,
#endif
#if SAVER_VFD
    SAVER_ID_VFD,
#endif
#if SAVER_HAZE
    SAVER_ID_HAZE,
#endif
#if SAVER_CROSSING
    SAVER_ID_CROSSING,
#endif
};
constexpr uint32_t SAVER_COUNT = sizeof(SAVERS) / sizeof(SAVERS[0]);
constexpr uint32_t SAVER_MS = SAVER_MINUTES * 60000u;

// Burn-in protection: static elements drift slowly on a Lissajous path.
struct Drift {
    int x, y;
};

Drift drift(uint32_t ms, int rx, int ry, uint32_t periodX, uint32_t periodY) {
#if BURNIN_SHIFT
    const float TAU = 6.2831853f;
    float ax = (ms % periodX) * (TAU / periodX), ay = (ms % periodY) * (TAU / periodY);
    return Drift{(int)lroundf(rx * sinf(ax)), (int)lroundf(ry * sinf(ay))};
#else
    (void)ms, (void)rx, (void)ry, (void)periodX, (void)periodY;
    return Drift{0, 0};
#endif
}

// Up to 10 px sideways (at most a pixel every half minute), barely vertically: the song line
// sits near the bottom.
Drift cardDrift(uint32_t ms) { return drift(ms, 10, 3, 38 * 60000u, 59 * 60000u); }
// The clock: the brightest, most static thing on screen, so it roams furthest.
Drift clockDrift(uint32_t ms) { return drift(ms, 10, 6, 37 * 60000u, 53 * 60000u); }

// How long a change fades out and back in, by what is changing.
struct Fade {
    int outMs, inMs;
};
Fade fadeFor(uint32_t from, uint32_t to) {
    if (to == SNOW_KEY) return {500, 300};    // like changing channel
    if (from == SNOW_KEY) return {600, 1600};
    bool fromIdle = from & IDLE_KEY, toIdle = to & IDLE_KEY;
    if (fromIdle && toIdle) return {1500, 2500};               // one screensaver to the next
    if (fromIdle != toIdle || from == 0) return {1000, 1600};  // music to clock and back
    if (from == LOADING_KEY) return {250, 700};                // the new cover arriving
    return {300, 500};                                         // track to track
}

// Dithered fade (2x2 pattern): RGB332 steps visibly otherwise.
uint8_t fadeLut[4][256];

}  // namespace

int Scene::saverFor(uint32_t ms) const {
    if (pinnedSaver_ >= 0) return pinnedSaver_;
    return SAVERS[(rotation_ + (ms - idleSince_) / SAVER_MS) % SAVER_COUNT];
}

void Scene::render(Canvas& c, const NowPlaying& np, const ClockTime& clock, uint32_t ms) {
    int dt = started_ ? (int)(ms - lastMs_) : 0;
    if (dt > 100) dt = 100;  // don't jump after a stall
    started_ = true;
    lastMs_ = ms;
    dtMs_ = dt;

    uint32_t want;
    if (np.playing) {
        if (idle_) rotation_ += (ms - idleSince_) / SAVER_MS + 1;  // next time, start on the next one
        idle_ = false;
        heardMusic_ = true;
        want = np.art ? np.version : LOADING_KEY;
    } else {
        if (!idle_) {
            idleSince_ = ms;
            if (heardMusic_) snowUntil_ = ms + REFRESH_AFTER_MUSIC_SECONDS * 1000u;
        }
        idle_ = true;
        want = (int32_t)(snowUntil_ - ms) > 0 ? SNOW_KEY : IDLE_KEY | (uint32_t)saverFor(ms);
    }
    if (snowHeld_) want = SNOW_KEY;

    bool changing = want != shownKey_;
    if (changing || !awake_) {
        level_ -= dt * 256 / (awake_ ? fadeFor(shownKey_, want).outMs : SLEEP_FADE_MS);
        if (level_ <= 0) {
            level_ = 0;
            if (changing) {
                fadeInMs_ = fadeFor(shownKey_, want).inMs;
                adopt(np, want, ms);
            }
        }
    } else {
        level_ += dt * 256 / fadeInMs_;
        if (level_ > 256) level_ = 256;
    }

    paused_ = np.paused;
    region_.on = false;
    glow_.on = false;
    if (shownKey_ & IDLE_KEY || shownKey_ == SNOW_KEY || shownKey_ == LOADING_KEY || !shownKey_) viz_.fed = false;
    if (shownKey_ & IDLE_KEY) {
        drawIdle(c, np, clock, ms);
    } else if (shownKey_ == SNOW_KEY) {
        drawSnow(c, ms);
    } else if (shownKey_ == LOADING_KEY) {
        audio_.update(ms);
        drawLoading(c, np, ms);
    } else if (shownKey_) {
        audio_.update(ms);
        drawNowPlaying(c, np, ms);
    }
    else
        c.fill(0);

    if (level_ < 256 && !glow_.on) {  // the visualizer fades through its palette instead
        for (int p = 0; p < 4; p++)
            for (int i = 0; i < 256; i++) {
                RGB q = fromRGB332((uint8_t)i);
                fadeLut[p][i] = dither332(RGB{(uint8_t)(q.r * level_ >> 8), (uint8_t)(q.g * level_ >> 8),
                                              (uint8_t)(q.b * level_ >> 8)},
                                          p & 1, p >> 1);
            }
        for (int y = 0; y < Canvas::H; y++) {
            uint8_t* row = c.rows[y];
            const uint8_t *even = fadeLut[(y & 1) * 2], *odd = fadeLut[(y & 1) * 2 + 1];
            if (region_.on && y >= region_.y0 && y < region_.y1) {  // the cover fades through its palette
                for (int x = 0; x < Canvas::W; x++)
                    if (x < region_.x0 || x >= region_.x1) row[x] = (x & 1 ? odd : even)[row[x]];
                continue;
            }
            for (int x = 0; x < Canvas::W; x += 2) {
                row[x] = even[row[x]];
                row[x + 1] = odd[row[x + 1]];
            }
        }
    }
    region_.level = level_;
    glow_.level = level_;
}

void Scene::adopt(const NowPlaying& np, uint32_t key, uint32_t ms) {
    shownKey_ = key;
    if (key == LOADING_KEY) loadingSince_ = ms;
    if (key & IDLE_KEY) {
        saver_ = (int)(key & 0xFF);
    } else if (key != LOADING_KEY && key != SNOW_KEY) {
        copyUtf8(title_, np.title, sizeof(title_));
        copyUtf8(artist_, np.artist, sizeof(artist_));
        art_ = np.art;
        artColours_ = np.art ? np.artColours : 0;
        pal_ = np.pal;
        lineMarquee_ = {lineWidth16(title_, artist_), ms};
    }
    artInUse_.store((key & IDLE_KEY) || key == LOADING_KEY || key == SNOW_KEY ? nullptr : art_);
}

// ---------------------------------------------------------------------------
// Now playing

void Scene::drawNowPlaying(Canvas& c, const NowPlaying& np, uint32_t ms) {
    Drift d = cardDrift(ms);
    // The visualizer while the Mac sends the music; a solid background otherwise.
#if VISUALIZER
    const bool heard = np.audio.active && np.audio.atMs && (int32_t)(ms - np.audio.atMs) < 800;  // signed: atMs can be a hair ahead
#else
    const bool heard = false;
#endif
    const int step = dtMs_ * 256 / GLOW_FADE_MS + 1;
    glowAmount_ = heard ? (glowAmount_ + step > 256 ? 256 : glowAmount_ + step) : (glowAmount_ > step ? glowAmount_ - step : 0);
    const bool glowing = glowAmount_ > 0;
    if (glowing) {
        static const NowPlaying::Audio QUIET;
        const NowPlaying::Audio& a = heard ? np.audio : QUIET;
        auto mean = [&](int from, int to) {
            int s = 0;
            for (int i = from; i < to; i++) s += a.band[i];
            return s / (255.0f * (to - from));
        };
        const VizInput in{a.band, mean(0, 4), mean(5, 11), mean(11, 16), a.beat / 255.0f};
        const int x0 = ART_X + d.x, y0 = ART_Y + d.y;
        // The style stays put while fading out, though the music has stopped saying which.
        if (heard) vizStyle_ = np.audio.style < VIZ_COUNT ? np.audio.style : 0;
        drawVisualizer(vizStyle_, c, x0, y0, x0 + ART_W, y0 + ART_H, y0 + ART_H + 4,
                       (dtMs_ > 100 ? 100 : dtMs_) * 0.001f, ms, in, viz_);
        glow_.on = true;
        glow_.look = glowLook(pal_, glowAmount_, vizStyle_);
        glow_.style = vizStyle_;
        glow_.version = shownKey_;
    } else {
        c.fill(pal_.bg);
        viz_.fed = false;
    }
    for (int y = 0; y < ART_H; y++) copyArtRow(art_, y, c.rows[ART_Y + d.y + y] + ART_X + d.x);
    if (artColours_ > 0) {
        region_.on = true;
        region_.x0 = ART_X + d.x, region_.y0 = ART_Y + d.y;
        region_.x1 = region_.x0 + ART_W, region_.y1 = region_.y0 + ART_H;
        region_.art = art_;
        region_.colours = artColours_;
        region_.version = shownKey_;
    }
    drawCaption(c, title_, artist_, lineMarquee_, Offset{d.x, d.y}, pal_.title, pal_.artist, true, ms, glowing);
}


// While the next cover downloads: its title and artist on dark, in the same place.
void Scene::drawLoading(Canvas& c, const NowPlaying& np, uint32_t ms) {
    Drift d = cardDrift(ms);
    c.fill(toRGB332(lerp(pal_.bgRGB, RGB{0, 0, 0}, 190)));
    Marquee m = {lineWidth16(np.title, np.artist), loadingSince_};
    drawCaption(c, np.title, np.artist, m, Offset{d.x, d.y}, RGB{255, 255, 255}, RGB{200, 200, 210}, false, ms);
}

int Scene::lineWidth16(const char* title, const char* artist) {
    return textWidth16(sora_title, title) + textWidth16(sora_title_regular, SEPARATOR) +
           textWidth16(sora_title_regular, artist);
}

void Scene::drawCaption(Canvas& c, const char* title, const char* artist, const Marquee& m, Offset d,
                        RGB titleColor, RGB artistColor, bool precise, uint32_t ms, bool glow) {
    // Title (SemiBold) and artist (Regular) side by side under the cover.
    const int baseline = ART_Y + d.y + ART_H + LINE_GAP + sora_title.cap;
    TextStyle st;
    st.precise = precise;
    st.clipLeft = TEXT_LEFT + d.x;
    st.clipRight = TEXT_RIGHT + d.x;
    st.rampLevels = GLOW_TEXT_LEVELS;
    auto line = [&](int x16) {
        st.color = titleColor;
        st.ramp = glow ? GLOW_TITLE : 0;  // over the visualizer: through its palette
        x16 = drawText(c, sora_title, title, x16, baseline, st);
        st.color = artistColor;  // same size, lighter weight
        st.ramp = glow ? GLOW_ARTIST : 0;
        x16 = drawText(c, sora_title_regular, SEPARATOR, x16, baseline, st);
        drawText(c, sora_title_regular, artist, x16, baseline, st);
    };
    const int box16 = (TEXT_RIGHT - TEXT_LEFT) * 16;
    if (m.width16 <= box16) {
        line(st.clipLeft * 16 + (box16 - m.width16) / 2);
        return;
    }
    // Too wide: hold, then scroll round as a loop.
    const int dist16 = m.width16 + MARQUEE_GAP_PX * 16;
    const uint32_t scrollMs = (uint32_t)dist16 * 1000 / (MARQUEE_PX_PER_S * 16);
    uint32_t t = (ms - m.start) % (MARQUEE_HOLD_MS + scrollMs);
    int off16 = t < MARQUEE_HOLD_MS ? 0 : (int)((t - MARQUEE_HOLD_MS) * MARQUEE_PX_PER_S * 16 / 1000);
    // The left edge only softens once text is actually sliding under it.
    int edge = off16 < dist16 - off16 ? off16 : dist16 - off16;
    st.fadeLeft = edge / 16 < MARQUEE_FADE_PX ? edge / 16 : MARQUEE_FADE_PX;
    st.fadeRight = MARQUEE_FADE_PX;
    int x16 = st.clipLeft * 16 - off16;
    line(x16);
    line(x16 + dist16);
}

// ---------------------------------------------------------------------------
// Screensaver + clock

void Scene::drawIdle(Canvas& c, const NowPlaying& np, const ClockTime& clock, uint32_t ms) {
    audio_.update(ms);
    // Some screensavers paint every pixel themselves.
    if (saver_ != SAVER_ID_HAZE && saver_ != SAVER_ID_STATIC && saver_ != SAVER_ID_NAVI &&
        saver_ != SAVER_ID_REDSKY && saver_ != SAVER_ID_PSYCHE && saver_ != SAVER_ID_CROSSING)
        c.fill(0);

    char text[8];
    if (!clock.valid)
        strcpy(text, "--:--");
    else if (CLOCK_24H)
        snprintf(text, sizeof(text), "%02d:%02d", clock.hour, clock.minute);
    else
        snprintf(text, sizeof(text), "%d:%02d", (clock.hour + 11) % 12 + 1, clock.minute);
    const int width16 = textWidth16(sora_clock, text);

    const SaverStyle style = saverStyle(saver_);
    const Drift still = cardDrift(ms), roam = clockDrift(ms);
    SaverFrame f;
    f.ms = ms;
    f.ox = still.x;
    f.oy = still.y;
    f.accent = pal_.accent;
    f.audio = &audio_;
    f.clockX = Canvas::W / 2 + roam.x;
    f.clockY = style.clockY + roam.y;
    f.clockHalfW = width16 / 32;
    f.clockHalfH = sora_clock.cap / 2 + 10;  // the date sits under the clock
    drawSaver(saver_, c, f);

    // Drawn around the colon so the colon can dim on the half second.
    TextStyle st;
    st.color = style.clock;
    if (!clock.valid) st.opacity = 90;
    int x16 = f.clockX * 16 - width16 / 2;
    int baseline = f.clockY + (sora_clock.cap + 1) / 2;
    if (style.ghostDigits) {  // the unlit segments of a fluorescent display
        char ghost[8];
        strcpy(ghost, text);
        for (char* p = ghost; *p; p++)
            if (*p >= '0' && *p <= '9') *p = '8';
        TextStyle g = st;
        g.opacity = 34;
        drawText(c, sora_clock, ghost, x16, baseline, g);
    }
    const char* colon = strchr(text, ':');
    char head[8];
    int n = (int)(colon - text);
    memcpy(head, text, n);
    head[n] = 0;
    x16 = drawText(c, sora_clock, head, x16, baseline, st);
    TextStyle colonStyle = st;
    if (CLOCK_BLINK_COLON && clock.valid && clock.millis >= 500) colonStyle.opacity = 70;
    x16 = drawText(c, sora_clock, ":", x16, baseline, colonStyle);
    drawText(c, sora_clock, colon + 1, x16, baseline, st);

    if (clock.valid) {  // the date, small, under the clock
        static const char* DAYS[] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};
        static const char* MONTHS[] = {"JAN", "FEB", "MAR", "APR", "MAY", "JUN",
                                       "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"};
        char date[24];
        if (DATE_DAY_FIRST)
            snprintf(date, sizeof(date), "%s  %d %s", DAYS[clock.wday % 7], clock.mday, MONTHS[clock.mon % 12]);
        else
            snprintf(date, sizeof(date), "%s  %s %d", DAYS[clock.wday % 7], MONTHS[clock.mon % 12], clock.mday);
        TextStyle ds = st;
        ds.opacity = 190;
        drawText(c, sora_artist, date, f.clockX * 16 - textWidth16(sora_artist, date) / 2, baseline + 17, ds);
    }
    drawSaverOverlay(saver_, c, f);

    if (np.status[0]) {
        fillRect(c, 0, STATUS_BASELINE - 11, Canvas::W, 15, 0);
        TextStyle s;
        s.color = RGB{170, 170, 170};
        drawText(c, sora_artist, np.status,
                 (Canvas::W / 2 + still.x) * 16 - textWidth16(sora_artist, np.status) / 2,
                 STATUS_BASELINE + still.y, s);
    }
}
