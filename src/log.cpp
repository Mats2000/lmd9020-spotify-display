#include "log.h"

#include <Arduino.h>
#include <lwip/sockets.h>
#include <stdarg.h>

#include "config.h"

namespace {
volatile uint32_t target = 0;
int sock = -1;
SemaphoreHandle_t lock;
}  // namespace

void logStart() { lock = xSemaphoreCreateMutex(); }
void logTo(uint32_t ip) { target = ip; }

void say(const char* fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    if (n >= (int)sizeof(buf)) n = sizeof(buf) - 1;
    Serial.write((const uint8_t*)buf, n);
    if (!target || !lock || xSemaphoreTake(lock, pdMS_TO_TICKS(20)) != pdTRUE) return;
    if (sock < 0) sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock >= 0) {
        sockaddr_in a = {};
        a.sin_family = AF_INET;
        a.sin_port = htons(VISUALIZER_PORT + 2);
        a.sin_addr.s_addr = target;
        sendto(sock, buf, n, MSG_DONTWAIT, (sockaddr*)&a, sizeof(a));
    }
    xSemaphoreGive(lock);
}
