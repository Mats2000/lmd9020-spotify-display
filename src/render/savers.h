#pragma once

#include "canvas.h"
#include "color.h"
#include "fakeaudio.h"

// The screensavers; the scene rotates through the enabled ones and draws the clock.
enum SaverId {
    SAVER_ID_WAVES,   // dot field rolling like a sea
    SAVER_ID_SPHERE,  // a globe of dots, turning and breathing
    SAVER_ID_VINYL,   // a record of dots seen at an angle, ripples running along the grooves
    SAVER_ID_RIDGES,  // dotted ridge lines rolling toward you, after Unknown Pleasures
    SAVER_ID_VFD,     // 80s head unit: one-colour fluorescent display, dot-matrix spectrum
    SAVER_ID_WIRED,   // utility poles and humming wires at dusk, after Serial Experiments Lain
    SAVER_ID_HAZE,    // Gen X soft club: pastel haze, drifting particles, orbit rings, fine print
    SAVER_ID_NETWORK,   // the Wired as a graph: drifting nodes, links, packets
    SAVER_ID_TUNNEL,    // a tube of dot rings twisting into cyberspace
    SAVER_ID_SCOPE,     // oscilloscope X-Y figure in green phosphor
    SAVER_ID_TERMINAL,  // hex dumps scrolling, a blinking prompt
    SAVER_ID_STATIC,    // TV snow, a rolling band, a dropout with two red words
    SAVER_ID_NAVI,      // a made-up 90s OS desktop, windows opening around the clock
    SAVER_ID_REDSKY,    // rooftops and crows on the wires against a blood-red dusk
    SAVER_ID_PSYCHE,    // the clock as a chip label, circuit traces pulsing outward
    SAVER_ID_CROSSING,  // a night street crossing, the walk signal cycling
    SAVER_ID_RAIN,         // a bus window at night: blurred city lights, drops running down
    SAVER_ID_CONTOURS,     // a survey map of a landscape that slowly reshapes itself
    SAVER_ID_CURRENTS,     // a wind map: fine streaks riding a turning flow
    SAVER_ID_COUNT,
};

struct SaverFrame {
    uint32_t ms;
    int ox, oy;       // burn-in drift, applied to everything that would otherwise stand still
    RGB accent;       // from the last cover played
    const FakeAudio* audio;
    int clockX, clockY;          // clock centre, drift included
    int clockHalfW, clockHalfH;  // its size, for keeping the background calm around it
};

struct SaverStyle {
    int clockY;       // clock centre before drift
    RGB clock;        // clock colour
    bool ghostDigits; // draw unlit "8"s behind the clock, like a fluorescent display
};

SaverStyle saverStyle(int id);
void drawSaver(int id, Canvas& c, const SaverFrame& f);
// Full-screen TV snow, for clearing image retention on the LCD.
void drawSnow(Canvas& c, uint32_t ms);
// Runs after the clock is drawn, for effects that cover it too (glitches).
void drawSaverOverlay(int id, Canvas& c, const SaverFrame& f);
