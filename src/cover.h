#pragma once

#include <Arduino.h>

#include "render/color.h"

// Downloads a cover and decodes it straight into dst (ART_W x ART_H, see art.h),
// picking the background palette. False on failure.
bool fetchCover(const String& url, int imageWidth, uint32_t* dst, ScenePalette& pal);
