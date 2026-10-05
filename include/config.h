#pragma once

// Settings. Wi-Fi and Spotify keys go in secrets.h (copy secrets.example.h).

// ---- Video -----------------------------------------------------------------

// 1 = NTSC (60 Hz), 0 = PAL (50 Hz). The LMD-9020 auto-detects either.
#define VIDEO_NTSC 1

// Video pin: 25 or 26 (both DACs carry the same picture). This board's 25 is damaged.
#define VIDEO_GPIO 26

// Width/height of one pixel on screen: 1.273 for 4:3 NTSC, 1.70 stretched to 16:9.
// Rerun tools/make_fonts.py after changing it.
#define DISPLAY_PIXEL_ASPECT 1.273f

// ---- Clock -----------------------------------------------------------------

// POSIX TZ string (America/New_York). E.g. "<-05>5" Ecuador, "PST8PDT,M3.2.0,M11.1.0" US Pacific.
#define TIMEZONE "EST5EDT,M3.2.0,M11.1.0"
#define CLOCK_24H 0          // 0 = 9:41, 1 = 21:41
#define CLOCK_BLINK_COLON 1  // dim the colon every other half second
#define DATE_DAY_FIRST 0     // 0 = SAT  OCT 4, 1 = SAT  4 OCT

// ---- Spotify ---------------------------------------------------------------

#define POLL_PLAYING_MS 3000  // how often to ask Spotify what's playing
#define POLL_IDLE_MS 5000     // ...while nothing is
// Pauses shorter than this keep the cover up.
#define PAUSE_GRACE_MS 8000

// ---- Screensaver -----------------------------------------------------------

// Minutes per screensaver; 0 turns one off (keep at least one on).
#define SAVER_MINUTES 20
#define SAVER_WAVES 1   // dots rolling like a sea
#define SAVER_SPHERE 1  // a globe of dots, turning and breathing
#define SAVER_VINYL 1   // a record of dots, ripples running along the grooves
#define SAVER_RIDGES 1  // dotted ridge lines rolling toward you, after Unknown Pleasures
#define SAVER_VFD 0     // 80s head unit: fluorescent display, dot-matrix spectrum
#define SAVER_WIRED 1   // poles and humming wires at dusk, after Serial Experiments Lain
#define SAVER_HAZE 0    // Gen X soft club: pastel haze, particles, orbit rings, fine print
#define SAVER_NETWORK 1   // the Wired as a graph: drifting nodes, links, packets
#define SAVER_TUNNEL 1    // a tube of dot rings twisting into cyberspace
#define SAVER_SCOPE 1     // oscilloscope X-Y figure in green phosphor
#define SAVER_TERMINAL 1  // hex dumps scrolling, a blinking prompt
#define SAVER_STATIC 1    // TV snow, a rolling band, a dropout with two red words
#define SAVER_NAVI 1      // a made-up 90s OS desktop, windows opening around the clock
#define SAVER_REDSKY 1    // rooftops and crows on the wires against a blood-red dusk
#define SAVER_PSYCHE 1    // the clock as a chip label, circuit traces pulsing outward
#define SAVER_CROSSING 0  // a night street crossing, the walk signal cycling

// ---- Sleep -----------------------------------------------------------------

// Minutes with nothing playing before the video signal turns off. 0 = never.
#define SLEEP_AFTER_MINUTES 60

// ---- Burn-in protection ----------------------------------------------------

// Slow pixel drift against LCD burn-in.
#define BURNIN_SHIFT 1

// ---- Look ------------------------------------------------------------------

// Error-diffusion dithering on the cover (RGB332 bands without it).
#define COVER_DITHER 1
// Sharpen the cover after scaling it down.
#define COVER_SHARPEN 1
