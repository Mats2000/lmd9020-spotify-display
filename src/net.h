#pragma once

#include "render/scene.h"

// Starts the Wi-Fi/NTP/Spotify task on core 0.
void netStart(const Scene* scene);

// Copies the latest state for the render loop. Cheap enough for every frame.
void netSnapshot(NowPlaying& out);
