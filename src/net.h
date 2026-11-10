#pragma once

#include "render/scene.h"

// Starts the Wi-Fi/NTP/Spotify task on core 0.
void netStart(const Scene* scene);

// Copies the latest state for the render loop. Cheap enough for every frame.
void netSnapshot(NowPlaying& out);

// For the watchdog: ms since the network task last went round its loop, and since Spotify last
// answered (playing or not).
uint32_t netSinceBeat();
uint32_t netSinceSpotify();

// While an update comes in over Wi-Fi: no Spotify requests (they'd compete for RAM and air).
void netPause(bool paused);
