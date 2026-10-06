// The display's own scene code, for the app's live preview (demo.cpp).
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// A new track: the cover as 320x320 RGB (what the firmware decodes) and 64x64 RGB (Spotify's
// thumbnail, for the cover's own colours); either may be null for a placeholder.
void lmd_demo_set_track(const uint8_t* cover320, const uint8_t* thumb64, const char* title, const char* artist);
void lmd_demo_set_playing(int playing, int paused);

// One frame (call about 30 times a second): the same bands and style the display is sent, the
// local time, and out = 256x240 RGB as the monitor would show it.
void lmd_demo_render(const uint8_t* bands16, uint8_t beat, int active, int style, uint32_t ms, int hour, int minute,
                     int wday, int mon, int mday, uint8_t* out);

#ifdef __cplusplus
}
#endif
