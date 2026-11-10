#pragma once

#include <Arduino.h>

#include "render/color.h"

// Downloads a cover and decodes it straight into dst (ART_W x ART_H, see art.h), picking the
// background palette. With a thumbnail, the cover first gets 256 colours of its own (picked
// from the thumbnail; `colours` says how many, 0 for RGB332). False on failure.
bool fetchCover(const String& url, int imageWidth, const String& thumbUrl, int thumbWidth, uint32_t* dst,
                ScenePalette& pal, int& colours);

// The same from JPEGs saved in a flash partition (a prefetched cover), at those offsets;
// thumbLen 0: no colours of its own.
#include <esp_partition.h>
bool decodeCover(const esp_partition_t* part, uint32_t thumbAt, size_t thumbLen, int thumbWidth, uint32_t imageAt,
                 size_t imageLen, int imageWidth, uint32_t* dst, ScenePalette& pal, int& colours);
