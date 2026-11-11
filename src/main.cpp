// LMD-9020 Spotify display: composite video from an ESP32 DAC.
// Core 1 renders (this file); core 0 runs Wi-Fi and Spotify (net.cpp).

#include <Arduino.h>
#include <ESP_8_BIT_composite.h>
#include <WiFi.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <sys/time.h>
#include <time.h>

#include "log.h"
#include "config.h"
#include "net.h"
#include "render/scene.h"

// Small render stack: the RAM is needed for HTTPS.
SET_LOOP_TASK_STACK_SIZE(4 * 1024);  // peaks near 2.1 KB

static ESP_8_BIT_composite video(VIDEO_NTSC);
static Scene scene;
static NowPlaying np;  // static to keep ~400 bytes off the loop task's stack

static const dac_channel_t VIDEO_DAC = VIDEO_GPIO == 26 ? DAC_CHANNEL_2 : DAC_CHANNEL_1;

static uint32_t lastActiveMs;  // last time Spotify was playing (or boot, or the button)
static uint32_t lastSwapField = 0;
static bool signalOff = false;
static bool buttonSnow = false;
static uint32_t buttonSnowUntil = 0;

static const int BOOT_BUTTON = 0;

// A stuck main loop (a driver call that never returns, video that doesn't come back) would
// leave the display dark for good while Wi-Fi carries on regardless; a stuck or cut-off network
// task would leave it asleep for good with the music playing. Either way, restart: if the loop
// hasn't come round for LOOP_STALL_MS, the network task for NET_STALL_MS, or Spotify hasn't
// answered for NET_SILENT_MS. Why, and whether the display was asleep, is kept in RTC memory,
// which a restart leaves alone: the next boot logs it and, if asleep, stays dark.
static const uint32_t LOOP_STALL_MS = 15000, NET_STALL_MS = 180000, NET_SILENT_MS = 600000;
enum Stage : uint32_t { STAGE_LOOP, STAGE_SLEEPING, STAGE_WAKING, STAGE_DRAWING, STAGE_FRAME, STAGE_COUNT };
static const char* const STAGE_NAMES[STAGE_COUNT] = {"going round", "going to sleep", "waking up", "drawing",
                                                     "waiting for the video frame"};
static const uint32_t NET_STUCK = 100, NET_SILENT = 101;  // stallStage values past the loop's own
static volatile uint32_t loopBeat, stage;
RTC_NOINIT_ATTR static uint32_t stallMagic, stallStage, stallAsleep;
static const uint32_t STALL_MAGIC = 0x57A11EDu;

static void restartFor(uint32_t why) {
    stallMagic = STALL_MAGIC;
    stallStage = why;
    stallAsleep = signalOff;
    esp_restart();
}

static void checkLoop(void*) {
    if (millis() - loopBeat >= LOOP_STALL_MS) restartFor(stage);
    if (netSinceBeat() >= NET_STALL_MS) restartFor(NET_STUCK);
    if (netSinceSpotify() >= NET_SILENT_MS) restartFor(NET_SILENT);
}

// Logs why the last restart happened; true if it was ours and the display was asleep then.
static bool reportLastReset() {
    const esp_reset_reason_t why = esp_reset_reason();
    const bool ours = why == ESP_RST_SW && stallMagic == STALL_MAGIC;
    stallMagic = 0;
    if (ours && stallStage < STAGE_COUNT)
        say("Restarted: the main loop stalled while %s\n", STAGE_NAMES[stallStage]);
    else if (ours && stallStage == NET_STUCK)
        say("%s\n", "Restarted: the network task stalled");
    else if (ours && stallStage == NET_SILENT)
        say("%s\n", "Restarted: Spotify hadn't answered for 10 minutes");
    else if (why == ESP_RST_PANIC || why == ESP_RST_INT_WDT || why == ESP_RST_TASK_WDT || why == ESP_RST_WDT)
        say("Restarted after a crash or watchdog (reason %d)\n", (int)why);
    else if (why == ESP_RST_BROWNOUT)
        say("%s\n", "Restarted after a brownout: check the power supply");
    return ours && stallAsleep;
}

void setup() {
    Serial.begin(115200);
    say("\nLMD-9020 Spotify display (%s). Free heap %u\n", VIDEO_NTSC ? "NTSC" : "PAL",
                  ESP.getFreeHeap());
    const bool stayAsleep = reportLastReset();

    // Must run on the loop() task: the library notifies the task that called begin().
    ESP_8_BIT_composite::setColor(NTSC_SATURATION, NTSC_HUE);
    ESP_8_BIT_composite::setStandardLine(NTSC_STANDARD_LINE);
    ESP_8_BIT_composite::setInterlace(NTSC_INTERLACE);
    video.begin();
    // The library starts on GPIO25; its I2S DAC is mono, so GPIO26 can carry it instead.
#if VIDEO_GPIO == 26
    dac_output_enable(DAC_CHANNEL_2);
    dac_output_disable(DAC_CHANNEL_1);
#endif
    say("Video running. Free heap %u, largest block %u\n", ESP.getFreeHeap(),
                  ESP.getMaxAllocHeap());

    pinMode(BOOT_BUTTON, INPUT_PULLUP);
    netStart(&scene);
    lastActiveMs = millis();
    if (stayAsleep) {  // the watchdog restarted it while asleep: stay dark until something plays
        lastActiveMs = millis() - SLEEP_AFTER_MINUTES * 60000UL;
        dac_output_disable(VIDEO_DAC);
        ESP_8_BIT_composite::pause();
        signalOff = true;
    }

    loopBeat = millis();
    const esp_timer_create_args_t check = {checkLoop, nullptr, ESP_TIMER_TASK, "loop check"};
    esp_timer_handle_t timer;
    if (esp_timer_create(&check, &timer) == ESP_OK) esp_timer_start_periodic(timer, 1000000);
}

// The BOOT button starts (or stops) a panel refresh, and wakes the display.
static void pollButton() {
    static bool wasDown = false;
    static uint32_t changedMs = 0;
    bool down = digitalRead(BOOT_BUTTON) == LOW;
    if (down != wasDown && millis() - changedMs > 50) {
        wasDown = down;
        changedMs = millis();
        if (down) {
            buttonSnow = !buttonSnow;
            buttonSnowUntil = millis() + REFRESH_BUTTON_MINUTES * 60000UL;
            lastActiveMs = millis();
            say("%s\n", buttonSnow ? "Button: panel refresh on" : "Button: panel refresh off");
        }
    }
    if (buttonSnow && (int32_t)(millis() - buttonSnowUntil) >= 0) buttonSnow = false;
}

// The cover's own colours, encoded at the scene's brightness, for its rectangle on screen.
static void showCoverPalette(const CoverRegion& r) {
    static uint32_t version = 0;
    static int level = -1;
    if (!r.on) {
        ESP_8_BIT_composite::setRegion(0, 0, 0, 0, nullptr);
        level = -1;
        return;
    }
    uint32_t* art = const_cast<uint32_t*>(r.art);
    uint32_t* encoded = artEncoded(art);
    if (r.version != version || r.level != level) {  // a new cover, or fading
        const uint32_t* rgb = artPalette(art);
        for (int i = 0; i < r.colours; i++) {
            uint32_t c = rgb[i];
            encoded[i] = ESP_8_BIT_composite::encodeColor(c >> 16, (c >> 8) & 255, c & 255, r.level);
        }
        version = r.version, level = r.level;
    }
    ESP_8_BIT_composite::setRegion(r.x0, r.y0, r.x1, r.y1, encoded);
}

// The visualizer's palette, encoded at the scene's brightness, for every row while it's on.
static uint32_t glowTable[256];

static void showGlowPalette(const GlowRegion& g) {
    static int amount = -1, level = -1, style = -1, scheme = -1;
    static uint32_t one = 0;
    static uint32_t version = 0;
    static bool wasOn = false;
    if (g.on != wasOn) {
        say("%s\n", g.on ? "Visualizer: on" : "Visualizer: off");
        wasOn = g.on;
    }
    if (!g.on) {
        ESP_8_BIT_composite::setBand(0, 0, nullptr);
        amount = level = style = scheme = -1;
        return;
    }
    if (g.look.amount != amount || g.level != level || g.version != version || g.style != style || g.scheme != scheme || g.one != one) {
        for (int i = 0; i < 256; i++) {
            RGB c = glowColour(g.look, i);
            glowTable[i] = ESP_8_BIT_composite::encodeColor(c.r, c.g, c.b, g.level);
        }
        amount = g.look.amount, level = g.level, version = g.version, style = g.style, scheme = g.scheme, one = g.one;
    }
    ESP_8_BIT_composite::setBand(0, Canvas::H, glowTable);
}

// Sleep: after SLEEP_AFTER_MINUTES with nothing playing, fade out and power down
// the DAC so the monitor sees no input. With nothing to make, the video stops too and the
// Wi-Fi stays fully awake: Spotify is still asked every POLL_IDLE_MS, so playing again wakes it.
// (Letting the radio doze, or switching the CPU clock, saved a little but after hours left it
// unable to reach Spotify, or hung.) Returns true while the signal is off.
static bool handleSleep() {
    if (np.playing) lastActiveMs = millis();
    const uint32_t sleepMs = SLEEP_AFTER_MINUTES * 60000UL, idleMs = millis() - lastActiveMs;
    bool sleepy = SLEEP_AFTER_MINUTES > 0 && idleMs >= sleepMs;
    bool lastMinutes = SLEEP_AFTER_MINUTES > 0 && idleMs + REFRESH_BEFORE_SLEEP_SECONDS * 1000UL >= sleepMs;
    scene.holdSnow(buttonSnow || lastMinutes);
    scene.setAwake(!sleepy);

    if (sleepy && scene.dark() && !signalOff) {
        stage = STAGE_SLEEPING;
        dac_output_disable(VIDEO_DAC);
        ESP_8_BIT_composite::pause();
        signalOff = true;
        say("%s\n", "Nothing played for a while: video signal off");
    } else if (!sleepy && signalOff) {
        stage = STAGE_WAKING;
        ESP_8_BIT_composite::resume();
        dac_output_enable(VIDEO_DAC);
        signalOff = false;
        say("%s\n", "Playing again: video signal on");
    }
    return signalOff;
}

void loop() {
    loopBeat = millis();
    stage = STAGE_LOOP;
    netSnapshot(np);
    pollButton();
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
    canvas.shown = video.getDisplayedFrameBufferLines();
    uint32_t start = micros();
    stage = STAGE_DRAWING;
    scene.render(canvas, np, clock, millis());
    if (np.audio.flashUntil && (int32_t)(np.audio.flashUntil - millis()) > 0) {
        canvas.fill(0xFF);  // white, with the cover's and the glow's palettes off
        ESP_8_BIT_composite::setRegion(0, 0, 0, 0, nullptr);
        ESP_8_BIT_composite::setBand(0, 0, nullptr);
    } else {
        showCoverPalette(scene.coverRegion());
        showGlowPalette(scene.glowRegion());
    }

    // Over two fields (33 ms) drops a frame.
    static uint32_t slowest = 0, total = 0, frames = 0, overBudget = 0, lastReport = 0;
    uint32_t took = micros() - start;
    if (took > slowest) slowest = took;
    total += took;
    frames++;
    if (took > (VIDEO_NTSC ? 33366u : 40000u)) overBudget++;  // two fields
    if (millis() - lastReport > 30000) {
        say("Last 30 s: %u frames, avg %u us, slowest %u us, %u over budget. "
                      "Heap %u, largest %u, render stack left %u\n",
                      frames, frames ? total / frames : 0, slowest, overBudget,
                      heap_caps_get_free_size(MALLOC_CAP_8BIT), heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
                      uxTaskGetStackHighWaterMark(nullptr));
        slowest = total = frames = overBudget = 0;
        lastReport = millis();
    }

    // Hold each frame for two fields: 30 fps (motion is time-based).
    stage = STAGE_FRAME;
    while (video.getRenderedFrameCount() == lastSwapField) vTaskDelay(1);
    video.waitForFrame();
    lastSwapField = video.getRenderedFrameCount();
}
