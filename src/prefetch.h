#pragma once

#include "render/color.h"
#include "spotify.h"

// The next song's cover, downloaded ahead into the spare flash partition (the "spiffs" one in
// min_spiffs.csv, 128 KB), so it shows the moment the song changes. The flash is erased a
// sector at a time between polls: each erase pauses the other core for tens of ms.
namespace prefetch {

bool begin();
// Is the stored cover this track's?
bool holds(const String& key);
// Decodes the stored cover into art; false if it isn't this track's or doesn't decode.
bool load(const Track& t, uint32_t* art, ScenePalette& pal, int& colours);
// Drops the stored cover; the slot is erased bit by bit with eraseStep().
void forget();
// Erases one sector if any is left; false when there's nothing to do.
bool eraseStep();
bool clean();
// Downloads t's cover (thumbnail and image) into the slot, erasing what's left first.
bool store(const Track& t);

}  // namespace prefetch
