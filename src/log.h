#pragma once

#include <stdint.h>

// A log line (printf-style; end it with \n): to the serial port, and to the Mac running the
// visualizer (UDP, its port + 2), which keeps them in ~/Library/Logs/Show Control/display.log.
void say(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void logStart();
void logTo(uint32_t ip);  // network order; set from the visualizer's packets
