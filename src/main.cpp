// LMD-9020 Spotify display: composite video from an ESP32 DAC.
// Core 1 renders (this file); core 0 runs Wi-Fi and Spotify (net.cpp).

#include <Arduino.h>
#include <ESP_8_BIT_composite.h>
#include <sys/time.h>
#include <time.h>

#include "config.h"
#include "net.h"
#include "render/scene.h"

// Small render stack: the RAM is needed for HTTPS.
SET_LOOP_TASK_STACK_SIZE(4 * 1024);  // peaks near 2.1 KB

static ESP_8_BIT_composite video(VIDEO_NTSC);
static Scene scene;
static NowPlaying np;  // static to keep ~400 bytes off the loop task's stack

static const dac_channel_t VIDEO_DAC = VIDEO_GPIO == 26 ? DAC_CHANNEL_2 : DAC_CHANNEL_1;

static uint32_t lastActiveMs;  // last time Spotify was playing (or boot)
static uint32_t lastSwapField = 0;
static bool signalOff = false;

void setup() {
    Serial.begin(115200);
    Serial.printf("\nLMD-9020 Spotify display (%s). Free heap %u\n", VIDEO_NTSC ? "NTSC" : "PAL",
                  ESP.getFreeHeap());

    // Must run on the loop() task: the library notifies the task that called begin().
    video.begin();
    // The library starts on GPIO25; its I2S DAC is mono, so GPIO26 can carry it instead.
#if VIDEO_GPIO == 26
    dac_output_enable(DAC_CHANNEL_2);
    dac_output_disable(DAC_CHANNEL_1);
#endif
    Serial.printf("Video running. Free heap %u, largest block %u\n", ESP.getFreeHeap(),
                  ESP.getMaxAllocHeap());

    netStart(&scene);
    lastActiveMs = millis();
}

// Sleep: after SLEEP_AFTER_MINUTES with nothing playing, fade out and power down
// the DAC so the monitor sees no input. Returns true while the signal is off.
static bool handleSleep() {
    if (np.playing) lastActiveMs = millis();
    bool sleepy = SLEEP_AFTER_MINUTES > 0 && millis() - lastActiveMs >= SLEEP_AFTER_MINUTES * 60000UL;
    scene.setAwake(!sleepy);

    if (sleepy && scene.dark() && !signalOff) {
        dac_output_disable(VIDEO_DAC);
        signalOff = true;
        Serial.println("Nothing played for a while: video signal off");
    } else if (!sleepy && signalOff) {
        dac_output_enable(VIDEO_DAC);
        signalOff = false;
        Serial.println("Playing again: video signal on");
    }
    return signalOff;
}

void loop() {
    netSnapshot(np);
    if (handleSleep()) {
        delay(100);  // nothing to draw; just keep an eye on Spotify
        return;
    }

    timeval tv;
    gettimeofday(&tv, nullptr);
    tm local;
    localtime_r(&tv.tv_sec, &local);
    ClockTime clock;
    clock.valid = tv.tv_sec > 1700000000;
    clock.hour = local.tm_hour;
    clock.minute = local.tm_min;
    clock.millis = (int)(tv.tv_usec / 1000);
    clock.wday = local.tm_wday;
    clock.mon = local.tm_mon;
    clock.mday = local.tm_mday;

    Canvas canvas{video.getFrameBufferLines()};
    uint32_t start = micros();
    scene.render(canvas, np, clock, millis());

    // Over two fields (33 ms) drops a frame.
    static uint32_t slowest = 0, total = 0, frames = 0, overBudget = 0, lastReport = 0;
    uint32_t took = micros() - start;
    if (took > slowest) slowest = took;
    total += took;
    frames++;
    if (took > (VIDEO_NTSC ? 33366u : 40000u)) overBudget++;  // two fields
    if (millis() - lastReport > 30000) {
        Serial.printf("Last 30 s: %u frames, avg %u us, slowest %u us, %u over budget. "
                      "Heap %u, largest %u, render stack left %u\n",
                      frames, frames ? total / frames : 0, slowest, overBudget,
                      heap_caps_get_free_size(MALLOC_CAP_8BIT), heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
                      uxTaskGetStackHighWaterMark(nullptr));
        slowest = total = frames = overBudget = 0;
        lastReport = millis();
    }

    // Hold each frame for two fields: 30 fps (motion is time-based).
    while (video.getRenderedFrameCount() == lastSwapField) vTaskDelay(1);
    video.waitForFrame();
    lastSwapField = video.getRenderedFrameCount();
}
