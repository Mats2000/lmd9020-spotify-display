#pragma once

#include <stdint.h>
#include <stdlib.h>
#ifdef ARDUINO
#include <esp_heap_caps.h>
#endif

// Memory to be read and written only a whole 32-bit word at a time: IRAM proper when there's
// room, since ordinary RAM is what runs short (a cover download needs ~10 KB of it), and
// ordinary RAM otherwise. Free with free().
inline void* wordRam(size_t bytes) {
#ifdef ARDUINO
    // Only 0x4008_0000..0x400A_0000: the rest of the executable heap is ordinary RAM seen through
    // another address, which would gain nothing.
    void* p = heap_caps_malloc(bytes, MALLOC_CAP_EXEC);
    if (p && ((uintptr_t)p < 0x40080000 || (uintptr_t)p + bytes > 0x400A0000)) {
        heap_caps_free(p);
        p = nullptr;
    }
    if (p) return p;
#endif
    return malloc(bytes);
}
