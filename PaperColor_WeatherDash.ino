// ============================================================
// PaperColor_WeatherDash.ino
// M5Paper Color — Sydney Weather Dashboard
// Portrait 400x600 — Single page main display
// ============================================================

#include <Arduino.h>
#include <M5Unified.h>
#include "config.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <WebServer.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <SPI.h>
#include <SD.h>
#include <time.h>
#include <esp_sntp.h>
#include <Adafruit_NeoPixel.h>
#include <M5UnitENV.h>
#include <M5PM1.h>

// ------------------------------------------------------------
// CONFIG — credentials in config.h (keep local, don't share)
// ------------------------------------------------------------
const char* WEATHER_URL = "https://api.weatherapi.com/v1/forecast.json?key=%s&q=Sydney&days=3&aqi=yes&alerts=no";

// ------------------------------------------------------------
// LAYOUT CONSTANTS  (prefix PC_ = PaperColor)
// ------------------------------------------------------------
static const int PC_W = 400;
static const int PC_H = 600;

static const int PC_Z1_Y = 0;    static const int PC_Z1_H = 28;   // Status bar
static const int PC_Z2_Y = 28;   static const int PC_Z2_H = 82;   // Date
static const int PC_Z3_Y = 110;  static const int PC_Z3_H = 76;   // Indoor
static const int PC_Z4_Y = 186;  static const int PC_Z4_H = 172;  // Weather
static const int PC_Z5_Y = 358;  static const int PC_Z5_H = 136;  // Forecast
static const int PC_Z6_Y = 494;  static const int PC_Z6_H = 106;  // Calm strip

// ── Exact ACeP hardware colours (6-colour panel, measured from device) ──
// Red #CC2200 / Yellow #CC8900 / Green #1A6B2A / Blue #1155CC / Black #1A1A1A / White #FFFFFF
static const uint16_t PC_RED    = 0xC900;   // #CC2200 — strong dark red
static const uint16_t PC_YELLOW = 0xCC40;   // #CC8900 — golden yellow (was "amber/orange")
static const uint16_t PC_GREEN  = 0x1B45;   // #1A6B2A — forest green
static const uint16_t PC_BLUE   = 0x12B9;   // #1155CC — mid blue
static const uint16_t PC_BLACK  = 0x18C3;   // #1A1A1A — near black
// Keep PC_AMBER as alias for yellow — used throughout for sun icons etc.
static const uint16_t PC_AMBER  = 0xCC40;   // same as PC_YELLOW
static const uint16_t PC_GREY   = 0x8410;   // mid grey for clouds/dividers
static const uint16_t PC_LGREY  = 0xF7BE;   // light grey for background strips

// ------------------------------------------------------------
// DATA STRUCTS
// ------------------------------------------------------------
struct WeatherData {
    float temp_c      = 0;
    float feelslike_c = 0;
    int   humidity    = 0;
    int   rain_chance = 0;
    int   uv          = 0;
    char  condition[32] = "";
    char  fc_day[3][8]  = {};
    int   fc_max[3]     = {};
    int   fc_rain[3]    = {};
    int   fc_code[3]    = {};
    // Astronomy from forecastday[0].astro
    char  sunrise[12]        = "";
    char  sunset[12]         = "";
    char  moonrise[20]       = "";
    char  moonset[20]        = "";
    char  moon_phase[32]     = "";
    int   moon_illumination  = 0;
    int   aqi_index          = 0;  // US EPA index 1-6
    // Current conditions extras
    float wind_kph       = 0;
    int   wind_degree    = 0;
    char  wind_dir[8]    = "";
    float pressure_mb    = 0;
    float vis_km         = 0;
    // Hourly data — hours 6..22 (index 0=06:00 .. 16=22:00)
    float hourly_temp[17]  = {};
    int   hourly_rain[17]  = {};
};

struct SensorData {
    float temp_c   = 0;
    float humidity = 0;
};

struct RtcData {
    int  day   = 1;
    int  month = 1;
    int  year  = 2026;
    int  hour  = 0;
    int  min   = 0;
    char dayname[12] = "---";
};

// ------------------------------------------------------------
// GLOBALS
// ------------------------------------------------------------
M5Canvas canvas(&M5.Display);

// NVS — persists across full power cycles (pm1.shutdown wipes RTC_DATA_ATTR)
Preferences prefs;

// Page state — loaded from NVS on boot, saved on change
int gCurrentPage = 1;  // 1=dashboard, 2=hourly graph, 3=calendar, 4=photo

// Photo mode — device stays awake, no sleep cycle
bool gPhotoMode = false;
static uint8_t* gImageBuffer = nullptr;  // 240KB in PSRAM, null = no image
static const int IMG_W = 400;
static const int IMG_H = 600;
static const int IMG_SIZE = IMG_W * IMG_H;  // 240000 bytes, 1 byte per pixel (0-5)

// Wake schedule — loaded from NVS on boot
int gWakeStart    = 6;   // first wake hour
int gWakeEnd      = 20;  // last wake hour
int gWakeInterval = 2;   // hours between wakes

// Web server
WebServer server(80);
static bool gWebMode = false;

// ── LED animation state ──
enum LedMode { LED_STATUS, LED_OFF, LED_SOLID, LED_PULSE, LED_FLASH, LED_RAINBOW };
LedMode  gLedMode       = LED_STATUS;
uint8_t  gLedR          = 255;
uint8_t  gLedG          = 255;
uint8_t  gLedB          = 255;
uint8_t  gLedBrightness = 80;

// ── SD card ──
static constexpr uint8_t SD_CS   = 47;
static constexpr uint8_t SD_SCK  = 15;
static constexpr uint8_t SD_MOSI = 13;
static constexpr uint8_t SD_MISO = 14;
bool gSdReady = false;

// ── Slideshow ──
bool     gSlideshowMode     = false;
int      gSlideInterval     = 300;   // seconds between slides
uint32_t gLastSlideMs       = 0;
int      gSlideIndex        = 0;
static const int MAX_PHOTOS = 64;
static char gPhotoList[MAX_PHOTOS][32];
int      gPhotoCount        = 0;

void renderPage2();
void renderPage3();
SHT4X sht4;
static constexpr int SHT_SDA_PIN = 3;
static constexpr int SHT_SCL_PIN = 2;
M5PM1 pm1;
static bool pm1_ready = false;
Adafruit_NeoPixel pixels(2, 21, NEO_GRB + NEO_KHZ800);
static const char* const RTC_WD[7] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
WeatherData  gWeather;
SensorData   gSensor;
RtcData      gRtc;
bool gWifiOk  = false;
bool gFetchOk = false;
int  gBattPct = 0;

// ------------------------------------------------------------
// FORWARD DECLARATIONS
// ------------------------------------------------------------
void initDisplay();
void readSensor();
void readRTC();
void connectWiFi();
bool fetchWeather();
void renderDashboard();
void renderPage2();
void renderPage3();
void renderPage4();
void renderWebMode();
void startWebServer();
void stopWebServer();
void handleWebLoop();
void savePrefs();
void loadPrefs();
void drawStatusBar(int pageNum);
void drawZone1_StatusBar();
void drawZone2_Date();
void drawZone3_Indoor();
void drawZone4_Weather();
void drawZone5_Forecast();
void drawZone6_Calm();
void drawWeatherIcon(M5Canvas* c, int cx, int cy, int code, int w, int h);
void drawSunIcon(M5Canvas* c, int cx, int cy, int r);
void drawCloudIcon(M5Canvas* c, int cx, int cy, int w, int h, uint16_t col);
void drawRainIcon(M5Canvas* c, int cx, int cy);
void drawStormIcon(M5Canvas* c, int cx, int cy);
void drawPartlyCloudyIcon(M5Canvas* c, int cx, int cy);
void drawCompassArrow(M5Canvas* c, int cx, int cy, int r, int degrees);
void drawMoonIcon(M5Canvas* c, int cx, int cy, int r, float illum, bool waxing);
const char* uvLabel(int uv);
uint16_t uvColour(int uv);
const char* aqiLabel(int aqi);
uint16_t aqiColour(int aqi);
const char* moonTimeDisplay(const char* t);
const char* monthName(int m);
const char* fullDayName(int dow);
void goToSleep();
void updateLeds();
bool initSD();
void logSensorReading();
void saveBmpToSD(const char* path);
void loadPhotoList();
void advanceSlideshow();
void renderSlideFromSD(const char* path);


// ============================================================
// RGB LED SYSTEM — 2x WS2812B on pin 21
// Supports: status-driven, off, solid, pulse, flash, rainbow
// ============================================================
static void ledsOff()
{
    pixels.clear();
    pixels.show();
}

static void setAllLeds(uint32_t color)
{
    pixels.setPixelColor(0, color);
    pixels.setPixelColor(1, color);
    pixels.show();
}

// Status-driven helpers — only active when gLedMode == LED_STATUS
static void ledBoot()      { if (gLedMode==LED_STATUS) setAllLeds(pixels.Color(0,0,255)); }
static void ledWifi()      { if (gLedMode==LED_STATUS) setAllLeds(pixels.Color(255,140,0)); }
static void ledFetching()  { if (gLedMode==LED_STATUS) setAllLeds(pixels.Color(0,255,0)); }
static void ledRendering() { if (gLedMode==LED_STATUS) setAllLeds(pixels.Color(255,255,255)); }
static void ledSleeping()  { if (gLedMode==LED_STATUS) ledsOff(); }
static void ledWebMode()   { if (gLedMode==LED_STATUS) setAllLeds(pixels.Color(0,80,255)); }

static void ledConfirm()
{
    // Brief red flash to confirm button press — always fires regardless of mode
    setAllLeds(pixels.Color(255, 0, 0));
    delay(300);
    // Restore will happen on next updateLeds() call
}

// ── Non-blocking LED animation — call every loop iteration ──
void updateLeds()
{
    if (gLedMode == LED_STATUS || gLedMode == LED_OFF) return;  // handled by status helpers or off

    uint32_t now = millis();
    pixels.setBrightness(gLedBrightness);

    switch (gLedMode) {

        case LED_SOLID:
            setAllLeds(pixels.Color(gLedR, gLedG, gLedB));
            break;

        case LED_PULSE: {
            // Sine wave on brightness, ~3 second period
            float phase = (now % 3000) / 3000.0f;
            float bright = (sinf(phase * 2.0f * PI) + 1.0f) / 2.0f;  // 0.0-1.0
            uint8_t b = (uint8_t)(bright * gLedBrightness);
            pixels.setBrightness(255);  // drive full, modulate via colour
            uint8_t r = (uint8_t)(gLedR * bright);
            uint8_t g = (uint8_t)(gLedG * bright);
            uint8_t bl = (uint8_t)(gLedB * bright);
            setAllLeds(pixels.Color(r, g, bl));
            break;
        }

        case LED_FLASH: {
            // 200ms on, 800ms off
            bool on = ((now % 1000) < 200);
            setAllLeds(on ? pixels.Color(gLedR, gLedG, gLedB) : 0);
            break;
        }

        case LED_RAINBOW: {
            // Full hue cycle, ~5 second period
            uint8_t hue = (uint8_t)((now % 5000) * 255 / 5000);
            uint32_t col = pixels.ColorHSV(hue * 256, 255, gLedBrightness);
            pixels.setPixelColor(0, col);
            // Offset second LED by 128 hue steps for variation
            pixels.setPixelColor(1, pixels.ColorHSV((hue + 128) * 256, 255, gLedBrightness));
            pixels.show();
            break;
        }

        default: break;
    }
}

// ============================================================
// SETUP
// ============================================================
void setup()
{
    auto cfg          = M5.config();
    cfg.clear_display = false;
    M5.begin(cfg);

    Serial.begin(115200);
    delay(500);
    Serial.println("=== PaperColor boot ===");
    Serial.println(">> M5.begin done");

    // Load persisted settings from NVS
    loadPrefs();
    Serial.printf("   NVS: page=%d  wake=%02d:00-%02d:00 every %dh\n",
                  gCurrentPage, gWakeStart, gWakeEnd, gWakeInterval);

    // Init display and canvas FIRST — matches official example order
    Serial.println(">> initDisplay");
    initDisplay();

    // NeoPixel init
    pixels.begin();
    pixels.setBrightness(80);
    pixels.clear();
    pixels.show();
    ledBoot();  // Blue — booting

    // M5PM1 init AFTER canvas created — matches official example order
    Serial.println(">> M5PM1 init");
    m5pm1_err_t pm1_err = pm1.begin(&M5.In_I2C, 0x6E, M5PM1_I2C_FREQ_100K);
    pm1_ready = (pm1_err == M5PM1_OK);
    if (pm1_ready) {
        pm1.setLdoEnable(true);
        Serial.println("   M5PM1: OK");
    } else {
        Serial.printf("   M5PM1: FAILED err=%d\n", pm1_err);
    }

    // Init SD card — must be after PM1 (needs PYG3/PYG4 control)
    Serial.println(">> initSD");
    gSdReady = initSD();
    Serial.printf("   SD: %s\n", gSdReady ? "OK" : "not available");

    Serial.println(">> readRTC");
    readRTC();
    Serial.printf("   RTC: %s %d/%d/%d  %02d:%02d\n",
                  gRtc.dayname, gRtc.day, gRtc.month, gRtc.year,
                  gRtc.hour, gRtc.min);

    Serial.println(">> readSensor");
    readSensor();
    Serial.printf("   SHT40: %.1f C  %.0f%%\n", gSensor.temp_c, gSensor.humidity);

    // Log sensor reading to SD
    if (gSdReady) {
        logSensorReading();
        loadPhotoList();
        Serial.printf("   Photos on SD: %d\n", gPhotoCount);
    }

    // Auto-clear photo/slideshow mode on boot
    // PSRAM is always empty after a full power cycle
    Serial.printf("   Boot check: photoMode=%d page=%d slideshow=%d\n",
                  gPhotoMode, gCurrentPage, gSlideshowMode);
    if (gPhotoMode && gCurrentPage != 4) {
        Serial.println("   Photo mode cleared — page is not 4");
        gPhotoMode = false;
        savePrefs();
    }
    if (gPhotoMode && gImageBuffer == nullptr && gPhotoCount == 0) {
        Serial.println("   Photo mode cleared — no image available");
        gPhotoMode = false;
        gCurrentPage = 1;
        savePrefs();
    }
    if (gSlideshowMode && gPhotoCount == 0) {
        Serial.println("   Slideshow cleared — no photos on SD");
        gSlideshowMode = false;
        savePrefs();
    }
    Serial.printf("   After clear: photoMode=%d page=%d\n", gPhotoMode, gCurrentPage);

    ledWifi();
    // Only skip WiFi if we're actually going to show a photo/slideshow
    bool willShowPhoto = (gSlideshowMode && gPhotoCount > 0) ||
                         (gPhotoMode && gCurrentPage == 4);
    if (!willShowPhoto) {
        Serial.println(">> connectWiFi");
        connectWiFi();
        Serial.printf("   WiFi: %s\n", gWifiOk ? "OK" : "FAILED");

        if (gWifiOk) {
            syncNTP();
            ledFetching();
            Serial.println(">> fetchWeather");
            gFetchOk = fetchWeather();
            Serial.printf("   fetch: %s\n", gFetchOk ? "OK" : "FAILED");
            if (gFetchOk) {
                Serial.printf("   temp=%.1f  humidity=%d  rain=%d  uv=%d\n",
                              gWeather.temp_c, gWeather.humidity,
                              gWeather.rain_chance, gWeather.uv);
                Serial.printf("   condition: %s\n", gWeather.condition);
                for (int i = 0; i < 3; i++) {
                    Serial.printf("   fc[%d]: %s  max=%d  rain=%d  code=%d\n", i,
                                  gWeather.fc_day[i], gWeather.fc_max[i],
                                  gWeather.fc_rain[i], gWeather.fc_code[i]);
                }
            }
        }
    } else {
        Serial.println(">> Photo/slideshow mode — skipping WiFi/weather fetch");
    }

    // If fetch failed or returned zeros, use test data so display renders
    if (!gFetchOk || gWeather.temp_c == 0.0f) {
        Serial.println(">> using test data for display");
        gWeather.temp_c      = 19.0f;
        gWeather.feelslike_c = 17.0f;
        gWeather.humidity    = 68;
        gWeather.rain_chance = 30;
        gWeather.uv          = 3;
        strncpy(gWeather.condition, "Partly Cloudy", sizeof(gWeather.condition) - 1);
        gWeather.aqi_index   = 1;
        gWeather.wind_kph    = 20.0f;
        gWeather.wind_degree = 315;
        strncpy(gWeather.wind_dir, "NNW", sizeof(gWeather.wind_dir) - 1);
        gWeather.pressure_mb = 1014.0f;
        gWeather.vis_km      = 10.0f;
        // Plausible Sydney day — cool morning, warm midday, cooling evening
        float testTemps[17] = {13,14,15,16,18,19,20,20,19,18,17,16,15,14,14,13,12};
        int   testRain[17]  = {0,0,5,10,20,30,20,10,5,0,0,0,0,0,0,0,0};
        for (int i = 0; i < 17; i++) {
            gWeather.hourly_temp[i] = testTemps[i];
            gWeather.hourly_rain[i] = testRain[i];
        }
        strncpy(gWeather.fc_day[0], "Mon", 7);
        strncpy(gWeather.fc_day[1], "Tue", 7);
        strncpy(gWeather.fc_day[2], "Wed", 7);
        gWeather.fc_max[0]  = 19; gWeather.fc_rain[0] = 30; gWeather.fc_code[0] = 1003;
        gWeather.fc_max[1]  = 22; gWeather.fc_rain[1] = 5;  gWeather.fc_code[1] = 1000;
        gWeather.fc_max[2]  = 21; gWeather.fc_rain[2] = 15; gWeather.fc_code[2] = 1003;
    }

    // If SHT40 returned zeros use test data
    if (gSensor.temp_c == 0.0f) {
        gSensor.temp_c   = 22.0f;
        gSensor.humidity = 55.0f;
    }

    // If RTC not set use placeholder
    if (gRtc.year < 2020) {
        gRtc.day   = 3;
        gRtc.month = 6;
        gRtc.year  = 2026;
        gRtc.hour  = 9;
        gRtc.min   = 0;
        strncpy(gRtc.dayname, "Tuesday", sizeof(gRtc.dayname) - 1);
    }

    // Render current page
    ledRendering();
    Serial.printf(">> rendering page %d  photoMode=%d slideshow=%d\n",
                  gCurrentPage, gPhotoMode, gSlideshowMode);
    if (gSlideshowMode && gPhotoCount > 0) {
        // Advance slide index on each wake — this IS the slideshow cycle
        gSlideIndex = (gSlideIndex + 1) % gPhotoCount;
        savePrefs();  // persist new index so next wake advances correctly
        Serial.printf("   Slideshow: showing %d of %d (%s)\n",
                      gSlideIndex + 1, gPhotoCount, gPhotoList[gSlideIndex]);
        renderSlideFromSD(gPhotoList[gSlideIndex]);
        gLastSlideMs = millis();
    } else if (gPhotoMode && gCurrentPage == 4 && gPhotoCount > 0) {
        // Photo mode, slideshow off — re-render current photo from SD
        Serial.printf("   Photo mode: re-rendering %s\n", gPhotoList[gSlideIndex]);
        renderSlideFromSD(gPhotoList[gSlideIndex]);
    } else {
        switch (gCurrentPage) {
            case 2:  renderPage2(); break;
            case 3:  renderPage3(); break;
            case 4:  renderPage4(); break;
            default: renderDashboard(); break;
        }
    }
    Serial.println(">> entering awake loop");
    ledsOff();
}

// ============================================================
// LOOP
// ============================================================
// ============================================================
// SMART WAKE SCHEDULE
// Active window: 06:00–20:00, refresh every 2 hours
// Outside window: sleep until 06:00
// Returns seconds until next scheduled wake
// ============================================================
static const uint32_t AWAKE_TIMEOUT_MS = 2 * 60 * 1000;  // 2 minutes

uint32_t secondsUntilNextWake()
{
    int nowH     = gRtc.hour;
    int nowM     = gRtc.min;
    int nowTotal = nowH * 60 + nowM;

    // Walk forward from gWakeStart to gWakeEnd in gWakeInterval steps
    for (int h = gWakeStart; h <= gWakeEnd; h += gWakeInterval) {
        int slotTotal = h * 60;
        if (slotTotal > nowTotal) {
            int delta = slotTotal - nowTotal;
            Serial.printf("   Next wake: %02d:00 in %d min\n", h, delta);
            return (uint32_t)delta * 60;
        }
    }
    // Past last slot — sleep until gWakeStart next day
    int minutesUntilMidnight = (24 * 60) - nowTotal;
    int minutesFromStart      = gWakeStart * 60;
    int delta = minutesUntilMidnight + minutesFromStart;
    Serial.printf("   Past %02d:00 — sleeping until %02d:00 (%d min)\n",
                  gWakeEnd, gWakeStart, delta);
    return (uint32_t)delta * 60;
}

void goToSleep()
{
    ledSleeping();
    uint32_t sleepSecs;
    if (gSlideshowMode && gPhotoCount > 0) {
        // Slideshow active — sleep for slide interval
        sleepSecs = (uint32_t)gSlideInterval;
        Serial.printf(">> slideshow sleep %lu seconds\n", sleepSecs);
    } else {
        // Normal schedule
        sleepSecs = secondsUntilNextWake();
    }
    Serial.printf(">> sleeping %lu seconds\n", sleepSecs);
    delay(200);
    // Use pm1 directly — M5.Power.timerSleep() does not reliably
    // wake the device on this hardware revision.
    // pm1.timerSet + pm1.shutdown is the confirmed working pattern.
    if (pm1_ready) {
        pm1.timerSet(sleepSecs, M5PM1_TIM_ACTION_POWERON);
        pm1.shutdown();
    } else {
        // pm1 not available — fallback, device will not auto-wake
        Serial.println("   WARNING: pm1 not ready, cannot set wake timer");
        M5.Power.timerSleep(sleepSecs);
    }
}

// ============================================================
// LOOP — 2 minute awake window for button interaction
// ============================================================
static uint32_t gLastInteraction = 0;

void renderCurrentPage()
{
    switch (gCurrentPage) {
        case 2:  renderPage2(); break;
        case 3:  renderPage3(); break;
        case 4:
            // Prefer SD photo over PSRAM placeholder if photos available
            if (gSdReady && gPhotoCount > 0) {
                int idx = (gSlideIndex < gPhotoCount) ? gSlideIndex : 0;
                renderSlideFromSD(gPhotoList[idx]);
            } else {
                renderPage4();  // placeholder if no SD photos
            }
            break;
        default: renderDashboard(); break;
    }
}

void loop()
{
    M5.update();

    if (gLastInteraction == 0) {
        gLastInteraction = millis();
    }

    uint32_t now = millis();

    // ── Button A — previous page (wraps 4→1) ──
    if (M5.BtnA.wasPressed()) {
        gLastInteraction = now;
        gCurrentPage--;
        if (gCurrentPage < 1) gCurrentPage = 4;
        // Leaving page 4 exits photo mode
        if (gCurrentPage != 4 && gPhotoMode) {
            gPhotoMode = false;
            Serial.println(">> Exiting photo mode via BtnA");
        }
        Serial.printf(">> BtnA — page %d\n", gCurrentPage);
        savePrefs();
        ledConfirm();
        renderCurrentPage();
    }

    // ── Button B — next page (wraps 1→4) ──
    if (M5.BtnB.wasPressed()) {
        gLastInteraction = now;
        gCurrentPage++;
        if (gCurrentPage > 4) gCurrentPage = 1;
        // Leaving page 4 exits photo mode
        if (gCurrentPage != 4 && gPhotoMode) {
            gPhotoMode = false;
            Serial.println(">> Exiting photo mode via BtnB");
        }
        Serial.printf(">> BtnB — page %d\n", gCurrentPage);
        savePrefs();
        ledConfirm();
        renderCurrentPage();
    }

    // ── Button C — single press: enter web mode ──
    if (M5.BtnC.wasPressed()) {
        gLastInteraction = now;
        Serial.println(">> BtnC — entering web mode");
        ledWebMode();
        gWebMode = true;
        if (!gWifiOk) {
            ledWifi();
            connectWiFi();
        }
        if (gWifiOk) {
            startWebServer();
            renderWebMode();
        } else {
            Serial.println("   WiFi not available for web mode");
            gWebMode = false;
            ledRendering();
        }
    }

    // ── LED animation tick ──
    updateLeds();

    // ── Slideshow tick ──
    if (gSlideshowMode && gSdReady && gPhotoCount > 0) {
        if (gLastSlideMs == 0) gLastSlideMs = now;
        if (now - gLastSlideMs >= (uint32_t)gSlideInterval * 1000) {
            gLastSlideMs = now;
            gSlideIndex = (gSlideIndex + 1) % gPhotoCount;
            savePrefs();  // persist so sleep/wake cycle stays in sync
            Serial.printf(">> Slideshow tick: showing %d of %d (%s)\n",
                          gSlideIndex + 1, gPhotoCount, gPhotoList[gSlideIndex]);
            renderSlideFromSD(gPhotoList[gSlideIndex]);
        }
    }

    // ── Web mode loop ──
    if (gWebMode) {
        handleWebLoop();
        return;
    }

    // ── Photo mode without slideshow — stay awake indefinitely ──
    // Slideshow mode uses normal sleep timeout so pm1 timer advances slides
    if (gPhotoMode && !gSlideshowMode) {
        delay(100);
        return;
    }

    // ── Sleep timeout ──
    if (now - gLastInteraction >= AWAKE_TIMEOUT_MS) {
        Serial.println(">> awake timeout");
        goToSleep();
    }

    delay(100);
}

// ============================================================
// INIT DISPLAY
// ============================================================
void initDisplay()
{
    // Mode set ONCE at init — ACeP panel locks mode at startup
    // Test epd_fastest to see if colour quality is acceptable
    M5.Display.setEpdMode(epd_mode_t::epd_quality);
    M5.Display.setRotation(0);
    // Use display dimensions directly — matches official examples
    int dw = M5.Display.width();
    int dh = M5.Display.height();
    Serial.printf("   Display: %d x %d\n", dw, dh);
    canvas.createSprite(dw, dh);
    canvas.setTextSize(1);
    Serial.printf("   Sprite created, PSRAM free: %d\n",
                  heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

// ============================================================
// READ RTC
// RX8130CE via M5Unified — field names from official example:
// dt.date.year, dt.date.month, dt.date.date, dt.date.weekDay
// dt.time.hours, dt.time.minutes  weekDay: 0=Sun
// ============================================================
void readRTC()
{
    if (!M5.Rtc.isEnabled()) {
        Serial.println("   RTC: not enabled");
        return;
    }

    auto dt = M5.Rtc.getDateTime();

    Serial.printf("   RTC: %04d/%02d/%02d (%s) %02d:%02d\n",
                  dt.date.year, dt.date.month, dt.date.date,
                  RTC_WD[dt.date.weekDay],
                  dt.time.hours, dt.time.minutes);

    gRtc.day   = dt.date.date;
    gRtc.month = dt.date.month;
    gRtc.year  = dt.date.year;
    gRtc.hour  = dt.time.hours;
    gRtc.min   = dt.time.minutes;

    int dow = dt.date.weekDay;  // 0=Sun
    if (dow >= 0 && dow <= 6) {
        strncpy(gRtc.dayname, fullDayName(dow), sizeof(gRtc.dayname) - 1);
    }
}

// ============================================================
// READ SHT40
// Uses M5UnitENV SHT4X class — pins 3(SDA), 2(SCL)
// ============================================================
void readSensor()
{
    static bool sht_ok = false;
    static bool sht_init = false;

    if (!sht_init) {
        sht_init = true;
        // M5Unified uses Wire1 for internal I2C on PaperColor
        // Using Wire conflicts with M5PM1/RTC on the same bus
        i2c_port_t portIn = M5.In_I2C.getPort();
        TwoWire* wireIn = (portIn == 1) ? &Wire1 : &Wire;
        Serial.printf("   SHT40 using Wire%d\n", (int)portIn);
        sht_ok = sht4.begin(wireIn, SHT40_I2C_ADDR_44,
                             SHT_SDA_PIN, SHT_SCL_PIN, 400000U);
        if (!sht_ok) {
            Serial.println("   SHT40: begin() failed");
            return;
        }
        Serial.println("   SHT40: init OK");
    }

    if (!sht_ok) return;

    if (sht4.update()) {
        gSensor.temp_c   = sht4.cTemp;
        gSensor.humidity = sht4.humidity;
        Serial.printf("   SHT40: %.1f C  %.0f%%\n",
                      gSensor.temp_c, gSensor.humidity);
    } else {
        Serial.println("   SHT40: update() failed");
    }
}


// ============================================================
// NTP TIME SYNC — sets RTC if year < 2020
// Sydney timezone: AEST UTC+10, AEDT UTC+11 (handled by posix string)
// ============================================================
void syncNTP()
{
    if (!gWifiOk) return;

    Serial.println(">> NTP sync");

    // Step 1: force SNTP sync and wait for COMPLETED status
    // Must check sntp_get_sync_status() — getLocalTime() returns stale
    // RTC-seeded system clock even before NTP completes
    configTime(0, 0, "pool.ntp.org", "time.google.com", "time.cloudflare.com");

    struct tm timeinfo;
    int attempts = 0;
    sntp_sync_status_t syncStatus = SNTP_SYNC_STATUS_RESET;
    while (attempts < 60) {
        delay(500);
        syncStatus = sntp_get_sync_status();
        Serial.printf("   NTP wait %d status=%d\n", attempts, (int)syncStatus);
        if (syncStatus == SNTP_SYNC_STATUS_COMPLETED) break;
        attempts++;
    }

    if (syncStatus != SNTP_SYNC_STATUS_COMPLETED) {
        Serial.println("   NTP: FAILED — keeping RTC time");
        return;
    }

    getLocalTime(&timeinfo);

    Serial.printf("   NTP UTC: %04d/%02d/%02d %02d:%02d:%02d\n",
                  timeinfo.tm_year + 1900, timeinfo.tm_mon + 1,
                  timeinfo.tm_mday, timeinfo.tm_hour,
                  timeinfo.tm_min, timeinfo.tm_sec);

    // Step 2: apply Sydney timezone — AEST UTC+10, AEDT UTC+11
    // DST: starts first Sun Oct, ends first Sun Apr
    setenv("TZ", "AEST-10AEDT,M10.1.0,M4.1.0/3", 1);
    tzset();
    getLocalTime(&timeinfo);

    Serial.printf("   NTP AEST: %04d/%02d/%02d %02d:%02d:%02d\n",
                  timeinfo.tm_year + 1900, timeinfo.tm_mon + 1,
                  timeinfo.tm_mday, timeinfo.tm_hour,
                  timeinfo.tm_min, timeinfo.tm_sec);

    // Step 3: set RTC
    m5::rtc_datetime_t dt;
    dt.date.year    = timeinfo.tm_year + 1900;
    dt.date.month   = timeinfo.tm_mon + 1;
    dt.date.date    = timeinfo.tm_mday;
    dt.date.weekDay = timeinfo.tm_wday;  // 0=Sun
    dt.time.hours   = timeinfo.tm_hour;
    dt.time.minutes = timeinfo.tm_min;
    dt.time.seconds = timeinfo.tm_sec;
    M5.Rtc.setDateTime(dt);
    Serial.println("   RTC updated from NTP");

    // Re-read RTC into gRtc
    readRTC();
}

// ============================================================
// WIFI
// ============================================================
void connectWiFi()
{
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);  // defined in config.h
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 30) {
        delay(500);
        attempts++;
    }
    gWifiOk = (WiFi.status() == WL_CONNECTED);
}

// ============================================================
// FETCH WEATHER
// Key fix: use getStream() not getStreamPtr() — avoids chunked
// encoding issue that gives empty parse results
// ============================================================
bool fetchWeather()
{
    char url[256];
    snprintf(url, sizeof(url), WEATHER_URL, WEATHER_API_KEY);  // WEATHER_API_KEY from config.h

    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    http.begin(client, url);
    http.setTimeout(10000);

    int code = http.GET();
    Serial.printf("   HTTP response code: %d\n", code);
    if (code != 200) {
        http.end();
        return false;
    }

    // ArduinoJson v6 filter — array elements must use [0] only
    // ArduinoJson applies [0] filter to ALL array elements
    StaticJsonDocument<768> filter;
    filter["current"]["temp_c"]            = true;
    filter["current"]["feelslike_c"]       = true;
    filter["current"]["humidity"]          = true;
    filter["current"]["uv"]               = true;
    filter["current"]["condition"]["code"] = true;
    filter["current"]["condition"]["text"] = true;
    filter["current"]["air_quality"]["us-epa-index"] = true;
    filter["current"]["wind_kph"]          = true;
    filter["current"]["wind_degree"]       = true;
    filter["current"]["wind_dir"]          = true;
    filter["current"]["pressure_mb"]       = true;
    filter["current"]["vis_km"]            = true;
    filter["forecast"]["forecastday"][0]["date"]                        = true;
    filter["forecast"]["forecastday"][0]["day"]["maxtemp_c"]            = true;
    filter["forecast"]["forecastday"][0]["day"]["daily_chance_of_rain"] = true;
    filter["forecast"]["forecastday"][0]["day"]["condition"]["code"]    = true;
    filter["forecast"]["forecastday"][0]["astro"]["sunrise"]           = true;
    filter["forecast"]["forecastday"][0]["astro"]["sunset"]            = true;
    filter["forecast"]["forecastday"][0]["astro"]["moonrise"]          = true;
    filter["forecast"]["forecastday"][0]["astro"]["moon_phase"]        = true;
    filter["forecast"]["forecastday"][0]["astro"]["moon_illumination"] = true;
    filter["forecast"]["forecastday"][0]["astro"]["moonset"]           = true;
    // Hourly — [0] filter applies to ALL 24 hour entries
    filter["forecast"]["forecastday"][0]["hour"][0]["time"]              = true;
    filter["forecast"]["forecastday"][0]["hour"][0]["temp_c"]            = true;
    filter["forecast"]["forecastday"][0]["hour"][0]["chance_of_rain"]    = true;

    // Fetch full response into PSRAM buffer — avoids stream/chunked issues
    int length = http.getSize();
    Serial.printf("   Content-Length: %d\n", length);

    String payload = http.getString();
    http.end();
    Serial.printf("   Payload length: %d\n", payload.length());

    if (payload.length() < 100) {
        Serial.println("   ERROR: payload too short");
        return false;
    }

    DynamicJsonDocument doc(32768);
    DeserializationError err = deserializeJson(doc, payload,
                                DeserializationOption::Filter(filter));
    Serial.printf("   JSON parse: %s\n", err.c_str());
    Serial.printf("   JSON mem used: %d\n", doc.memoryUsage());

    if (err) return false;

    // Debug — print raw values before assignment
    Serial.printf("   raw temp_c: %.1f\n", doc["current"]["temp_c"].as<float>());
    Serial.printf("   raw humidity: %d\n", doc["current"]["humidity"].as<int>());
    Serial.printf("   raw condition: %s\n", doc["current"]["condition"]["text"].as<const char*>());

    gWeather.temp_c      = doc["current"]["temp_c"]      | 0.0f;
    gWeather.feelslike_c = doc["current"]["feelslike_c"] | 0.0f;
    gWeather.humidity    = doc["current"]["humidity"]    | 0;
    // UV can come as int or float from WeatherAPI
    if (doc["current"]["uv"].is<float>()) {
        gWeather.uv = (int)round(doc["current"]["uv"].as<float>());
    } else {
        gWeather.uv = doc["current"]["uv"] | 0;
    }
    Serial.printf("   UV raw value: %d\n", gWeather.uv);
    strncpy(gWeather.condition,
            doc["current"]["condition"]["text"] | "Unknown",
            sizeof(gWeather.condition) - 1);
    gWeather.aqi_index = doc["current"]["air_quality"]["us-epa-index"] | 1;

    // Wind, pressure, visibility
    gWeather.wind_kph    = doc["current"]["wind_kph"]    | 0.0f;
    gWeather.wind_degree = doc["current"]["wind_degree"] | 0;
    gWeather.pressure_mb = doc["current"]["pressure_mb"] | 0.0f;
    gWeather.vis_km      = doc["current"]["vis_km"]      | 0.0f;
    strncpy(gWeather.wind_dir,
            doc["current"]["wind_dir"] | "N",
            sizeof(gWeather.wind_dir) - 1);
    Serial.printf("   Wind: %.0f km/h %s (%d deg)  P: %.0f hPa  Vis: %.0f km\n",
                  gWeather.wind_kph, gWeather.wind_dir,
                  gWeather.wind_degree, gWeather.pressure_mb, gWeather.vis_km);

    // Hourly data — hours 6..22 from forecastday[0].hour[]
    // WeatherAPI returns 24 entries (index 0=midnight .. 23=11pm)
    // We want indices 6-22 mapped to hourly_temp/rain[0..16]
    JsonArray hours = doc["forecast"]["forecastday"][0]["hour"];
    for (int i = 0; i < 17; i++) {
        int hIdx = i + 6;  // 6am=0 .. 10pm=16
        if (hIdx < (int)hours.size()) {
            gWeather.hourly_temp[i] = hours[hIdx]["temp_c"] | 0.0f;
            gWeather.hourly_rain[i] = hours[hIdx]["chance_of_rain"] | 0;
        }
    }
    Serial.printf("   Hourly temps 6am-10pm:");
    for (int i = 0; i < 17; i++) Serial.printf(" %.0f", gWeather.hourly_temp[i]);
    Serial.println();

    gWeather.rain_chance = doc["forecast"]["forecastday"][0]["day"]["daily_chance_of_rain"] | 0;

    // Astronomy data from forecastday[0].astro
    JsonObject astro = doc["forecast"]["forecastday"][0]["astro"];
    strncpy(gWeather.sunrise,   astro["sunrise"]   | "N/A", sizeof(gWeather.sunrise) - 1);
    strncpy(gWeather.sunset,    astro["sunset"]    | "N/A", sizeof(gWeather.sunset) - 1);
    strncpy(gWeather.moonrise,  astro["moonrise"]  | "N/A", sizeof(gWeather.moonrise) - 1);
    strncpy(gWeather.moonset,   astro["moonset"]   | "N/A", sizeof(gWeather.moonset) - 1);
    strncpy(gWeather.moon_phase, astro["moon_phase"] | "Unknown", sizeof(gWeather.moon_phase) - 1);
    gWeather.moon_illumination = astro["moon_illumination"] | 0;
    Serial.printf("   Astro: rise=%s set=%s moon=%s %d%% moonrise=%s moonset=%s\n",
                  gWeather.sunrise, gWeather.sunset,
                  gWeather.moon_phase, gWeather.moon_illumination,
                  gWeather.moonrise, gWeather.moonset);

    // Parse forecast days
    static const char* zdays[] = {"Mon","Tue","Wed","Thu","Fri","Sat","Sun"};
    for (int i = 0; i < 3; i++) {
        JsonObject fd = doc["forecast"]["forecastday"][i];
        const char* dateStr = fd["date"] | "2026-01-01";
        int yr = 0, mo = 0, dy = 0;
        sscanf(dateStr, "%d-%d-%d", &yr, &mo, &dy);
        // Zeller's congruence
        int zm = mo, zy = yr;
        if (zm < 3) { zm += 12; zy--; }
        int dow = (dy + (13*(zm+1)/5) + zy + zy/4 - zy/100 + zy/400) % 7;
        int adjusted = ((dow - 2) % 7 + 7) % 7;  // 0=Mon..6=Sun
        strncpy(gWeather.fc_day[i], zdays[adjusted], 7);

        gWeather.fc_max[i]  = (int)round(fd["day"]["maxtemp_c"].as<float>());
        gWeather.fc_rain[i] = fd["day"]["daily_chance_of_rain"] | 0;
        gWeather.fc_code[i] = fd["day"]["condition"]["code"]    | 1000;
    }

    return true;
}

// ============================================================
// MASTER RENDER
// ============================================================
void renderDashboard()
{
    canvas.fillSprite(0xFFFF);
    drawZone1_StatusBar();
    drawZone2_Date();
    drawZone3_Indoor();
    drawZone4_Weather();
    drawZone5_Forecast();
    drawZone6_Calm();
    // Watchdog already disabled — safe to push
    canvas.pushSprite(0, 0);
}

// ============================================================
// ZONE 1 — STATUS BAR
// ============================================================
void drawZone1_StatusBar()
{
    canvas.fillRect(0, PC_Z1_Y, PC_W, PC_Z1_H, 0xFFFF);
    canvas.setFont(&fonts::Font2);
    canvas.setTextSize(1);

    canvas.setTextColor(gWifiOk ? PC_BLACK : PC_RED);
    canvas.setTextDatum(middle_left);
    canvas.drawString(gWifiOk ? "WiFi OK" : "No WiFi", 8, PC_Z1_Y + PC_Z1_H / 2);

    char timebuf[20];
    snprintf(timebuf, sizeof(timebuf), "Updated %02d:%02d", gRtc.hour, gRtc.min);
    canvas.setTextColor(PC_BLACK);
    canvas.setTextDatum(middle_center);
    canvas.drawString(timebuf, PC_W / 2, PC_Z1_Y + PC_Z1_H / 2);

    gBattPct = M5.Power.getBatteryLevel();
    char batbuf[8];
    snprintf(batbuf, sizeof(batbuf), "%d%%", gBattPct);
    canvas.setTextDatum(middle_right);
    canvas.drawString(batbuf, PC_W - 8, PC_Z1_Y + PC_Z1_H / 2);

    canvas.drawLine(0, PC_Z1_Y + PC_Z1_H - 1, PC_W, PC_Z1_Y + PC_Z1_H - 1, PC_BLACK);
    canvas.drawLine(0, PC_Z1_Y + PC_Z1_H - 2, PC_W, PC_Z1_Y + PC_Z1_H - 2, PC_BLACK);
}

// ============================================================
// ZONE 2 — DATE BLOCK
// ============================================================
void drawZone2_Date()
{
    canvas.fillRect(0, PC_Z2_Y, PC_W, PC_Z2_H, 0xFFFF);

    canvas.setFont(&fonts::FreeSansBold24pt7b);
    canvas.setTextColor(PC_BLACK);
    canvas.setTextDatum(top_left);
    canvas.drawString(gRtc.dayname, 14, PC_Z2_Y + 6);

    char datebuf[24];
    snprintf(datebuf, sizeof(datebuf), "%d %s %d",
             gRtc.day, monthName(gRtc.month), gRtc.year);
    canvas.setFont(&fonts::Font4);
    canvas.setTextColor(PC_BLACK);
    canvas.drawString(datebuf, 14, PC_Z2_Y + 52);

    canvas.drawLine(0, PC_Z2_Y + PC_Z2_H - 1, PC_W, PC_Z2_Y + PC_Z2_H - 1, PC_BLACK);
    canvas.drawLine(0, PC_Z2_Y + PC_Z2_H - 2, PC_W, PC_Z2_Y + PC_Z2_H - 2, PC_BLACK);
}

// ============================================================
// ZONE 3 — INDOOR SENSOR ROW
// ============================================================
void drawZone3_Indoor()
{
    canvas.fillRect(0, PC_Z3_Y, PC_W, PC_Z3_H, 0xFFFF);

    int col1X = PC_W / 4;
    int col2X = 3 * PC_W / 4;

    canvas.setFont(&fonts::Font4);
    canvas.setTextColor(PC_BLACK);
    canvas.setTextDatum(top_center);
    canvas.drawString("Indoor", col1X, PC_Z3_Y + 4);
    canvas.drawString("Humidity", col2X, PC_Z3_Y + 4);

    canvas.setFont(&fonts::FreeSansBold18pt7b);
    canvas.setTextColor(PC_BLACK);
    canvas.setTextDatum(middle_center);

    char buf[16];
    int midY = PC_Z3_Y + PC_Z3_H / 2 + 8;  // shifted down
    snprintf(buf, sizeof(buf), "%.1f C", gSensor.temp_c);
    canvas.drawString(buf, col1X, midY);

    snprintf(buf, sizeof(buf), "%.0f%%", gSensor.humidity);
    canvas.drawString(buf, col2X, midY);

    canvas.drawLine(PC_W / 2, PC_Z3_Y + 4, PC_W / 2, PC_Z3_Y + PC_Z3_H - 4, PC_BLACK);
    canvas.drawLine(0, PC_Z3_Y + PC_Z3_H - 1, PC_W, PC_Z3_Y + PC_Z3_H - 1, PC_BLACK);
    canvas.drawLine(0, PC_Z3_Y + PC_Z3_H - 2, PC_W, PC_Z3_Y + PC_Z3_H - 2, PC_BLACK);
}

// ============================================================
// ZONE 4 — SYDNEY WEATHER
// ============================================================
void drawZone4_Weather()
{
    canvas.fillRect(0, PC_Z4_Y, PC_W, PC_Z4_H, 0xFFFF);

    canvas.setFont(&fonts::Font4);
    canvas.setTextColor(PC_BLACK);
    canvas.setTextDatum(top_left);
    canvas.drawString("Sydney", 14, PC_Z4_Y + 4);

    int leftW  = 130;
    int iconCX = leftW / 2;
    int iconCY = PC_Z4_Y + 56;

    drawWeatherIcon(&canvas, iconCX, iconCY, gWeather.fc_code[0], 44, 36);

    canvas.setFont(&fonts::FreeSansBold24pt7b);
    canvas.setTextColor(PC_BLACK);
    canvas.setTextDatum(top_left);
    char tempbuf[8];
    snprintf(tempbuf, sizeof(tempbuf), "%dC", (int)round(gWeather.temp_c));
    canvas.drawString(tempbuf, 10, PC_Z4_Y + 82);

    canvas.setFont(&fonts::Font2);
    canvas.setTextColor(PC_BLACK);
    canvas.drawString(gWeather.condition, 10, PC_Z4_Y + 148);

    canvas.drawLine(leftW, PC_Z4_Y + 22, leftW, PC_Z4_Y + PC_Z4_H - 1, PC_BLACK);

    int gridX = leftW + 1;
    int gridW = PC_W - gridX;
    int halfW = gridW / 2;
    int halfH = (PC_Z4_H - 20) / 2;
    int gridY = PC_Z4_Y + 20;

    canvas.drawLine(gridX + halfW, gridY, gridX + halfW, gridY + halfH * 2, PC_GREY);
    canvas.drawLine(gridX, gridY + halfH, PC_W, gridY + halfH, PC_GREY);

    char buf[12];

    // TL: Humidity
    {
        int cx = gridX + halfW / 2;
        canvas.setFont(&fonts::Font2);
        canvas.setTextColor(PC_BLACK);
        canvas.setTextDatum(top_center);
        canvas.drawString("HUMIDITY", cx, gridY + 4);
        canvas.setFont(&fonts::FreeSansBold12pt7b);
        canvas.setTextColor(PC_BLACK);
        canvas.setTextDatum(middle_center);
        snprintf(buf, sizeof(buf), "%d%%", gWeather.humidity);
        canvas.drawString(buf, cx, gridY + halfH / 2 + 10);
    }
    // TR: Rain
    {
        int cx = gridX + halfW + halfW / 2;
        canvas.setFont(&fonts::Font2);
        canvas.setTextColor(PC_BLACK);
        canvas.setTextDatum(top_center);
        canvas.drawString("RAIN", cx, gridY + 4);
        canvas.setFont(&fonts::FreeSansBold12pt7b);
        canvas.setTextColor(gWeather.rain_chance >= 60 ? PC_RED : PC_BLUE);
        canvas.setTextDatum(middle_center);
        snprintf(buf, sizeof(buf), "%d%%", gWeather.rain_chance);
        canvas.drawString(buf, cx, gridY + halfH / 2 + 10);
    }
    // BL: UV
    {
        int cx = gridX + halfW / 2;
        int by = gridY + halfH;
        canvas.setFont(&fonts::Font2);
        canvas.setTextColor(PC_BLACK);
        canvas.setTextDatum(top_center);
        canvas.drawString("UV INDEX", cx, by + 4);
        canvas.setFont(&fonts::FreeSansBold12pt7b);
        canvas.setTextColor(uvColour(gWeather.uv));
        canvas.setTextDatum(middle_center);
        snprintf(buf, sizeof(buf), "%d", gWeather.uv);
        canvas.drawString(buf, cx, by + halfH / 2 + 4);
        canvas.setFont(&fonts::Font2);
        canvas.setTextColor(uvColour(gWeather.uv));
        canvas.drawString(uvLabel(gWeather.uv), cx, by + halfH / 2 + 22);
    }
    // BR: Feels Like
    {
        int cx = gridX + halfW + halfW / 2;
        int by = gridY + halfH;
        canvas.setFont(&fonts::Font2);
        canvas.setTextColor(PC_BLACK);
        canvas.setTextDatum(top_center);
        canvas.drawString("FEELS LIKE", cx, by + 4);
        canvas.setFont(&fonts::FreeSansBold12pt7b);
        canvas.setTextColor(PC_BLACK);
        canvas.setTextDatum(middle_center);
        snprintf(buf, sizeof(buf), "%dC", (int)round(gWeather.feelslike_c));
        canvas.drawString(buf, cx, by + halfH / 2 + 10);
    }

    canvas.drawLine(0, PC_Z4_Y + PC_Z4_H - 1, PC_W, PC_Z4_Y + PC_Z4_H - 1, PC_BLACK);
    canvas.drawLine(0, PC_Z4_Y + PC_Z4_H - 2, PC_W, PC_Z4_Y + PC_Z4_H - 2, PC_BLACK);
}

// ============================================================
// ZONE 5 — 4-DAY FORECAST STRIP
// ============================================================
void drawZone5_Forecast()
{
    canvas.fillRect(0, PC_Z5_Y, PC_W, PC_Z5_H, 0xFFFF);

    int colW   = PC_W / 3;
    // Fixed Y positions for each row — clean and predictable
    int rowDay  = PC_Z5_Y + 8;   // day name
    int rowIcon = PC_Z5_Y + 50;  // icon centre
    int rowTemp = PC_Z5_Y + 72;  // temperature
    int rowRain = PC_Z5_Y + 108; // rain %

    for (int i = 0; i < 3; i++) {
        int colX = i * colW;
        int cx   = colX + colW / 2;

        if (i > 0) {
            canvas.drawLine(colX, PC_Z5_Y + 2, colX, PC_Z5_Y + PC_Z5_H - 2, PC_GREY);
        }

        // Day name
        canvas.setFont(&fonts::FreeSansBold12pt7b);
        canvas.setTextColor(PC_BLACK);
        canvas.setTextDatum(top_center);
        canvas.drawString(gWeather.fc_day[i], cx, rowDay);

        // Weather icon
        drawWeatherIcon(&canvas, cx, rowIcon, gWeather.fc_code[i], 36, 28);

        // Max temp
        canvas.setFont(&fonts::FreeSansBold18pt7b);
        canvas.setTextColor(PC_BLACK);
        canvas.setTextDatum(top_center);
        char buf[8];
        snprintf(buf, sizeof(buf), "%dC", gWeather.fc_max[i]);
        canvas.drawString(buf, cx, rowTemp);

        // Rain chance
        canvas.setFont(&fonts::FreeSansBold12pt7b);
        snprintf(buf, sizeof(buf), "%d%%", gWeather.fc_rain[i]);
        canvas.setTextColor(gWeather.fc_rain[i] >= 60 ? PC_RED :
                           gWeather.fc_rain[i] >= 40 ? PC_BLUE : (uint16_t)PC_BLACK);
        canvas.drawString(buf, cx, rowRain);
    }

    canvas.drawLine(0, PC_Z5_Y + PC_Z5_H - 1, PC_W, PC_Z5_Y + PC_Z5_H - 1, PC_BLACK);
    canvas.drawLine(0, PC_Z5_Y + PC_Z5_H - 2, PC_W, PC_Z5_Y + PC_Z5_H - 2, PC_BLACK);
}

// ============================================================
// ZONE 6 — CALM STRIP (placeholder until AstronomyAPI Phase 4)
// ============================================================
void drawZone6_Calm()
{
    canvas.fillRect(0, PC_Z6_Y, PC_W, PC_Z6_H, PC_LGREY);

    int thirdW = PC_W / 3;
    int midY   = PC_Z6_Y + PC_Z6_H / 2;

    // Vertical dividers
    canvas.drawLine(thirdW,     PC_Z6_Y + 4, thirdW,     PC_Z6_Y + PC_Z6_H - 4, PC_GREY);
    canvas.drawLine(thirdW * 2, PC_Z6_Y + 4, thirdW * 2, PC_Z6_Y + PC_Z6_H - 4, PC_GREY);

    // ── Left: Moon phase icon + name beside icon ──
    {
        int zoneLeft = 0;
        int zoneW    = thirdW;
        int iconR    = 22;
        int iconCX   = zoneLeft + iconR + 18;  // pushed right, clear of edge
        int iconCY   = PC_Z6_Y + PC_Z6_H / 2;
        float illum  = gWeather.moon_illumination / 100.0f;
        bool waxing  = (strstr(gWeather.moon_phase, "Waxing") != nullptr ||
                        strstr(gWeather.moon_phase, "New")    != nullptr);
        drawMoonIcon(&canvas, iconCX, iconCY, iconR, illum, waxing);

        // Labels to the right of icon
        int labelX = iconCX + iconR + 6;
        canvas.setFont(&fonts::Font2);
        canvas.setTextColor(PC_BLACK);
        canvas.setTextDatum(top_left);
        const char* phase = gWeather.moon_phase;
        const char* sp    = strchr(phase, ' ');
        if (sp) {
            char line1[16] = {0};
            strncpy(line1, phase, sp - phase);
            canvas.drawString(line1,  labelX, PC_Z6_Y + 28);
            canvas.drawString(sp + 1, labelX, PC_Z6_Y + 42);
        } else {
            canvas.drawString(phase, labelX, PC_Z6_Y + 34);
        }
        char illumbuf[8];
        snprintf(illumbuf, sizeof(illumbuf), "%d%%", gWeather.moon_illumination);
        canvas.drawString(illumbuf, labelX, PC_Z6_Y + 58);
    }

    // ── Centre: Sunrise / Sunset ──
    {
        int cx = thirdW + thirdW / 2;
        canvas.setFont(&fonts::Font2);
        canvas.setTextColor(PC_BLACK);
        canvas.setTextDatum(top_center);
        canvas.drawString("SUNRISE", cx, PC_Z6_Y + 16);

        canvas.setFont(&fonts::FreeSansBold12pt7b);
        canvas.setTextColor(PC_AMBER);
        canvas.setTextDatum(top_center);
        canvas.drawString(moonTimeDisplay(gWeather.sunrise), cx, PC_Z6_Y + 30);

        canvas.setFont(&fonts::Font2);
        canvas.setTextColor(PC_BLACK);
        canvas.drawString("SUNSET", cx, PC_Z6_Y + 58);

        canvas.setFont(&fonts::FreeSansBold12pt7b);
        canvas.setTextColor(PC_BLUE);
        canvas.drawString(moonTimeDisplay(gWeather.sunset), cx, PC_Z6_Y + 72);
    }

    // ── Right: Moonrise + Moonset ──
    {
        int cx = thirdW * 2 + thirdW / 2;
        canvas.setFont(&fonts::Font2);
        canvas.setTextColor(PC_BLACK);
        canvas.setTextDatum(top_center);
        canvas.drawString("MOONRISE", cx, PC_Z6_Y + 16);

        canvas.setFont(&fonts::FreeSansBold12pt7b);
        canvas.setTextColor(PC_BLUE);
        canvas.drawString(moonTimeDisplay(gWeather.moonrise), cx, PC_Z6_Y + 30);

        canvas.setFont(&fonts::Font2);
        canvas.setTextColor(PC_BLACK);
        canvas.drawString("MOONSET", cx, PC_Z6_Y + 58);

        canvas.setFont(&fonts::FreeSansBold12pt7b);
        canvas.setTextColor(PC_BLUE);
        canvas.drawString(moonTimeDisplay(gWeather.moonset), cx, PC_Z6_Y + 72);
    }
}


// ============================================================
// ============================================================
// SHARED STATUS BAR HELPER  (pages 2 and 3)
// ============================================================
void drawStatusBar(int pageNum)
{
    canvas.fillRect(0, 0, PC_W, PC_Z1_H, 0xFFFF);
    canvas.setFont(&fonts::Font2);
    canvas.setTextSize(1);
    canvas.setTextColor(gWifiOk ? PC_BLACK : PC_RED);
    char pagebuf[10];
    snprintf(pagebuf, sizeof(pagebuf), "Pg %d/3", pageNum);
    canvas.setTextDatum(middle_left);
    canvas.drawString(pagebuf, 8, PC_Z1_H / 2);
    char timebuf[20];
    snprintf(timebuf, sizeof(timebuf), "Updated %02d:%02d", gRtc.hour, gRtc.min);
    canvas.setTextColor(PC_BLACK);
    canvas.setTextDatum(middle_center);
    canvas.drawString(timebuf, PC_W / 2, PC_Z1_H / 2);
    int batt = M5.Power.getBatteryLevel();
    char batbuf[8];
    snprintf(batbuf, sizeof(batbuf), "%d%%", batt);
    canvas.setTextDatum(middle_right);
    canvas.drawString(batbuf, PC_W - 8, PC_Z1_H / 2);
    canvas.drawLine(0, PC_Z1_H - 1, PC_W, PC_Z1_H - 1, PC_BLACK);
    canvas.drawLine(0, PC_Z1_H - 2, PC_W, PC_Z1_H - 2, PC_BLACK);
}

// ============================================================
// PAGE 2 — HOURLY GRAPH
// Clean open layout — no internal box borders
// Status bar / Hero temp / Stats strip / Graph / Forecast / Moon+AQI
// Zone maths: 28+76+36+140+256+64 = 600px
// P2_ constants defined earlier — reused here
// ============================================================
void renderPage2()
{
    canvas.fillSprite(0xFFFF);

    // ── Status bar — indoor reading tucked left ──
    canvas.fillRect(0, 0, PC_W, PC_Z1_H, 0xFFFF);
    canvas.setFont(&fonts::Font2);
    canvas.setTextSize(1);
    canvas.setTextColor(gWifiOk ? PC_BLACK : PC_RED);
    {
        char indbuf[24];
        snprintf(indbuf, sizeof(indbuf), "In: %.1fC  %.0f%%",
                 gSensor.temp_c, gSensor.humidity);
        canvas.setTextDatum(middle_left);
        canvas.drawString(indbuf, 8, PC_Z1_H / 2);
    }
    canvas.setTextColor(PC_BLACK);
    {
        char timebuf[20];
        snprintf(timebuf, sizeof(timebuf), "Updated %02d:%02d", gRtc.hour, gRtc.min);
        canvas.setTextDatum(middle_center);
        canvas.drawString(timebuf, PC_W / 2, PC_Z1_H / 2);
    }
    {
        char batbuf[8];
        snprintf(batbuf, sizeof(batbuf), "%d%%", M5.Power.getBatteryLevel());
        canvas.setTextDatum(middle_right);
        canvas.drawString(batbuf, PC_W - 8, PC_Z1_H / 2);
    }
    canvas.drawLine(0, PC_Z1_H - 1, PC_W, PC_Z1_H - 1, PC_BLACK);
    canvas.drawLine(0, PC_Z1_H - 2, PC_W, PC_Z1_H - 2, PC_BLACK);

    // ── Hero: big temp + icon + condition ──
    // Y=28  H=88 — extra height to avoid crowding
    {
        int y = PC_Z1_H;  // 28
        canvas.setFont(&fonts::Font2);
        canvas.setTextColor(PC_BLACK);
        canvas.setTextDatum(top_left);
        {
            char loc[32];
            snprintf(loc, sizeof(loc), "Sydney  %s %d %s",
                     gRtc.dayname, gRtc.day, monthName(gRtc.month));
            canvas.drawString(loc, 14, y + 4);
        }
        canvas.setFont(&fonts::FreeSansBold24pt7b);
        canvas.setTextColor(PC_BLACK);
        canvas.setTextDatum(top_left);
        char tempbuf[8];
        snprintf(tempbuf, sizeof(tempbuf), "%dC", (int)round(gWeather.temp_c));
        canvas.drawString(tempbuf, 14, y + 18);

        // Condition below temp — Font4 for readability
        canvas.setFont(&fonts::Font4);
        canvas.setTextColor(PC_BLACK);
        canvas.setTextDatum(top_left);
        canvas.drawString(gWeather.condition, 14, y + 62);

        // Feels like — same line as condition, right-aligned
        canvas.setFont(&fonts::Font2);
        char feelsbuf[16];
        snprintf(feelsbuf, sizeof(feelsbuf), "feels %dC", (int)round(gWeather.feelslike_c));
        canvas.setTextDatum(top_right);
        canvas.drawString(feelsbuf, PC_W - 14, y + 66);

        // Icon — top right, clear of temp text
        int iconCX = PC_W - 52;
        int iconCY = y + 36;
        drawWeatherIcon(&canvas, iconCX, iconCY, gWeather.fc_code[0], 56, 44);
    }

    // ── Stats strip: Wind / Pressure / Visibility ──
    // Y=116  H=56 — 3 rows: label(Font2~14px) + value(FreeSansBold12pt~22px) + unit(Font2~14px)
    // With 4px gaps: 4+14+4+22+4+14 = 62 — use 60px and tighten gaps
    {
        int y      = 116;
        int colW   = PC_W / 3;
        int labelY = y + 2;    // Font2 label — top
        int valueY = y + 18;   // FreeSansBold12pt value — top (renders ~22px tall)
        int unitY  = y + 44;   // Font2 unit — top, 4px below value bottom

        // Wind
        {
            int cx = colW / 2;
            canvas.setFont(&fonts::Font2);
            canvas.setTextColor(PC_BLACK);
            canvas.setTextDatum(top_center);
            canvas.drawString("WIND", cx, labelY);
            // Compass arrow — small, left of value text
            drawCompassArrow(&canvas, cx - 44, valueY + 11, 9, gWeather.wind_degree);
            canvas.setFont(&fonts::FreeSansBold12pt7b);
            canvas.setTextDatum(top_center);
            char wbuf[10];
            snprintf(wbuf, sizeof(wbuf), "%.0f km/h", gWeather.wind_kph);
            canvas.drawString(wbuf, cx + 10, valueY);
            canvas.setFont(&fonts::Font2);
            canvas.setTextDatum(top_center);
            canvas.drawString(gWeather.wind_dir, cx, unitY);
        }

        // Pressure
        {
            int cx = colW + colW / 2;
            canvas.setFont(&fonts::Font2);
            canvas.setTextColor(PC_BLACK);
            canvas.setTextDatum(top_center);
            canvas.drawString("PRESSURE", cx, labelY);
            canvas.setFont(&fonts::FreeSansBold12pt7b);
            canvas.setTextDatum(top_center);
            char pbuf[10];
            snprintf(pbuf, sizeof(pbuf), "%.0f", gWeather.pressure_mb);
            canvas.drawString(pbuf, cx, valueY);
            canvas.setFont(&fonts::Font2);
            canvas.setTextDatum(top_center);
            canvas.drawString("hPa", cx, unitY);
        }

        // Visibility
        {
            int cx = colW * 2 + colW / 2;
            canvas.setFont(&fonts::Font2);
            canvas.setTextColor(PC_BLACK);
            canvas.setTextDatum(top_center);
            canvas.drawString("VISIBILITY", cx, labelY);
            canvas.setFont(&fonts::FreeSansBold12pt7b);
            canvas.setTextDatum(top_center);
            char vbuf[8];
            if (gWeather.vis_km >= 10.0f)
                snprintf(vbuf, sizeof(vbuf), ">10");
            else
                snprintf(vbuf, sizeof(vbuf), "%.0f", gWeather.vis_km);
            canvas.drawString(vbuf, cx, valueY);
            canvas.setFont(&fonts::Font2);
            canvas.setTextDatum(top_center);
            canvas.drawString("km", cx, unitY);
        }
    }

    // ── Hourly graph — Y=140  H=200 ──
    // Plot hourly_temp[0..16] = 06:00..22:00
    // X: graph area left=40, right=390, width=350
    // Y: graph area top=150, bottom=310, height=160
    // Temp range: find min/max from hourly data, add 2° padding
    {
        int gLeft  = 42;
        int gRight = PC_W - 10;
        int gTop   = 192;
        int gBot   = 336;
        int gW     = gRight - gLeft;
        int gH     = gBot   - gTop;

        // Find min/max temp
        float tMin = gWeather.hourly_temp[0];
        float tMax = gWeather.hourly_temp[0];
        for (int i = 1; i < 17; i++) {
            if (gWeather.hourly_temp[i] < tMin) tMin = gWeather.hourly_temp[i];
            if (gWeather.hourly_temp[i] > tMax) tMax = gWeather.hourly_temp[i];
        }
        // Round to nearest 2 for clean labels, add padding
        tMin = floorf((tMin - 2.0f) / 2.0f) * 2.0f;
        tMax = ceilf( (tMax + 2.0f) / 2.0f) * 2.0f;
        float tRange = tMax - tMin;
        if (tRange < 1.0f) tRange = 1.0f;

        // Max rain chance for bar height scaling
        int rainMax = 0;
        for (int i = 0; i < 17; i++)
            if (gWeather.hourly_rain[i] > rainMax) rainMax = gWeather.hourly_rain[i];
        if (rainMax < 10) rainMax = 10;  // minimum scale

        // X step between hourly points
        float xStep = (float)gW / 16.0f;  // 16 gaps for 17 points

        // Y axis labels — 4 evenly spaced
        canvas.setFont(&fonts::Font2);
        canvas.setTextColor(PC_BLACK);
        canvas.setTextDatum(middle_right);
        for (int i = 0; i <= 3; i++) {
            float t   = tMin + (tRange / 3.0f) * i;
            int   py  = gBot - (int)((t - tMin) / tRange * gH);
            char  lbl[6];
            snprintf(lbl, sizeof(lbl), "%d", (int)round(t));
            canvas.drawString(lbl, gLeft - 2, py);
        }

        // Blue rain bars — only draw when meaningfully tall (>= 2px)
        int barMaxH = 44;
        int barW    = (int)(xStep * 0.6f);
        if (barW < 4) barW = 4;
        for (int i = 0; i < 17; i++) {
            if (gWeather.hourly_rain[i] > 0) {
                int bh = (int)((float)gWeather.hourly_rain[i] / rainMax * barMaxH);
                if (bh < 2) bh = 2;  // minimum visible bar
                int px = gLeft + (int)(i * xStep);
                canvas.fillRect(px - barW / 2, gBot - bh, barW, bh, PC_BLUE);
            }
        }

        // Amber temperature curve
        for (int i = 0; i < 16; i++) {
            int x1 = gLeft + (int)(i       * xStep);
            int y1 = gBot  - (int)((gWeather.hourly_temp[i]     - tMin) / tRange * gH);
            int x2 = gLeft + (int)((i + 1) * xStep);
            int y2 = gBot  - (int)((gWeather.hourly_temp[i + 1] - tMin) / tRange * gH);
            // Draw 3px wide line by offsetting ±1
            canvas.drawLine(x1, y1,     x2, y2,     PC_AMBER);
            canvas.drawLine(x1, y1 - 1, x2, y2 - 1, PC_AMBER);
            canvas.drawLine(x1, y1 + 1, x2, y2 + 1, PC_AMBER);
        }

        // Peak dot + label
        int peakIdx = 0;
        for (int i = 1; i < 17; i++)
            if (gWeather.hourly_temp[i] > gWeather.hourly_temp[peakIdx]) peakIdx = i;
        {
            int px = gLeft + (int)(peakIdx * xStep);
            int py = gBot  - (int)((gWeather.hourly_temp[peakIdx] - tMin) / tRange * gH);
            canvas.fillCircle(px, py, 4, PC_AMBER);
            char peakbuf[6];
            snprintf(peakbuf, sizeof(peakbuf), "%dC", (int)round(gWeather.hourly_temp[peakIdx]));
            canvas.setFont(&fonts::Font4);
            canvas.setTextColor(PC_BLACK);
            canvas.setTextDatum(bottom_center);
            canvas.drawString(peakbuf, px, py - 4);
        }

        // X axis hour labels — every 3 hours: 6am 9am 12pm 3pm 6pm 9pm
        canvas.setFont(&fonts::Font2);
        canvas.setTextColor(PC_BLACK);
        canvas.setTextDatum(top_center);
        const char* xLabels[] = {"6am","9am","12pm","3pm","6pm","9pm"};
        int xLabelIdx[]       = {0, 3, 6, 9, 12, 15};
        for (int i = 0; i < 6; i++) {
            int px = gLeft + (int)(xLabelIdx[i] * xStep);
            canvas.drawString(xLabels[i], px, gBot + 4);
        }

        // Rain legend — top right of graph area
        canvas.fillRect(gRight - 68, gTop + 2, 8, 6, PC_BLUE);
        canvas.setFont(&fonts::Font2);
        canvas.setTextColor(PC_BLACK);
        canvas.setTextDatum(middle_left);
        canvas.drawString("rain %", gRight - 58, gTop + 5);
    }

    // ── 3-day forecast — Y=352  H=160 ──
    // Option 1 stacked: day / icon / temp / rain% same size
    {
        int fcY  = 370;
        int fcH  = 142;
        int colW = PC_W / 3;
        int rowDay  = fcY + 8;
        int rowIcon = fcY + 46;
        int rowTemp = fcY + 80;
        int rowRain = fcY + 108;

        for (int i = 0; i < 3; i++) {
            int cx = i * colW + colW / 2;

            canvas.setFont(&fonts::FreeSansBold12pt7b);
            canvas.setTextColor(PC_BLACK);
            canvas.setTextDatum(top_center);
            canvas.drawString(gWeather.fc_day[i], cx, rowDay);

            drawWeatherIcon(&canvas, cx, rowIcon, gWeather.fc_code[i], 44, 34);

            canvas.setFont(&fonts::FreeSansBold18pt7b);
            canvas.setTextColor(PC_BLACK);
            canvas.setTextDatum(top_center);
            char buf[8];
            snprintf(buf, sizeof(buf), "%dC", gWeather.fc_max[i]);
            canvas.drawString(buf, cx, rowTemp);

            snprintf(buf, sizeof(buf), "%d%%", gWeather.fc_rain[i]);
            canvas.setTextColor(gWeather.fc_rain[i] >= 60 ? PC_RED :
                               gWeather.fc_rain[i] >= 40 ? PC_BLUE : (uint16_t)PC_BLACK);
            canvas.drawString(buf, cx, rowRain);
        }
    }

    // ── Moon + AQI — Y=512  H=88 ──
    {
        canvas.fillRect(0, 512, PC_W, 88, PC_LGREY);
        int halfW = PC_W / 2;
        canvas.drawLine(halfW, 516, halfW, 596, PC_BLACK);

        // Moon — icon centred vertically, labels to the right
        {
            int stripY  = 512;
            int stripH  = 88;
            int iconR   = 20;
            int iconCX  = iconR + 18;  // pushed right, clear of edge
            int iconCY  = stripY + stripH / 2;   // vertically centred = 556
            float illum = gWeather.moon_illumination / 100.0f;
            bool waxing = (strstr(gWeather.moon_phase, "Waxing") != nullptr ||
                           strstr(gWeather.moon_phase, "New")    != nullptr);
            drawMoonIcon(&canvas, iconCX, iconCY, iconR, illum, waxing);

            // Labels start to the right of icon, vertically centred as a group
            int labelX  = iconCX + iconR + 8;
            canvas.setFont(&fonts::Font2);
            canvas.setTextColor(PC_BLACK);
            canvas.setTextDatum(top_left);
            const char* phase = gWeather.moon_phase;
            const char* sp    = strchr(phase, ' ');
            if (sp) {
                char line1[16] = {0};
                strncpy(line1, phase, sp - phase);
                canvas.drawString(line1,  labelX, stripY + 24);
                canvas.drawString(sp + 1, labelX, stripY + 38);
                char illumbuf[8];
                snprintf(illumbuf, sizeof(illumbuf), "%d%%", gWeather.moon_illumination);
                canvas.drawString(illumbuf, labelX, stripY + 54);
            } else {
                canvas.drawString(phase, labelX, stripY + 30);
                char illumbuf[8];
                snprintf(illumbuf, sizeof(illumbuf), "%d%%", gWeather.moon_illumination);
                canvas.drawString(illumbuf, labelX, stripY + 46);
            }
        }

        // AQI
        {
            int cx = halfW + halfW / 2;
            canvas.setFont(&fonts::Font4);
            canvas.setTextColor(PC_BLACK);
            canvas.setTextDatum(top_center);
            canvas.drawString("Air Quality", cx, 512 + 6);
            canvas.setFont(&fonts::FreeSansBold24pt7b);
            canvas.setTextColor(aqiColour(gWeather.aqi_index));
            char aqibuf[4];
            snprintf(aqibuf, sizeof(aqibuf), "%d", gWeather.aqi_index);
            canvas.drawString(aqibuf, cx, 512 + 24);
            canvas.setFont(&fonts::Font4);
            canvas.setTextColor(aqiColour(gWeather.aqi_index));
            canvas.drawString(aqiLabel(gWeather.aqi_index), cx, 512 + 60);
        }
    }

    canvas.pushSprite(0, 0);
}


// ============================================================
// PAGE 3 — CALENDAR + WEATHER HEADER
// Zone maths: 28+54+68+46+28 = 224px weather header
//             grid starts Y=224, height=376px, 6 rows of 62px
// ============================================================
static const int P3_SB_H  = 28;
static const int P3_DT_Y  = 28;   static const int P3_DT_H  = 54;   // date — room for large day name
static const int P3_OD_Y  = 82;   static const int P3_OD_H  = 68;   // outdoor+indoor — taller
static const int P3_ST_Y  = 150;  static const int P3_ST_H  = 46;   // stats — label+value+unit
static const int P3_DH_Y  = 196;  static const int P3_DH_H  = 28;   // day headers
static const int P3_GR_Y  = 224;
static const int P3_GR_H  = PC_H - P3_GR_Y;   // 376px
static const int P3_ROW_H = P3_GR_H / 6;       // 62px per row — still very readable

void renderPage3()
{
    canvas.fillSprite(0xFFFF);
    drawStatusBar(3);

    // ── Date strip ──
    canvas.fillRect(0, P3_DT_Y, PC_W, P3_DT_H, 0xFFFF);
    canvas.setFont(&fonts::FreeSansBold24pt7b);
    canvas.setTextColor(PC_BLACK);
    canvas.setTextDatum(top_left);
    canvas.drawString(gRtc.dayname, 14, P3_DT_Y + 2);
    {
        char datebuf[20];
        snprintf(datebuf, sizeof(datebuf), "%d %s %d",
                 gRtc.day, monthName(gRtc.month), gRtc.year);
        canvas.setFont(&fonts::Font4);
        canvas.setTextColor(PC_BLACK);
        canvas.setTextDatum(top_right);
        canvas.drawString(datebuf, PC_W - 14, P3_DT_Y + 18);
    }
    canvas.drawLine(0, P3_DT_Y + P3_DT_H - 1, PC_W, P3_DT_Y + P3_DT_H - 1, PC_BLACK);
    canvas.drawLine(0, P3_DT_Y + P3_DT_H - 2, PC_W, P3_DT_Y + P3_DT_H - 2, PC_BLACK);

    // ── Outdoor + Indoor row ──
    // Left half: icon + temp + condition
    // Right half: Indoor | Humidity (two equal cells)
    canvas.fillRect(0, P3_OD_Y, PC_W, P3_OD_H, 0xFFFF);
    {
        int leftW = PC_W / 2;
        int midY  = P3_OD_Y + P3_OD_H / 2;

        // Icon
        drawWeatherIcon(&canvas, 38, midY, gWeather.fc_code[0], 44, 36);

        // Temp
        canvas.setFont(&fonts::FreeSansBold18pt7b);
        canvas.setTextColor(PC_BLACK);
        canvas.setTextDatum(middle_left);
        char tempbuf[8];
        snprintf(tempbuf, sizeof(tempbuf), "%dC", (int)round(gWeather.temp_c));
        canvas.drawString(tempbuf, 70, midY - 8);

        // Condition
        canvas.setFont(&fonts::Font2);
        canvas.setTextColor(PC_BLACK);
        canvas.drawString(gWeather.condition, 70, midY + 12);

        // Dividers
        canvas.drawLine(leftW,         P3_OD_Y + 4, leftW,         P3_OD_Y + P3_OD_H - 4, PC_BLACK);
        canvas.drawLine(leftW + PC_W / 4, P3_OD_Y + 4, leftW + PC_W / 4, P3_OD_Y + P3_OD_H - 4, PC_GREY);

        // Indoor temp
        int cx2 = leftW + PC_W / 8;
        canvas.setFont(&fonts::Font2);
        canvas.setTextColor(PC_BLACK);
        canvas.setTextDatum(top_center);
        canvas.drawString("INDOOR", cx2, P3_OD_Y + 4);
        canvas.setFont(&fonts::FreeSansBold12pt7b);
        canvas.setTextDatum(middle_center);
        char ibuf[12];
        snprintf(ibuf, sizeof(ibuf), "%.1f C", gSensor.temp_c);
        canvas.drawString(ibuf, cx2, midY + 6);

        // Humidity
        int cx3 = leftW + 3 * PC_W / 8;
        canvas.setFont(&fonts::Font2);
        canvas.setTextColor(PC_BLACK);
        canvas.setTextDatum(top_center);
        canvas.drawString("HUMIDITY", cx3, P3_OD_Y + 4);
        canvas.setFont(&fonts::FreeSansBold12pt7b);
        canvas.setTextDatum(middle_center);
        char hbuf[8];
        snprintf(hbuf, sizeof(hbuf), "%.0f%%", gSensor.humidity);
        canvas.drawString(hbuf, cx3, midY + 6);
    }
    canvas.drawLine(0, P3_OD_Y + P3_OD_H - 1, PC_W, P3_OD_Y + P3_OD_H - 1, PC_BLACK);
    canvas.drawLine(0, P3_OD_Y + P3_OD_H - 2, PC_W, P3_OD_Y + P3_OD_H - 2, PC_BLACK);

    // ── Stats row: Rain / UV / Feels — label + value properly spaced ──
    canvas.fillRect(0, P3_ST_Y, PC_W, P3_ST_H, 0xFFFF);
    {
        int colW   = PC_W / 3;
        int labelY = P3_ST_Y + 2;
        int valueY = P3_ST_Y + 22;  // more gap between label and value
        char buf[12];
        for (int i = 0; i < 3; i++) {
            int cx = i * colW + colW / 2;
            canvas.setFont(&fonts::Font2);
            canvas.setTextColor(PC_BLACK);
            canvas.setTextDatum(top_center);
            if (i == 0)      canvas.drawString("RAIN", cx, labelY);
            else if (i == 1) canvas.drawString("UV INDEX", cx, labelY);
            else             canvas.drawString("FEELS LIKE", cx, labelY);
            canvas.setFont(&fonts::FreeSansBold12pt7b);
            canvas.setTextDatum(top_center);
            if (i == 0) {
                snprintf(buf, sizeof(buf), "%d%%", gWeather.rain_chance);
                canvas.setTextColor(gWeather.rain_chance >= 60 ? PC_RED : PC_BLUE);
            } else if (i == 1) {
                snprintf(buf, sizeof(buf), "%d", gWeather.uv);
                canvas.setTextColor(uvColour(gWeather.uv));
            } else {
                snprintf(buf, sizeof(buf), "%dC", (int)round(gWeather.feelslike_c));
                canvas.setTextColor(PC_BLACK);
            }
            canvas.drawString(buf, cx, valueY);
            if (i < 2)
                canvas.drawLine((i + 1) * colW, P3_ST_Y + 4,
                                (i + 1) * colW, P3_ST_Y + P3_ST_H - 4, PC_GREY);
        }
    }
    canvas.drawLine(0, P3_ST_Y + P3_ST_H - 1, PC_W, P3_ST_Y + P3_ST_H - 1, PC_BLACK);
    canvas.drawLine(0, P3_ST_Y + P3_ST_H - 2, PC_W, P3_ST_Y + P3_ST_H - 2, PC_BLACK);

    // ── Day headers M T W T F S S ──
    {
        static const char* dayHdr[] = {"M","T","W","T","F","S","S"};
        int colW = PC_W / 7;
        canvas.fillRect(0, P3_DH_Y, PC_W, P3_DH_H, 0xFFFF);
        for (int d = 0; d < 7; d++) {
            int cx = d * colW + colW / 2;
            canvas.setFont(&fonts::Font4);
            canvas.setTextColor(d >= 5 ? PC_RED : PC_BLACK);
            canvas.setTextDatum(middle_center);
            canvas.drawString(dayHdr[d], cx, P3_DH_Y + P3_DH_H / 2);
        }
        canvas.drawLine(0, P3_DH_Y + P3_DH_H - 1, PC_W, P3_DH_Y + P3_DH_H - 1, PC_BLACK);
        canvas.drawLine(0, P3_DH_Y + P3_DH_H - 2, PC_W, P3_DH_Y + P3_DH_H - 2, PC_BLACK);
    }

    // ── Calendar grid ──
    // ── Calendar grid — dynamic row height fills available space ──
    {
        int yr = gRtc.year, mo = gRtc.month;
        if (mo < 3) { mo += 12; yr--; }
        int dow      = (1 + (13 * (mo + 1) / 5) + yr + yr / 4 - yr / 100 + yr / 400) % 7;
        int startCol = ((dow - 2) % 7 + 7) % 7;  // 0=Mon

        static const int daysInMonth[] = {0,31,28,31,30,31,30,31,31,30,31,30,31};
        int dim = daysInMonth[gRtc.month];
        if (gRtc.month == 2 && ((gRtc.year % 4 == 0 && gRtc.year % 100 != 0)
                                 || gRtc.year % 400 == 0)) dim = 29;

        // Calculate actual number of rows needed
        int totalCells = startCol + dim;
        int numRows    = (totalCells + 6) / 7;  // ceil division
        int rowH       = P3_GR_H / numRows;     // fills all available space

        int colW         = PC_W / 7;
        int todayCircleR = (rowH / 2) - 4;  // circle fits row, min reasonable size
        if (todayCircleR < 14) todayCircleR = 14;
        if (todayCircleR > 22) todayCircleR = 22;

        for (int day = 1; day <= dim; day++) {
            int idx = startCol + day - 1;
            int col = idx % 7;
            int row = idx / 7;
            int cx  = col * colW + colW / 2;
            int cy  = P3_GR_Y + row * rowH + rowH / 2;

            bool isToday = (day == gRtc.day);
            bool isWkend = (col >= 5);

            if (isToday) {
                canvas.fillCircle(cx, cy, todayCircleR, PC_RED);
                canvas.setFont(&fonts::FreeSansBold12pt7b);
                canvas.setTextColor(0xFFFF);
            } else {
                canvas.setFont(&fonts::FreeSansBold12pt7b);
                canvas.setTextColor(isWkend ? PC_RED : PC_BLACK);
            }
            canvas.setTextDatum(middle_center);
            char daybuf[4];
            snprintf(daybuf, sizeof(daybuf), "%d", day);
            canvas.drawString(daybuf, cx, cy);
        }
    }

    canvas.pushSprite(0, 0);
}

// ============================================================
// PAGE 4 — PHOTO
// Renders PSRAM image buffer if populated, placeholder if not.
// 6 hardware colours indexed 0-5:
//   0=PC_RED 1=PC_YELLOW 2=PC_GREEN 3=PC_BLUE 4=PC_BLACK 5=WHITE
// ============================================================
static const uint16_t IMG_PALETTE[6] = {
    PC_RED, PC_YELLOW, PC_GREEN, PC_BLUE, PC_BLACK, 0xFFFF
};

void renderPage4()
{
    canvas.fillSprite(0xFFFF);

    if (gImageBuffer == nullptr) {
        // Placeholder — no image uploaded yet
        canvas.setFont(&fonts::FreeSansBold24pt7b);
        canvas.setTextColor(PC_BLACK);
        canvas.setTextDatum(middle_center);
        canvas.drawString("No Image", PC_W / 2, PC_H / 2 - 40);
        canvas.setFont(&fonts::Font4);
        canvas.drawString("Open web UI and", PC_W / 2, PC_H / 2 + 10);
        canvas.drawString("upload a photo", PC_W / 2, PC_H / 2 + 34);
        canvas.setFont(&fonts::Font2);
        canvas.drawString("Press C for web mode", PC_W / 2, PC_H / 2 + 70);
    } else {
        // Render indexed pixel buffer
        for (int y = 0; y < IMG_H; y++) {
            for (int x = 0; x < IMG_W; x++) {
                uint8_t idx = gImageBuffer[y * IMG_W + x];
                if (idx > 5) idx = 4;  // clamp to valid range
                canvas.drawPixel(x, y, IMG_PALETTE[idx]);
            }
        }
    }

    canvas.pushSprite(0, 0);
}

// ============================================================
// SD CARD — INIT
// Powers SD via M5PM1 (PYG4+PYG3), checks card detect (PYG1),
// then initialises SPI and SD library.
// ============================================================
bool initSD()
{
    if (!pm1_ready) {
        Serial.println("   SD: PM1 not ready, skipping");
        return false;
    }
    // Enable SD detection pull-up and SD power
    pm1.pinMode(M5PM1_GPIO_NUM_4, OUTPUT);
    pm1.digitalWrite(M5PM1_GPIO_NUM_4, HIGH);  // PY_SD_DET_EN
    pm1.pinMode(M5PM1_GPIO_NUM_3, OUTPUT);
    pm1.digitalWrite(M5PM1_GPIO_NUM_3, HIGH);  // PY_SD_PWR_EN
    delay(50);

    // Check card detect
    pm1.pinMode(M5PM1_GPIO_NUM_1, INPUT_PULLUP);
    if (pm1.digitalRead(M5PM1_GPIO_NUM_1) != LOW) {
        Serial.println("   SD: no card detected");
        return false;
    }

    // Initialise SPI with SD pins
    SPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
    if (!SD.begin(SD_CS, SPI, 25000000)) {
        Serial.println("   SD: SD.begin() failed");
        return false;
    }

    // Create directories if needed
    if (!SD.exists("/photos"))  SD.mkdir("/photos");
    if (!SD.exists("/sensors")) SD.mkdir("/sensors");

    uint64_t cardSize = SD.cardSize() / (1024 * 1024);
    Serial.printf("   SD: OK — %llu MB\n", cardSize);
    return true;
}

// ============================================================
// SENSOR LOGGING
// Appends one CSV line per wake cycle to /sensors/log.csv
// Format: YYYY-MM-DDTHH:MM,temp_c,humidity,batt_mv,batt_pct
// Trims to last 720 entries (90 days at 8/day)
// ============================================================
void logSensorReading()
{
    char line[64];
    snprintf(line, sizeof(line), "%04d-%02d-%02dT%02d:%02d,%.1f,%.0f,%d,%d\n",
             gRtc.year, gRtc.month, gRtc.day, gRtc.hour, gRtc.min,
             gSensor.temp_c, gSensor.humidity,
             M5.Power.getBatteryVoltage(), M5.Power.getBatteryLevel());

    File f = SD.open("/sensors/log.csv", FILE_APPEND);
    if (!f) {
        Serial.println("   SD: failed to open log.csv");
        return;
    }
    f.print(line);
    f.close();

    // Count lines and trim if over 720
    f = SD.open("/sensors/log.csv", FILE_READ);
    if (!f) return;
    int lineCount = 0;
    while (f.available()) {
        if (f.read() == '\n') lineCount++;
    }
    f.close();
    Serial.printf("   SD: sensor log %d entries\n", lineCount);

    if (lineCount > 720) {
        // Read all lines, write back last 720
        f = SD.open("/sensors/log.csv", FILE_READ);
        if (!f) return;
        String all = f.readString();
        f.close();
        // Find the start of entry (lineCount - 720)
        int skip = lineCount - 720;
        int pos = 0;
        for (int i = 0; i < skip; i++) {
            int nl = all.indexOf('\n', pos);
            if (nl < 0) break;
            pos = nl + 1;
        }
        String trimmed = all.substring(pos);
        SD.remove("/sensors/log.csv");
        f = SD.open("/sensors/log.csv", FILE_WRITE);
        if (f) { f.print(trimmed); f.close(); }
        Serial.printf("   SD: trimmed log to 720 entries\n");
    }
}

// ============================================================
// SAVE BMP TO SD
// Converts indexed pixel buffer to 24-bit BMP and saves to SD.
// path must be a full path e.g. "/photos/photo_001.bmp"
// ============================================================
void saveBmpToSD(const char* path)
{
    if (!gSdReady || gImageBuffer == nullptr) return;

    const int W = IMG_W, H = IMG_H;
    const int rowBytes  = W * 3;
    const int rowStride = (rowBytes + 3) & ~3;  // 4-byte aligned
    const int pixBytes  = rowStride * H;
    const int fileSize  = 54 + pixBytes;

    uint8_t hdr[54] = {0};
    hdr[0]='B'; hdr[1]='M';
    hdr[2]=fileSize&0xFF; hdr[3]=(fileSize>>8)&0xFF;
    hdr[4]=(fileSize>>16)&0xFF; hdr[5]=(fileSize>>24)&0xFF;
    hdr[10]=54;
    hdr[14]=40;
    hdr[18]=W&0xFF; hdr[19]=(W>>8)&0xFF;
    int negH = -H;
    hdr[22]=negH&0xFF; hdr[23]=(negH>>8)&0xFF;
    hdr[24]=(negH>>16)&0xFF; hdr[25]=(negH>>24)&0xFF;
    hdr[26]=1; hdr[28]=24;
    hdr[34]=pixBytes&0xFF; hdr[35]=(pixBytes>>8)&0xFF;
    hdr[36]=(pixBytes>>16)&0xFF; hdr[37]=(pixBytes>>24)&0xFF;

    SD.remove(path);
    File f = SD.open(path, FILE_WRITE);
    if (!f) {
        Serial.printf("   SD: failed to create %s\n", path);
        return;
    }
    f.write(hdr, 54);

    // Exact same palette as IMG_PALETTE — map index to RGB
    static const uint8_t PAL[6][3] = {
        {0xCC, 0x22, 0x00},  // 0 RED
        {0xCC, 0x89, 0x00},  // 1 YELLOW
        {0x1A, 0x6B, 0x2A},  // 2 GREEN
        {0x11, 0x55, 0xCC},  // 3 BLUE
        {0x1A, 0x1A, 0x1A},  // 4 BLACK
        {0xFF, 0xFF, 0xFF},  // 5 WHITE
    };
    uint8_t row[rowStride];
    memset(row, 0, sizeof(row));
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            uint8_t idx = gImageBuffer[y * W + x];
            if (idx > 5) idx = 4;
            row[x*3+0] = PAL[idx][2];  // B
            row[x*3+1] = PAL[idx][1];  // G
            row[x*3+2] = PAL[idx][0];  // R
        }
        f.write(row, rowStride);
    }
    f.close();
    Serial.printf("   SD: saved %s (%d bytes)\n", path, fileSize);
}

// ============================================================
// PHOTO LIST — scan /photos/ directory on SD
// ============================================================
void loadPhotoList()
{
    gPhotoCount = 0;
    File dir = SD.open("/photos");
    if (!dir) {
        Serial.println("   SD: /photos dir not found");
        return;
    }
    if (!dir.isDirectory()) {
        Serial.println("   SD: /photos is not a directory");
        dir.close();
        return;
    }
    File entry = dir.openNextFile();
    while (entry && gPhotoCount < MAX_PHOTOS) {
        if (!entry.isDirectory()) {
            String name = entry.name();
            Serial.printf("   SD: found entry: %s\n", name.c_str());
            // Only include .bmp files
            if (name.endsWith(".bmp") || name.endsWith(".BMP")) {
                // entry.name() may or may not include path depending on library version
                if (name.startsWith("/photos/") || name.startsWith("/")) {
                    // Full path already included
                    strncpy(gPhotoList[gPhotoCount], name.c_str(), 31);
                } else {
                    snprintf(gPhotoList[gPhotoCount], 32, "/photos/%s", name.c_str());
                }
                gPhotoList[gPhotoCount][31] = 0;
                Serial.printf("   SD: added photo[%d]: %s\n", gPhotoCount, gPhotoList[gPhotoCount]);
                gPhotoCount++;
            }
        }
        entry.close();
        entry = dir.openNextFile();
    }
    dir.close();
    Serial.printf("   SD: found %d photos\n", gPhotoCount);
}

// ============================================================
// RENDER SLIDE FROM SD
// Loads a BMP from SD into the canvas and pushes to display.
// Loads a BMP from SD into the canvas and pushes to display.
// ============================================================
void renderSlideFromSD(const char* path)
{
    canvas.fillSprite(0xFFFF);

    // Manual 24-bit BMP decode — direct SD file read
    File f = SD.open(path, FILE_READ);
    if (!f) {
        Serial.printf("   Slide: failed to open %s\n", path);
        canvas.setFont(&fonts::Font4);
        canvas.setTextColor(PC_RED);
        canvas.setTextDatum(middle_center);
        canvas.drawString("Photo load failed", PC_W/2, PC_H/2);
        canvas.pushSprite(0, 0);
        return;
    }

    // Read BMP header
    uint8_t hdr[54];
    f.read(hdr, 54);
    if (hdr[0] != 'B' || hdr[1] != 'M') {
        Serial.printf("   Slide: not a BMP: %s\n", path);
        f.close();
        canvas.pushSprite(0, 0);
        return;
    }
    int32_t w    = hdr[18] | (hdr[19]<<8) | (hdr[20]<<16) | (hdr[21]<<24);
    int32_t h    = hdr[22] | (hdr[23]<<8) | (hdr[24]<<16) | (hdr[25]<<24);
    bool topDown = (h < 0);
    if (h < 0) h = -h;
    int rowStride = ((w * 3) + 3) & ~3;

    uint8_t* row = (uint8_t*)heap_caps_malloc(rowStride, MALLOC_CAP_SPIRAM);
    if (!row) {
        Serial.println("   Slide: PSRAM alloc failed for row buffer");
        f.close();
        canvas.pushSprite(0, 0);
        return;
    }

    for (int y = 0; y < h && y < IMG_H; y++) {
        int fy = topDown ? y : (h - 1 - y);
        f.seek(54 + fy * rowStride);
        f.read(row, rowStride);
        for (int x = 0; x < w && x < IMG_W; x++) {
            uint8_t b = row[x*3+0];
            uint8_t g = row[x*3+1];
            uint8_t r = row[x*3+2];
            canvas.drawPixel(x, y, canvas.color565(r, g, b));
        }
    }
    free(row);
    f.close();
    canvas.pushSprite(0, 0);
    Serial.printf("   Slide: rendered %s\n", path);
}

// ============================================================
// FIND NEXT PHOTO FILENAME
// Returns path like "/photos/photo_001.bmp" for next available slot
// ============================================================
String nextPhotoPath()
{
    char path[32];
    for (int i = 1; i <= 999; i++) {
        snprintf(path, sizeof(path), "/photos/photo_%03d.bmp", i);
        if (!SD.exists(path)) return String(path);
    }
    return "/photos/photo_001.bmp";  // overwrite if full
}

void handleViewPhoto()
{
    if (!server.hasArg("file")) { server.send(400, "text/plain", "no file"); return; }
    String fname = server.arg("file");
    // Sanitise — only allow simple filenames, no path traversal
    if (fname.indexOf('/') >= 0 || fname.indexOf('.') != fname.lastIndexOf('.')) {
        server.send(400, "text/plain", "invalid filename");
        return;
    }
    String path = "/photos/" + fname;
    File f = SD.open(path, FILE_READ);
    if (!f) { server.send(404, "text/plain", "not found"); return; }
    server.sendHeader("Cache-Control", "no-cache");
    server.streamFile(f, "image/bmp");
    f.close();
}

// ============================================================
// API: GET /api/photos — list photos on SD
// ============================================================
void handleGetPhotos()
{
    loadPhotoList();
    String json = "[";
    for (int i = 0; i < gPhotoCount; i++) {
        if (i > 0) json += ",";
        // Extract filename only
        String full = gPhotoList[i];
        int slash = full.lastIndexOf('/');
        String fname = (slash >= 0) ? full.substring(slash + 1) : full;
        json += "\"" + fname + "\"";
    }
    json += "]";
    server.send(200, "application/json", json);
}

// ============================================================
// API: POST /api/photos/delete — delete a photo
// ============================================================
// ============================================================
// API: POST /api/photos/display — display a specific photo
// body: {"filename":"photo_003.bmp"}
// Sets that photo as current, renders immediately, exits slideshow
// ============================================================
void handleDisplayPhoto()
{
    if (!server.hasArg("plain")) { server.send(400,"application/json","{\"error\":\"no body\"}"); return; }
    StaticJsonDocument<64> doc;
    if (deserializeJson(doc, server.arg("plain"))) { server.send(400,"application/json","{\"error\":\"bad json\"}"); return; }
    const char* fname = doc["filename"] | "";
    if (strlen(fname) == 0) { server.send(400,"application/json","{\"error\":\"no filename\"}"); return; }

    // Find the index of this photo in gPhotoList
    char path[48];
    snprintf(path, sizeof(path), "/photos/%s", fname);
    int idx = -1;
    for (int i = 0; i < gPhotoCount; i++) {
        if (strcmp(gPhotoList[i], path) == 0) { idx = i; break; }
    }
    if (idx < 0) { server.send(404,"application/json","{\"error\":\"not found\"}"); return; }

    // Set state — exit slideshow, go to photo page
    gSlideIndex    = idx;
    gCurrentPage   = 4;
    gPhotoMode     = true;
    gSlideshowMode = false;
    savePrefs();

    // Render immediately — blocks until complete before responding
    ledRendering();
    renderSlideFromSD(gPhotoList[idx]);
    ledWebMode();

    server.send(200, "application/json", "{\"ok\":true}");
    Serial.printf("   Display photo: %s (index %d)\n", path, idx);
}

void handleDeletePhoto()
{
    if (!server.hasArg("plain")) { server.send(400,"application/json","{\"error\":\"no body\"}"); return; }
    StaticJsonDocument<64> doc;
    if (deserializeJson(doc, server.arg("plain"))) { server.send(400,"application/json","{\"error\":\"bad json\"}"); return; }
    const char* fname = doc["filename"] | "";
    if (strlen(fname) == 0) { server.send(400,"application/json","{\"error\":\"no filename\"}"); return; }
    char path[48];
    snprintf(path, sizeof(path), "/photos/%s", fname);
    bool ok = SD.remove(path);
    loadPhotoList();
    server.send(200, "application/json", ok ? "{\"ok\":true}" : "{\"error\":\"delete failed\"}");
    Serial.printf("   SD: delete %s %s\n", path, ok ? "OK" : "FAILED");
}

// ============================================================
// API: POST /api/slideshow — control slideshow
// body: {"active":true,"interval":300}
// ============================================================
void handleSlideshow()
{
    if (!server.hasArg("plain")) { server.send(400,"application/json","{\"error\":\"no body\"}"); return; }
    StaticJsonDocument<64> doc;
    if (deserializeJson(doc, server.arg("plain"))) { server.send(400,"application/json","{\"error\":\"bad json\"}"); return; }
    if (doc.containsKey("active"))   gSlideshowMode = doc["active"];
    if (doc.containsKey("interval")) gSlideInterval = doc["interval"];
    if (doc.containsKey("index"))    gSlideIndex    = doc["index"];
    if (gSlideInterval < 10) gSlideInterval = 10;
    // Clamp index
    if (gPhotoCount > 0) gSlideIndex = gSlideIndex % gPhotoCount;
    // Turning on slideshow: set photo mode, go to page 4
    if (gSlideshowMode) {
        gPhotoMode   = true;
        gCurrentPage = 4;
        gLastSlideMs = 0;  // force immediate render of first slide
    }
    savePrefs();
    // If activating slideshow, render first slide immediately before responding
    if (gSlideshowMode && gSdReady && gPhotoCount > 0) {
        int idx = (gSlideIndex < gPhotoCount) ? gSlideIndex : 0;
        ledRendering();
        renderSlideFromSD(gPhotoList[idx]);
        ledWebMode();
    }
    server.send(200, "application/json", "{\"ok\":true}");
    Serial.printf("   Slideshow: active=%d interval=%d index=%d\n",
                  gSlideshowMode, gSlideInterval, gSlideIndex);
}

// ============================================================
// API: GET /api/sensors/history — last N sensor readings
// ============================================================
// ============================================================
// API: POST /api/sensors/reading — take an immediate sensor reading
// Reads SHT40, logs to SD, returns the new values
// ============================================================
void handleTakeReading()
{
    readSensor();
    if (gSdReady) logSensorReading();

    StaticJsonDocument<128> doc;
    doc["temp_c"]   = gSensor.temp_c;
    doc["humidity"] = gSensor.humidity;
    doc["batt_mv"]  = M5.Power.getBatteryVoltage();
    doc["batt_pct"] = M5.Power.getBatteryLevel();
    doc["ok"]       = true;

    String out;
    serializeJson(doc, out);
    server.send(200, "application/json", out);
    Serial.printf("   Manual reading: %.1f C  %.0f%%\n", gSensor.temp_c, gSensor.humidity);
}

void handleSensorHistory()
{
    if (!gSdReady) { server.send(200,"application/json","[]"); return; }
    File f = SD.open("/sensors/log.csv", FILE_READ);
    if (!f) { server.send(200,"application/json","[]"); return; }

    // Read last 168 lines (21 days at 8/day — good chart range)
    const int MAX_LINES = 168;
    String lines[MAX_LINES];
    int count = 0;
    while (f.available()) {
        String line = f.readStringUntil('\n');
        line.trim();
        if (line.length() > 0) {
            lines[count % MAX_LINES] = line;
            count++;
        }
    }
    f.close();

    int start = (count > MAX_LINES) ? (count % MAX_LINES) : 0;
    int total = min(count, MAX_LINES);

    String json = "[";
    for (int i = 0; i < total; i++) {
        String& l = lines[(start + i) % MAX_LINES];
        // Parse: timestamp,temp,hum,batt_mv,batt_pct
        int c1 = l.indexOf(',');
        int c2 = l.indexOf(',', c1+1);
        int c3 = l.indexOf(',', c2+1);
        int c4 = l.indexOf(',', c3+1);
        if (c1 < 0) continue;
        if (i > 0) json += ",";
        json += "{\"ts\":\"" + l.substring(0, c1) + "\"";
        if (c2 > c1) json += ",\"t\":" + l.substring(c1+1, c2);
        if (c3 > c2) json += ",\"h\":" + l.substring(c2+1, c3);
        if (c4 > c3) json += ",\"mv\":" + l.substring(c3+1, c4);
        if (c4 > 0)  json += ",\"pct\":" + l.substring(c4+1);
        json += "}";
    }
    json += "]";
    server.send(200, "application/json", json);
}


void loadPrefs()
{
    prefs.begin("papercolor", true);
    gCurrentPage       = prefs.getInt("page",      1);
    gWakeStart         = prefs.getInt("wakeStart", 6);
    gWakeEnd           = prefs.getInt("wakeEnd",  20);
    gWakeInterval      = prefs.getInt("wakeIntv",  2);
    gPhotoMode         = prefs.getBool("photoMode", false);
    gSlideshowMode     = prefs.getBool("slideshow", false);
    gSlideInterval     = prefs.getInt("slideIntv", 300);
    gLedMode           = (LedMode)prefs.getInt("ledMode", (int)LED_STATUS);
    gLedR              = prefs.getUChar("ledR",  255);
    gLedG              = prefs.getUChar("ledG",  255);
    gLedB              = prefs.getUChar("ledB",  255);
    gLedBrightness     = prefs.getUChar("ledBri", 80);
    prefs.end();
    if (gCurrentPage  < 1 || gCurrentPage  > 4) gCurrentPage  = 1;
    if (gWakeStart    < 0 || gWakeStart    > 22) gWakeStart    = 6;
    if (gWakeEnd      < 1 || gWakeEnd      > 23) gWakeEnd      = 20;
    if (gWakeInterval < 1 || gWakeInterval >  8) gWakeInterval = 2;
    if (gWakeEnd <= gWakeStart) gWakeEnd = gWakeStart + gWakeInterval;
    if (gLedMode < LED_STATUS || gLedMode > LED_RAINBOW) gLedMode = LED_STATUS;
    if (gSlideInterval < 10) gSlideInterval = 60;
}

void savePrefs()
{
    prefs.begin("papercolor", false);
    prefs.putInt("page",       gCurrentPage);
    prefs.putInt("wakeStart",  gWakeStart);
    prefs.putInt("wakeEnd",    gWakeEnd);
    prefs.putInt("wakeIntv",   gWakeInterval);
    prefs.putBool("photoMode", gPhotoMode);
    prefs.putBool("slideshow", gSlideshowMode);
    prefs.putInt("slideIntv",  gSlideInterval);
    prefs.putInt("ledMode",    (int)gLedMode);
    prefs.putUChar("ledR",     gLedR);
    prefs.putUChar("ledG",     gLedG);
    prefs.putUChar("ledB",     gLedB);
    prefs.putUChar("ledBri",   gLedBrightness);
    prefs.end();
    Serial.printf("   NVS saved: page=%d photoMode=%d slideshow=%d\n",
                  gCurrentPage, gPhotoMode, gSlideshowMode);
}

// ============================================================
// WEB MODE DISPLAY — renders IP address screen
// ============================================================
void renderWebMode()
{
    canvas.fillSprite(0xFFFF);

    // Title
    canvas.setFont(&fonts::FreeSansBold24pt7b);
    canvas.setTextColor(PC_BLACK);
    canvas.setTextDatum(top_center);
    canvas.drawString("Web Mode", PC_W / 2, 40);

    // Divider
    canvas.drawLine(20, 94, PC_W - 20, 94, PC_BLACK);
    canvas.drawLine(20, 95, PC_W - 20, 95, PC_BLACK);

    // IP address — large and readable
    canvas.setFont(&fonts::Font4);
    canvas.setTextColor(PC_BLACK);
    canvas.setTextDatum(top_center);
    canvas.drawString("Open in browser:", PC_W / 2, 110);

    String ip = WiFi.localIP().toString();
    canvas.setFont(&fonts::FreeSansBold24pt7b);
    canvas.setTextColor(PC_BLUE);
    canvas.setTextDatum(top_center);
    canvas.drawString(ip.c_str(), PC_W / 2, 148);

    // Sensor snapshot
    canvas.drawLine(20, 210, PC_W - 20, 210, PC_GREY);
    canvas.setFont(&fonts::Font4);
    canvas.setTextColor(PC_BLACK);
    canvas.setTextDatum(top_left);
    canvas.drawString("Indoor", 20, 225);
    char sbuf[32];
    snprintf(sbuf, sizeof(sbuf), "%.1f C   %.0f%%",
             gSensor.temp_c, gSensor.humidity);
    canvas.setFont(&fonts::FreeSansBold18pt7b);
    canvas.drawString(sbuf, 20, 248);

    // Battery + WiFi
    canvas.setFont(&fonts::Font4);
    canvas.setTextColor(PC_BLACK);
    canvas.setTextDatum(top_left);
    int rssi = WiFi.RSSI();
    char wbuf[48];
    snprintf(wbuf, sizeof(wbuf), "Battery %d%%   WiFi %d dBm",
             M5.Power.getBatteryLevel(), rssi);
    canvas.drawString(wbuf, 20, 300);

    // Instructions
    canvas.drawLine(20, 346, PC_W - 20, 346, PC_GREY);
    canvas.setFont(&fonts::Font2);
    canvas.setTextColor(PC_BLACK);
    canvas.setTextDatum(top_center);
    canvas.drawString("Press Button C to exit web mode", PC_W / 2, 360);
    canvas.drawString("Device stays awake while web mode is active", PC_W / 2, 378);

    canvas.pushSprite(0, 0);
    Serial.printf("   Web mode — IP: %s\n", ip.c_str());
}

// ============================================================
// WEB SERVER HTML — single page app, served from PROGMEM
// ============================================================
static const char WEB_HTML[] PROGMEM = R"HTMLEOF(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>PaperColor Dashboard</title>
<style>
  *{box-sizing:border-box;margin:0;padding:0}
  body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif;background:#f0f2f5;color:#1a1a2e;min-height:100vh}
  header{background:#1a1a2e;color:#fff;padding:16px 20px;display:flex;align-items:center;gap:12px}
  header h1{font-size:20px;font-weight:600}
  .dot{width:10px;height:10px;border-radius:50%;background:#4caf50;animation:pulse 2s infinite}
  @keyframes pulse{0%,100%{opacity:1}50%{opacity:.4}}
  .tabs{display:flex;background:#fff;border-bottom:2px solid #e0e0e0;padding:0 8px;overflow-x:auto}
  .tab{padding:12px 14px;cursor:pointer;font-size:13px;font-weight:500;color:#666;border-bottom:3px solid transparent;margin-bottom:-2px;transition:all .2s;white-space:nowrap}
  .tab.active{color:#1a1a2e;border-bottom-color:#1155cc}
  .panel{display:none;padding:16px;max-width:600px;margin:0 auto}
  .panel.active{display:block}
  .card{background:#fff;border-radius:12px;padding:18px;margin-bottom:14px;box-shadow:0 1px 4px rgba(0,0,0,.08)}
  .card h2{font-size:12px;font-weight:600;text-transform:uppercase;letter-spacing:.8px;color:#888;margin-bottom:12px}
  .stat-grid{display:grid;grid-template-columns:1fr 1fr;gap:10px}
  .stat-grid-3{display:grid;grid-template-columns:1fr 1fr 1fr;gap:10px}
  .stat{background:#f7f8fa;border-radius:8px;padding:12px}
  .stat .label{font-size:11px;color:#888;margin-bottom:3px}
  .stat .value{font-size:22px;font-weight:700;color:#1a1a2e;line-height:1.1}
  .stat .unit{font-size:12px;color:#888;margin-left:2px}
  .stat .sub{font-size:11px;color:#888;margin-top:3px}
  .badge{display:inline-block;padding:2px 8px;border-radius:12px;font-size:11px;font-weight:600;margin-top:3px}
  .badge-green{background:#e8f5e9;color:#1a6b2a}
  .badge-blue{background:#e8f0fe;color:#1155cc}
  .badge-grey{background:#f0f0f0;color:#666}
  .btn{display:inline-flex;align-items:center;gap:6px;padding:9px 18px;border:none;border-radius:8px;font-size:13px;font-weight:600;cursor:pointer;transition:all .15s}
  .btn-primary{background:#1155cc;color:#fff}.btn-primary:hover{background:#0d3fa0}
  .btn-danger{background:#cc2200;color:#fff}.btn-danger:hover{background:#a01a00}
  .btn-secondary{background:#f0f2f5;color:#1a1a2e;border:1px solid #ddd}.btn-secondary:hover{background:#e4e6e9}
  .btn-success{background:#1a6b2a;color:#fff}.btn-success:hover{background:#145220}
  .btn:disabled{opacity:.5;cursor:not-allowed}
  .btn-row{display:flex;gap:8px;flex-wrap:wrap;margin-top:8px}
  .form-row{margin-bottom:14px}
  .form-row label{display:block;font-size:12px;font-weight:500;margin-bottom:5px;color:#444}
  .form-row input[type=number],.form-row select{width:100%;padding:9px 11px;border:1.5px solid #ddd;border-radius:8px;font-size:14px}
  .form-row input[type=number]:focus,.form-row select:focus{outline:none;border-color:#1155cc}
  .form-row input.dirty{border-color:#cc8900;background:#fffdf0}
  input[type=range]{width:100%;accent-color:#1155cc}
  input[type=color]{width:44px;height:34px;border:none;border-radius:6px;cursor:pointer;padding:2px}
  .page-selector{display:grid;grid-template-columns:repeat(4,1fr);gap:8px;margin-bottom:14px}
  .page-btn{padding:10px 4px;border:2px solid #ddd;border-radius:10px;background:#fff;cursor:pointer;text-align:center;transition:all .2s;font-size:11px;font-weight:500}
  .page-btn.active{border-color:#1155cc;background:#e8f0fe;color:#1155cc}
  .page-btn .pg-num{font-size:18px;font-weight:700;display:block;margin-bottom:3px}
  .preview-wrap{background:#f0f2f5;border-radius:10px;padding:10px;text-align:center;min-height:80px;display:flex;align-items:center;justify-content:center}
  .preview-wrap img{max-width:100%;max-height:320px;border-radius:6px;box-shadow:0 2px 8px rgba(0,0,0,.15)}
  .preview-loading{color:#888;font-size:13px}
  .info-row{display:flex;justify-content:space-between;padding:7px 0;border-bottom:1px solid #f0f0f0;font-size:13px}
  .info-row:last-child{border:none}
  .info-row .info-label{color:#888}
  .toast{position:fixed;bottom:24px;left:50%;transform:translateX(-50%);background:#1a1a2e;color:#fff;padding:10px 20px;border-radius:8px;font-size:14px;opacity:0;transition:opacity .3s;pointer-events:none;z-index:999}
  .toast.show{opacity:1}
  .sched-summary{background:#e8f0fe;border-radius:8px;padding:10px;font-size:12px;color:#1a1a2e;margin-bottom:14px}
  .dirty-notice{background:#fffdf0;border:1px solid #cc8900;border-radius:8px;padding:9px 12px;font-size:12px;color:#7a5500;margin-bottom:10px;display:none}
  .dirty-notice.show{display:block}
  /* LED */
  .led-mode-grid{display:grid;grid-template-columns:repeat(3,1fr);gap:8px;margin-bottom:14px}
  .led-mode-btn{padding:14px 6px;border:2px solid #ddd;border-radius:10px;background:#fff;cursor:pointer;text-align:center;transition:all .2s;font-size:12px;font-weight:500}
  .led-mode-btn .mode-icon{font-size:22px;display:block;margin-bottom:4px}
  .led-mode-btn.active{border-color:#1155cc;background:#e8f0fe;color:#1155cc}
  .led-preview{display:flex;gap:8px;align-items:center;margin-bottom:14px}
  .led-dot{width:26px;height:26px;border-radius:50%;border:2px solid #ddd;transition:background .3s}
  .colour-presets{display:flex;gap:6px;flex-wrap:wrap;margin-bottom:10px}
  .colour-preset{width:26px;height:26px;border-radius:50%;cursor:pointer;border:2px solid transparent}
  .colour-preset:hover{border-color:#1155cc}
  .quick-led{display:flex;align-items:center;gap:8px;padding:8px 0}
  .quick-led label{font-size:13px;color:#666;flex:1}
  /* Photo */
  .drop-zone{border:2.5px dashed #ccd;border-radius:12px;padding:32px 16px;text-align:center;cursor:pointer;transition:all .2s;background:#fafbfc}
  .drop-zone:hover,.drop-zone.drag-over{border-color:#1155cc;background:#e8f0fe}
  .drop-zone .drop-icon{font-size:36px;margin-bottom:8px}
  .photo-controls{display:none}.photo-controls.show{display:block}
  .fit-toggle{display:flex;gap:6px;margin-bottom:14px}
  .fit-btn{flex:1;padding:9px;border:2px solid #ddd;border-radius:8px;background:#fff;cursor:pointer;font-size:12px;font-weight:500;text-align:center;transition:all .2s}
  .fit-btn.active{border-color:#1155cc;background:#e8f0fe;color:#1155cc}
  .rotation-row{display:flex;gap:6px;margin-bottom:14px}
  .rot-btn{flex:1;padding:7px;border:1.5px solid #ddd;border-radius:8px;background:#fff;cursor:pointer;font-size:12px;font-weight:500;text-align:center}
  .rot-btn:hover{background:#f0f2f5}
  .rot-btn.active{border-color:#1155cc;background:#e8f0fe;color:#1155cc}
  .preview-row{display:grid;grid-template-columns:1fr 1fr;gap:10px;margin-bottom:14px}
  .preview-col{text-align:center}
  .preview-col label{display:block;font-size:11px;color:#888;margin-bottom:5px;font-weight:500;text-transform:uppercase;letter-spacing:.5px}
  .preview-col canvas{max-width:100%;border-radius:6px;box-shadow:0 2px 6px rgba(0,0,0,.12);border:1px solid #e0e0e0}
  .photo-mode-banner{background:#1a6b2a;color:#fff;border-radius:10px;padding:12px 16px;margin-bottom:14px;display:none;align-items:center;gap:8px;font-size:13px}
  .photo-mode-banner.show{display:flex}
  /* Photo library */
  .photo-grid{display:grid;grid-template-columns:repeat(3,1fr);gap:8px;margin-bottom:14px}
  .photo-item{position:relative;background:#f0f0f0;border-radius:8px;overflow:hidden;aspect-ratio:2/3}
  .photo-item img{width:100%;height:100%;object-fit:cover}
  .photo-item .del-btn{position:absolute;top:4px;right:4px;background:rgba(204,34,0,.85);color:#fff;border:none;border-radius:50%;width:22px;height:22px;font-size:12px;cursor:pointer;display:flex;align-items:center;justify-content:center}
  .photo-item .disp-btn{position:absolute;top:4px;left:4px;background:rgba(17,85,204,.85);color:#fff;border:none;border-radius:50%;width:22px;height:22px;font-size:12px;cursor:pointer;display:flex;align-items:center;justify-content:center}
  .photo-item .photo-name{position:absolute;bottom:0;left:0;right:0;background:rgba(0,0,0,.5);color:#fff;font-size:9px;padding:2px 4px;text-align:center;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
  .slideshow-bar{display:flex;align-items:center;gap:10px;background:#f7f8fa;border-radius:10px;padding:12px;margin-bottom:14px}
  .slide-toggle{width:44px;height:24px;border-radius:12px;background:#ddd;cursor:pointer;position:relative;transition:background .2s;flex-shrink:0}
  .slide-toggle.on{background:#1155cc}
  .slide-toggle::after{content:'';position:absolute;top:3px;left:3px;width:18px;height:18px;border-radius:50%;background:#fff;transition:left .2s}
  .slide-toggle.on::after{left:23px}
  /* Sensors chart */
  .chart-wrap{background:#f7f8fa;border-radius:10px;padding:10px;margin-bottom:14px}
  .chart-canvas{width:100%;height:200px;display:block}
  .chart-legend{display:flex;gap:14px;font-size:11px;color:#888;margin-top:6px}
  .chart-legend span{display:flex;align-items:center;gap:4px}
  .legend-dot{width:8px;height:8px;border-radius:50%}
  .sd-badge{display:inline-flex;align-items:center;gap:4px;padding:3px 10px;border-radius:12px;font-size:11px;font-weight:600}
  .sd-ok{background:#e8f5e9;color:#1a6b2a}
  .sd-no{background:#fce8e8;color:#cc2200}
  /* Time range chips */
  .time-chips{display:flex;gap:6px;margin-bottom:14px;flex-wrap:wrap}
  .time-chip{padding:5px 14px;border:1.5px solid #ddd;border-radius:20px;font-size:12px;font-weight:500;cursor:pointer;background:#fff;color:#666;transition:all .15s}
  .time-chip.active{border-color:#1155cc;background:#1155cc;color:#fff}
</style>
</head>
<body>
<header><div class="dot"></div><h1>PaperColor Dashboard</h1></header>
<div class="tabs">
  <div class="tab active" onclick="showTab('dashboard')">Dashboard</div>
  <div class="tab" onclick="showTab('sensors')">&#128200; Sensors</div>
  <div class="tab" onclick="showTab('schedule')">Schedule</div>
  <div class="tab" onclick="showTab('settings')">Settings</div>
  <div class="tab" onclick="showTab('photo')">&#128247; Photo</div>
  <div class="tab" onclick="showTab('leds')">&#128161; LEDs</div>
</div>

<!-- DASHBOARD -->
<div class="panel active" id="tab-dashboard">
  <div class="card"><h2>Indoor Sensors</h2>
    <div class="stat-grid">
      <div class="stat"><div class="label">Temperature</div><div class="value" id="temp">--<span class="unit">°C</span></div></div>
      <div class="stat"><div class="label">Humidity</div><div class="value" id="hum">--<span class="unit">%</span></div></div>
    </div>
  </div>
  <div class="card"><h2>Battery</h2>
    <div class="stat-grid-3">
      <div class="stat"><div class="label">Charge</div><div class="value" id="batt">--<span class="unit">%</span></div></div>
      <div class="stat"><div class="label">Voltage</div><div class="value" id="batt_mv">--<span class="unit">mV</span></div></div>
      <div class="stat"><div class="label">Status</div><div class="value" style="font-size:13px;padding-top:4px" id="batt_status">--</div></div>
    </div>
  </div>
  <div class="card"><h2>Device Status</h2>
    <div class="stat-grid">
      <div class="stat"><div class="label">WiFi</div><div class="value" id="rssi">--<span class="unit">dBm</span></div><div class="sub" id="rssi_bar"></div></div>
      <div class="stat"><div class="label">Heap</div><div class="value" id="heap" style="font-size:17px;padding-top:3px">--</div></div>
    </div>
    <div style="margin-top:10px">
      <div class="info-row"><span class="info-label">Updated</span><span id="updated">--</span></div>
      <div class="info-row"><span class="info-label">Current page</span><span id="curpage">--</span></div>
      <div class="info-row"><span class="info-label">Uptime</span><span id="uptime">--</span></div>
      <div class="info-row"><span class="info-label">IP</span><span id="ip">--</span></div>
      <div class="info-row"><span class="info-label">SD Card</span><span id="sd-status">--</span></div>
    </div>
  </div>
  <div class="card"><h2>Controls</h2>
    <div class="btn-row">
      <button class="btn btn-primary" onclick="doRefresh()">&#8635; Refresh</button>
      <button class="btn btn-danger" onclick="doSleep()">&#9210; Sleep Now</button>
    </div>
  </div>
</div>

<!-- SENSORS TAB -->
<div class="panel" id="tab-sensors">
  <div class="card"><h2>Latest Reading</h2>
    <div class="stat-grid" style="margin-bottom:12px">
      <div class="stat"><div class="label">Temperature</div><div class="value" id="s-temp">--<span class="unit">°C</span></div></div>
      <div class="stat"><div class="label">Humidity</div><div class="value" id="s-hum">--<span class="unit">%</span></div></div>
    </div>
    <div class="btn-row">
      <button class="btn btn-primary" id="reading-btn" onclick="takeReading()">&#9673; Take Reading</button>
      <button class="btn btn-secondary" onclick="loadSensorHistory()">&#8635; Refresh Charts</button>
    </div>
    <div id="reading-status" style="font-size:12px;color:#888;margin-top:8px"></div>
  </div>
  <div class="card">
    <div style="display:flex;align-items:center;justify-content:space-between;margin-bottom:10px">
      <h2 style="margin-bottom:0">Time Range</h2>
      <span style="font-size:11px;color:#aaa" id="sensor-count">Loading...</span>
    </div>
    <div class="time-chips">
      <div class="time-chip"        id="chip-1h"  onclick="setTimeRange(1)">1 hr</div>
      <div class="time-chip active" id="chip-8h"  onclick="setTimeRange(8)">8 hrs</div>
      <div class="time-chip"        id="chip-24h" onclick="setTimeRange(24)">24 hrs</div>
      <div class="time-chip"        id="chip-7d"  onclick="setTimeRange(168)">7 days</div>
      <div class="time-chip"        id="chip-30d" onclick="setTimeRange(720)">30 days</div>
      <div class="time-chip"        id="chip-90d" onclick="setTimeRange(2160)">90 days</div>
    </div>
  </div>
  <div class="card"><h2>Temperature</h2>
    <div class="chart-wrap"><canvas class="chart-canvas" id="temp-chart"></canvas></div>
    <div class="chart-legend"><span><div class="legend-dot" style="background:#cc2200"></div>Temperature °C</span></div>
  </div>
  <div class="card"><h2>Humidity</h2>
    <div class="chart-wrap"><canvas class="chart-canvas" id="hum-chart"></canvas></div>
    <div class="chart-legend"><span><div class="legend-dot" style="background:#1155cc"></div>Humidity %</span></div>
  </div>
  <div class="card"><h2>Battery</h2>
    <div class="chart-wrap"><canvas class="chart-canvas" id="batt-chart"></canvas></div>
    <div class="chart-legend"><span><div class="legend-dot" style="background:#1a6b2a"></div>Battery %</span></div>
  </div>
</div>

<!-- SCHEDULE -->
<div class="panel" id="tab-schedule">
  <div class="card"><h2>Wake Schedule</h2>
    <div class="sched-summary" id="sched-summary">Loading...</div>
    <div class="dirty-notice" id="dirty-notice">&#9888; Unsaved changes</div>
    <div class="form-row"><label>Start Hour (0–22)</label><input type="number" id="wakeStart" min="0" max="22" value="6"></div>
    <div class="form-row"><label>End Hour (1–23)</label><input type="number" id="wakeEnd" min="1" max="23" value="20"></div>
    <div class="form-row"><label>Interval (hours, 1–8)</label><input type="number" id="wakeInterval" min="1" max="8" value="2"></div>
    <button class="btn btn-primary" onclick="saveSchedule()">Save Schedule</button>
  </div>
</div>

<!-- SETTINGS -->
<div class="panel" id="tab-settings">
  <div class="card"><h2>Default Page</h2>
    <div class="page-selector" id="page-selector">
      <div class="page-btn" onclick="setPage(1)"><span class="pg-num">1</span>Dashboard</div>
      <div class="page-btn" onclick="setPage(2)"><span class="pg-num">2</span>Hourly</div>
      <div class="page-btn" onclick="setPage(3)"><span class="pg-num">3</span>Calendar</div>
      <div class="page-btn" onclick="setPage(4)"><span class="pg-num">4</span>Photo</div>
    </div>
    <div class="preview-wrap">
      <div class="preview-loading" id="preview-loading">Select a page to preview</div>
      <img id="preview-img" style="display:none" alt="Display preview">
    </div>
  </div>
  <div class="card"><h2>LED Quick Control</h2>
    <div class="quick-led">
      <div class="led-dot" id="quick-led-dot" style="background:#fff"></div>
      <label id="quick-led-label">Loading...</label>
      <button class="btn btn-secondary" style="padding:5px 12px;font-size:12px" onclick="showTab('leds')">Edit &#8599;</button>
    </div>
    <div class="form-row" style="margin:6px 0 0 0"><label>Brightness</label>
      <input type="range" id="quick-brightness" min="0" max="255" value="80" oninput="quickBrightness(this.value)">
    </div>
  </div>
  <div class="card"><h2>Device Info</h2>
    <div class="info-row"><span class="info-label">Free PSRAM</span><span id="psram">--</span></div>
    <div class="info-row"><span class="info-label">IP Address</span><span id="ip2">--</span></div>
  </div>
</div>

<!-- PHOTO -->
<div class="panel" id="tab-photo">
  <div class="photo-mode-banner" id="photo-banner">
    <span style="font-size:18px">&#128247;</span>
    <div><strong>Photo mode active</strong> — device staying awake.</div>
  </div>

  <!-- Slideshow control -->
  <div class="card"><h2>Slideshow</h2>
    <div class="slideshow-bar">
      <div class="slide-toggle" id="slide-toggle" onclick="toggleSlideshow()"></div>
      <div style="flex:1">
        <div style="font-size:13px;font-weight:500" id="slide-label">Off</div>
        <div style="font-size:11px;color:#888" id="slide-info">--</div>
      </div>
    </div>
    <div class="form-row"><label>Interval</label>
      <select id="slide-interval" onchange="setSlideInterval(this.value)">
        <option value="60">1 minute</option>
        <option value="300" selected>5 minutes</option>
        <option value="600">10 minutes</option>
        <option value="1800">30 minutes</option>
        <option value="3600">1 hour</option>
      </select>
    </div>
    <div class="btn-row">
      <button class="btn btn-success" id="start-ss-btn" onclick="startSlideshow()">&#9654; Start Slideshow</button>
      <button class="btn btn-secondary" id="stop-ss-btn" onclick="stopSlideshow()" style="display:none">&#9646;&#9646; Stop</button>
    </div>
    <div id="ss-status" style="font-size:12px;color:#888;margin-top:8px"></div>
  </div>

  <!-- Photo library -->
  <div class="card"><h2>Photos on SD <span id="photo-count-badge" style="font-size:11px;font-weight:400;color:#888"></span></h2>
    <div class="photo-grid" id="photo-grid"></div>
    <div id="no-photos" style="text-align:center;color:#aaa;font-size:13px;padding:16px 0">No photos on SD card</div>
    <button class="btn btn-secondary" onclick="loadPhotoLibrary()" style="font-size:12px;padding:7px 14px">&#8635; Refresh list</button>
  </div>

  <!-- Upload -->
  <div class="card"><h2>Upload New Photo</h2>
    <div class="drop-zone" id="drop-zone" onclick="document.getElementById('file-input').click()"
         ondragover="event.preventDefault();this.classList.add('drag-over')"
         ondragleave="this.classList.remove('drag-over')"
         ondrop="handleDrop(event)">
      <div class="drop-icon">&#128444;</div>
      <p><strong>Click to choose</strong> or drag &amp; drop</p>
      <p style="margin-top:5px;font-size:11px;color:#aaa">JPG, PNG, WebP</p>
    </div>
    <input type="file" id="file-input" accept="image/*" style="display:none" onchange="handleFile(this.files[0])">
  </div>
  <div class="card photo-controls" id="photo-controls"><h2>Adjust</h2>
    <div class="fit-toggle">
      <div class="fit-btn active" id="fit-fill" onclick="setFit('fill')">Fill</div>
      <div class="fit-btn" id="fit-fit" onclick="setFit('fit')">Fit</div>
      <div class="fit-btn" id="fit-stretch" onclick="setFit('stretch')">Stretch</div>
    </div>
    <div class="form-row" id="crop-x-row" style="display:none"><label>Horizontal</label><input type="range" id="crop-x" min="0" max="100" value="50" oninput="updatePreview()"></div>
    <div class="form-row" id="crop-y-row"><label>Vertical position</label><input type="range" id="crop-y" min="0" max="100" value="50" oninput="updatePreview()"></div>
    <div class="form-row"><label>Rotation</label>
      <div class="rotation-row">
        <div class="rot-btn active" id="rot-0" onclick="setRotation(0)">0°</div>
        <div class="rot-btn" id="rot-90" onclick="setRotation(90)">↻ 90°</div>
        <div class="rot-btn" id="rot-180" onclick="setRotation(180)">180°</div>
        <div class="rot-btn" id="rot-270" onclick="setRotation(270)">↺ 90°</div>
      </div>
    </div>
    <div class="preview-row">
      <div class="preview-col"><label>Original</label><canvas id="orig-canvas" width="120" height="180"></canvas></div>
      <div class="preview-col"><label>ACeP dithered</label><canvas id="dith-canvas" width="120" height="180"></canvas></div>
    </div>
    <div class="btn-row">
      <button class="btn btn-success" id="send-btn" onclick="sendImage()">&#8679; Send to Display</button>
      <button class="btn btn-secondary" onclick="resetPhoto()">&#10005; Cancel</button>
    </div>
    <div id="send-progress" style="display:none;margin-top:10px;font-size:12px;color:#555;text-align:center">
      Sending — please wait (~30s for display refresh)
    </div>
  </div>
</div>

<!-- LEDS -->
<div class="panel" id="tab-leds">
  <div class="card"><h2>LED Mode</h2>
    <div class="led-preview">
      <div class="led-dot" id="led-dot-1" style="background:#fff"></div>
      <div class="led-dot" id="led-dot-2" style="background:#fff"></div>
      <span style="font-size:12px;color:#888;margin-left:6px" id="led-mode-label">Loading...</span>
    </div>
    <div class="led-mode-grid">
      <div class="led-mode-btn" id="lm-0" onclick="setLedMode(0)"><span class="mode-icon">&#9898;</span>Status</div>
      <div class="led-mode-btn" id="lm-1" onclick="setLedMode(1)"><span class="mode-icon">&#9711;</span>Off</div>
      <div class="led-mode-btn" id="lm-2" onclick="setLedMode(2)"><span class="mode-icon">&#11088;</span>Solid</div>
      <div class="led-mode-btn" id="lm-3" onclick="setLedMode(3)"><span class="mode-icon">&#128165;</span>Pulse</div>
      <div class="led-mode-btn" id="lm-4" onclick="setLedMode(4)"><span class="mode-icon">&#9889;</span>Flash</div>
      <div class="led-mode-btn" id="lm-5" onclick="setLedMode(5)"><span class="mode-icon">&#127752;</span>Rainbow</div>
    </div>
  </div>
  <div class="card" id="led-colour-card"><h2>Colour</h2>
    <div class="colour-presets">
      <div class="colour-preset" style="background:#fff;border:2px solid #ddd" onclick="setLedColour(255,255,255)"></div>
      <div class="colour-preset" style="background:#ff4444" onclick="setLedColour(255,68,68)"></div>
      <div class="colour-preset" style="background:#ff8800" onclick="setLedColour(255,136,0)"></div>
      <div class="colour-preset" style="background:#ffdd00" onclick="setLedColour(255,221,0)"></div>
      <div class="colour-preset" style="background:#44cc44" onclick="setLedColour(68,204,68)"></div>
      <div class="colour-preset" style="background:#1155cc" onclick="setLedColour(17,85,204)"></div>
      <div class="colour-preset" style="background:#8844cc" onclick="setLedColour(136,68,204)"></div>
      <div class="colour-preset" style="background:#ff44aa" onclick="setLedColour(255,68,170)"></div>
      <div class="colour-preset" style="background:#00ddff" onclick="setLedColour(0,221,255)"></div>
      <div class="colour-preset" style="background:#ffaacc" onclick="setLedColour(255,170,204)"></div>
    </div>
    <div style="display:flex;align-items:center;gap:10px">
      <label style="font-size:12px;font-weight:500;color:#444">Custom</label>
      <input type="color" id="led-colour-picker" value="#ffffff" oninput="colourPickerChange(this.value)">
      <span style="font-size:12px;color:#888" id="led-hex">#ffffff</span>
    </div>
  </div>
  <div class="card"><h2>Brightness</h2>
    <div style="display:flex;align-items:center;gap:10px">
      <span style="font-size:12px;color:#888">0</span>
      <input type="range" id="led-brightness" min="0" max="255" value="80" oninput="setBrightness(this.value)">
      <span style="font-size:12px;color:#888">255</span>
    </div>
    <div style="text-align:center;font-size:12px;color:#888;margin-top:4px" id="brightness-label">80</div>
  </div>
</div>

<div class="toast" id="toast"></div>

<script>
// ── State ──
let schedDirty=false,sourceImage=null,fitMode='fill',rotDeg=0,lastDithered=null;
let ledMode=0,ledR=255,ledG=255,ledB=255,ledBri=80;
let slideshowActive=false,slideInterval=300,slideIndex=0,photoCount=0;
const DW=400,DH=600;
const PALETTE=[[204,34,0],[204,137,0],[26,107,42],[17,85,204],[26,26,26],[255,255,255]];
const LED_MODE_NAMES=['Status','Off','Solid','Pulse','Flash','Rainbow'];
let sensorData=[];

// ── Tabs ──
function showTab(name){
  const names=['dashboard','sensors','schedule','settings','photo','leds'];
  document.querySelectorAll('.tab').forEach((t,i)=>t.classList.toggle('active',names[i]===name));
  document.querySelectorAll('.panel').forEach(p=>p.classList.toggle('active',p.id==='tab-'+name));
  if(name==='sensors')loadSensorHistory();
  if(name==='photo')loadPhotoLibrary();
}

// ── Toast ──
function toast(msg,ok=true){const t=document.getElementById('toast');t.textContent=msg;t.style.background=ok?'#1a6b2a':'#cc2200';t.classList.add('show');setTimeout(()=>t.classList.remove('show'),3000);}

// ── Status ──
function rssiLabel(r){return r>=-50?'Excellent':r>=-65?'Good':r>=-75?'Fair':'Weak';}
async function fetchStatus(){
  try{
    const d=await(await fetch('/api/status')).json();
    document.getElementById('temp').innerHTML=d.temp_c.toFixed(1)+'<span class="unit">°C</span>';
    document.getElementById('hum').innerHTML=d.humidity.toFixed(0)+'<span class="unit">%</span>';
    // Also update sensors tab latest values if visible
    const st=document.getElementById('s-temp');
    const sh=document.getElementById('s-hum');
    if(st&&!st.dataset.manual){st.innerHTML=d.temp_c.toFixed(1)+'<span class="unit">°C</span>';}
    if(sh&&!sh.dataset.manual){sh.innerHTML=d.humidity.toFixed(0)+'<span class="unit">%</span>';}
    document.getElementById('batt').innerHTML=d.battery+'<span class="unit">%</span>';
    document.getElementById('batt_mv').innerHTML=d.batt_mv+'<span class="unit">mV</span>';
    const s=d.charging?'<span class="badge badge-green">&#9889; Charging</span>':d.usb_in?'<span class="badge badge-blue">USB</span>':'<span class="badge badge-grey">On battery</span>';
    document.getElementById('batt_status').innerHTML=s;
    document.getElementById('rssi').innerHTML=d.rssi+'<span class="unit">dBm</span>';
    document.getElementById('rssi_bar').textContent=rssiLabel(d.rssi);
    document.getElementById('heap').textContent=(d.heap/1024).toFixed(0)+' KB';
    const pn=['','Dashboard','Hourly Graph','Calendar','Photo'];
    document.getElementById('updated').textContent=d.updated;
    document.getElementById('curpage').textContent=pn[d.page]||d.page;
    document.getElementById('uptime').textContent=formatUptime(d.uptime);
    document.getElementById('ip').textContent=d.ip;
    document.getElementById('psram').textContent=(d.psram/1024).toFixed(0)+' KB';
    document.getElementById('ip2').textContent=d.ip;
    // SD status
    const sdEl=document.getElementById('sd-status');
    sdEl.innerHTML=d.sdReady?'<span class="sd-badge sd-ok">&#9679; Ready ('+d.photoCount+' photos)</span>':'<span class="sd-badge sd-no">&#9679; Not ready</span>';
    document.getElementById('photo-banner').classList.toggle('show',d.photoMode||d.slideshow);
    if(!schedDirty){
      document.getElementById('wakeStart').value=d.wakeStart;
      document.getElementById('wakeEnd').value=d.wakeEnd;
      document.getElementById('wakeInterval').value=d.wakeInterval;
      updateSchedSummary(d.wakeStart,d.wakeEnd,d.wakeInterval);
    }
    document.querySelectorAll('.page-btn').forEach((b,i)=>b.classList.toggle('active',i+1===d.page));
    // Slideshow state
    slideshowActive=d.slideshow;slideInterval=d.slideInterval;slideIndex=d.slideIndex;photoCount=d.photoCount;
    updateSlideshowUi();
    // LED
    ledMode=d.ledMode;ledR=d.ledR;ledG=d.ledG;ledB=d.ledB;ledBri=d.ledBrightness;
    applyLedUi();
  }catch(e){console.error(e);}
}
function formatUptime(s){const h=Math.floor(s/3600),m=Math.floor((s%3600)/60);return h?h+'h '+m+'m':m+'m';}

// ── Schedule ──
function updateSchedSummary(s,e,i){const slots=[];for(let h=+s;h<=+e;h+=+i)slots.push(h+':00');document.getElementById('sched-summary').textContent='Active '+s+':00–'+e+':00, every '+i+'h → '+slots.length+' wakes/day';}
document.querySelectorAll('#wakeStart,#wakeEnd,#wakeInterval').forEach(el=>{el.addEventListener('input',()=>{schedDirty=true;document.getElementById('dirty-notice').classList.add('show');el.classList.add('dirty');updateSchedSummary(document.getElementById('wakeStart').value,document.getElementById('wakeEnd').value,document.getElementById('wakeInterval').value);});});
async function saveSchedule(){
  const b={start:+document.getElementById('wakeStart').value,end:+document.getElementById('wakeEnd').value,interval:+document.getElementById('wakeInterval').value};
  if(b.start>=b.end){toast('Start must be before end',false);return;}
  const r=await fetch('/api/schedule',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(b)});
  if(r.ok){schedDirty=false;document.getElementById('dirty-notice').classList.remove('show');document.querySelectorAll('#wakeStart,#wakeEnd,#wakeInterval').forEach(el=>el.classList.remove('dirty'));toast('Schedule saved');}
  else toast('Failed',false);
}

// ── Controls ──
async function doRefresh(){toast('Refreshing...');await fetch('/api/refresh',{method:'POST'});fetchStatus();loadPreview();}
async function doSleep(){if(!confirm('Sleep now?'))return;await fetch('/api/sleep',{method:'POST'});toast('Device sleeping');}

// ── Settings ──
async function setPage(n){
  document.getElementById('preview-loading').textContent='Rendering...';
  document.getElementById('preview-loading').style.display='block';
  document.getElementById('preview-img').style.display='none';
  const r=await fetch('/api/page',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({page:n})});
  if(r.ok){document.querySelectorAll('.page-btn').forEach((b,i)=>b.classList.toggle('active',i+1===n));toast('Page '+n+' set');loadPreview();}
  else toast('Failed',false);
}
function loadPreview(){
  const img=document.getElementById('preview-img'),loading=document.getElementById('preview-loading');
  loading.textContent='Loading...';loading.style.display='block';img.style.display='none';
  const url='/api/screenshot?'+Date.now();
  const tmp=new Image();
  tmp.onload=()=>{img.src=url;img.style.display='block';loading.style.display='none';};
  tmp.onerror=()=>{loading.textContent='Preview unavailable';};
  tmp.src=url;
}

// ── Sensor chart ──
let activeHours = 8;  // default to last 8 hours

function setTimeRange(hours){
  activeHours = hours;
  // Update chip active state
  document.querySelectorAll('.time-chip').forEach(c => c.classList.remove('active'));
  const chipMap = {1:'chip-1h', 8:'chip-8h', 24:'chip-24h', 168:'chip-7d', 720:'chip-30d', 2160:'chip-90d'};
  const el = document.getElementById(chipMap[hours]);
  if(el) el.classList.add('active');
  // Redraw charts with filtered data (no refetch needed)
  redrawCharts();
}

function filterByHours(data, hours){
  if(!data.length) return data;
  // Parse last timestamp as reference point
  const last = data[data.length-1];
  const lastTs = last.ts || '';
  if(!lastTs) return data;
  // ISO format: "2026-06-08T13:34"
  const lastDate = new Date(lastTs.replace('T',' '));
  const cutoff = new Date(lastDate.getTime() - hours * 3600 * 1000);
  return data.filter(d => {
    const ts = (d.ts||'').replace('T',' ');
    return new Date(ts) >= cutoff;
  });
}

function redrawCharts(){
  const filtered = filterByHours(sensorData, activeHours);
  const count = filtered.length;
  const total = sensorData.length;
  document.getElementById('sensor-count').textContent =
    count + ' of ' + total + ' reading' + (total!==1?'s':'') + ' shown';
  drawChart('temp-chart', filtered, 't',   '°C', '#cc2200', 10,  40);
  drawChart('hum-chart',  filtered, 'h',   '%',  '#1155cc', 30, 100);
  drawChart('batt-chart', filtered, 'pct', '%',  '#1a6b2a',  0, 105);
}

async function loadSensorHistory(){
  try{
    const data = await(await fetch('/api/sensors/history')).json();
    sensorData = data;
    redrawCharts();
    // Populate latest values from last entry
    if(data.length > 0){
      const last = data[data.length-1];
      if(last.t  !== undefined) document.getElementById('s-temp').innerHTML = parseFloat(last.t).toFixed(1)+'<span class="unit">°C</span>';
      if(last.h  !== undefined) document.getElementById('s-hum').innerHTML  = parseFloat(last.h).toFixed(0)+'<span class="unit">%</span>';
    }
  }catch(e){ document.getElementById('sensor-count').textContent = 'No sensor data available'; }
}

async function takeReading(){
  const btn=document.getElementById('reading-btn');
  const status=document.getElementById('reading-status');
  btn.disabled=true;
  status.textContent='Reading sensor...';
  try{
    const r=await fetch('/api/sensors/reading',{method:'POST'});
    if(r.ok){
      const d=await r.json();
      document.getElementById('s-temp').innerHTML=d.temp_c.toFixed(1)+'<span class="unit">°C</span>';
      document.getElementById('s-hum').innerHTML=d.humidity.toFixed(0)+'<span class="unit">%</span>';
      status.textContent='Reading taken at '+new Date().toLocaleTimeString()+' — charts updated';
      toast('Reading logged to SD');
      // Refresh full dataset then redraw with active time filter
      await loadSensorHistory();
    } else {
      status.textContent='Read failed';
      toast('Reading failed',false);
    }
  }catch(e){status.textContent='Error: '+e.message;toast('Error',false);}
  btn.disabled=false;
}

function drawChart(canvasId,data,key,unit,colour,yMin,yMax){
  const canvas=document.getElementById(canvasId);
  if(!canvas||!data.length)return;
  const ctx=canvas.getContext('2d');
  const W=canvas.offsetWidth||canvas.width,H=canvas.height||200;
  canvas.width=W;canvas.height=H;
  ctx.clearRect(0,0,W,H);
  const pad={t:24,r:10,b:28,l:36};
  const gW=W-pad.l-pad.r,gH=H-pad.t-pad.b;
  const range=yMax-yMin||1;
  // Grid lines
  ctx.strokeStyle='#f0f0f0';ctx.lineWidth=1;
  for(let i=0;i<=4;i++){
    const y=pad.t+gH*(1-i/4);
    ctx.beginPath();ctx.moveTo(pad.l,y);ctx.lineTo(pad.l+gW,y);ctx.stroke();
    ctx.fillStyle='#aaa';ctx.font='10px sans-serif';ctx.textAlign='right';
    ctx.fillText(Math.round(yMin+range*i/4),pad.l-4,y+3);
  }
  // Line
  ctx.strokeStyle=colour;ctx.lineWidth=2;ctx.beginPath();
  const step=gW/(data.length-1||1);
  let first=true;
  data.forEach((d,i)=>{
    const v=parseFloat(d[key]);if(isNaN(v))return;
    const x=pad.l+i*step;
    const y=pad.t+gH*(1-(v-yMin)/range);
    if(first){ctx.moveTo(x,y);first=false;}else ctx.lineTo(x,y);
  });
  ctx.stroke();
  // X labels — show up to 6 timestamps, time only (HH:MM), date on day change
  ctx.fillStyle='#aaa';ctx.font='9px sans-serif';
  const labelCount=Math.min(6,data.length);
  let lastDate='';
  for(let i=0;i<labelCount;i++){
    const idx=Math.floor(i*(data.length-1)/(labelCount-1||1));
    const ts=data[idx].ts||'';
    // ts format: "2026-06-08T13:34" — extract date and time parts
    const tPart = ts.length>=16 ? ts.slice(11,16) : ts;  // HH:MM
    const dPart = ts.length>=10 ? ts.slice(5,10)  : '';  // MM-DD
    // Show date prefix if day changed
    const label = (dPart && dPart !== lastDate) ? dPart+' '+tPart : tPart;
    if (dPart) lastDate = dPart;
    const x = pad.l + idx*step;
    // Clamp text alignment to avoid clipping at edges
    if(i===0) ctx.textAlign='left';
    else if(i===labelCount-1) ctx.textAlign='right';
    else ctx.textAlign='center';
    ctx.fillText(label, x, H-4);
  }
}

// ── Slideshow ──
function updateSlideshowUi(){
  const tog=document.getElementById('slide-toggle');
  const startBtn=document.getElementById('start-ss-btn');
  const stopBtn=document.getElementById('stop-ss-btn');
  tog.classList.toggle('on',slideshowActive);
  document.getElementById('slide-label').textContent=
    slideshowActive ? 'Active — cycling '+photoCount+' photos' : 'Off';
  document.getElementById('slide-info').textContent=
    slideshowActive
      ? 'Every '+formatInterval(slideInterval)+', photo '+(slideIndex+1)+' of '+photoCount
      : photoCount>0 ? photoCount+' photos on SD — press Start to begin' : 'No photos on SD yet';
  document.getElementById('slide-interval').value=slideInterval;
  if(startBtn) startBtn.style.display = slideshowActive ? 'none' : 'inline-flex';
  if(stopBtn)  stopBtn.style.display  = slideshowActive ? 'inline-flex' : 'none';
  document.getElementById('ss-status').textContent='';
}
function formatInterval(s){if(s<60)return s+'s';if(s<3600)return(s/60)+'m';return(s/3600)+'h';}

async function startSlideshow(){
  if(photoCount===0){toast('No photos on SD — upload some first',false);return;}
  const btn=document.getElementById('start-ss-btn');
  btn.disabled=true;
  document.getElementById('ss-status').textContent='Starting — rendering first slide (~30s)...';
  const interval=+document.getElementById('slide-interval').value;
  const r=await fetch('/api/slideshow',{method:'POST',
    headers:{'Content-Type':'application/json'},
    body:JSON.stringify({active:true,interval:interval,index:0})});
  if(r.ok){
    slideshowActive=true; slideInterval=interval; slideIndex=0;
    updateSlideshowUi();
    toast('Slideshow started');
    document.getElementById('ss-status').textContent='';
  } else {
    toast('Failed to start slideshow',false);
    document.getElementById('ss-status').textContent='';
  }
  btn.disabled=false;
}

async function stopSlideshow(){
  const r=await fetch('/api/slideshow',{method:'POST',
    headers:{'Content-Type':'application/json'},
    body:JSON.stringify({active:false,interval:slideInterval})});
  if(r.ok){slideshowActive=false;updateSlideshowUi();toast('Slideshow stopped');}
  else toast('Failed',false);
}

async function toggleSlideshow(){
  if(slideshowActive) await stopSlideshow();
  else await startSlideshow();
}

async function setSlideInterval(v){
  slideInterval=+v;
  if(slideshowActive){
    await fetch('/api/slideshow',{method:'POST',
      headers:{'Content-Type':'application/json'},
      body:JSON.stringify({active:true,interval:slideInterval})});
  }
}

// ── Photo library ──
async function loadPhotoLibrary(){
  try{
    const photos=await(await fetch('/api/photos')).json();
    const grid=document.getElementById('photo-grid');
    const none=document.getElementById('no-photos');
    const badge=document.getElementById('photo-count-badge');
    badge.textContent='('+photos.length+')';
    if(!photos.length){grid.innerHTML='';none.style.display='block';return;}
    none.style.display='none';
    grid.innerHTML=photos.map(f=>`
      <div class="photo-item">
        <img src="/api/photos/view?file=${encodeURIComponent(f)}&t=${Date.now()}" 
             style="width:100%;height:100%;object-fit:cover"
             onerror="this.style.display='none';this.nextElementSibling.style.display='flex'"
             alt="${f}">
        <div style="display:none;background:#e0e0e0;width:100%;height:100%;align-items:center;justify-content:center;font-size:9px;color:#888;text-align:center;padding:4px">${f}</div>
        <button class="disp-btn" title="Display on device" onclick="displayPhoto('${f}')">&#9654;</button>
        <button class="del-btn" title="Delete" onclick="deletePhoto('${f}')">&#10005;</button>
        <div class="photo-name">${f}</div>
      </div>`).join('');
  }catch(e){console.error(e);}
}

async function deletePhoto(fname){
  if(!confirm('Delete '+fname+'?'))return;
  const r=await fetch('/api/photos/delete',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({filename:fname})});
  if(r.ok){toast('Deleted '+fname);loadPhotoLibrary();}
  else toast('Delete failed',false);
}

async function displayPhoto(fname){
  toast('Sending to display — please wait...');
  const r=await fetch('/api/photos/display',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({filename:fname})});
  if(r.ok){
    toast('Displaying '+fname);
    document.getElementById('photo-banner').classList.add('show');
    fetchStatus();
  } else toast('Failed to display photo',false);
}

// ── Photo upload ──
function handleDrop(e){e.preventDefault();document.getElementById('drop-zone').classList.remove('drag-over');const f=e.dataTransfer.files[0];if(f&&f.type.startsWith('image/'))handleFile(f);}
function handleFile(file){if(!file)return;const r=new FileReader();r.onload=e=>{const img=new Image();img.onload=()=>{sourceImage=img;rotDeg=0;updateRotBtns();updatePreview();document.getElementById('photo-controls').classList.add('show');};img.src=e.target.result;};r.readAsDataURL(file);}
function setFit(mode){fitMode=mode;['fill','fit','stretch'].forEach(m=>document.getElementById('fit-'+m).classList.toggle('active',m===mode));updatePreview();}
function setRotation(deg){rotDeg=deg;updateRotBtns();updatePreview();}
function updateRotBtns(){[0,90,180,270].forEach(d=>document.getElementById('rot-'+d).classList.toggle('active',d===rotDeg));}

function getScaledImageData(){
  if(!sourceImage)return null;
  const offscreen=document.createElement('canvas');offscreen.width=DW;offscreen.height=DH;
  const ctx=offscreen.getContext('2d');ctx.fillStyle='#fff';ctx.fillRect(0,0,DW,DH);
  const rad=rotDeg*Math.PI/180;
  const sw=sourceImage.width,sh=sourceImage.height;
  const rotW=(rotDeg%180===0)?sw:sh,rotH=(rotDeg%180===0)?sh:sw;
  const rot=document.createElement('canvas');rot.width=rotW;rot.height=rotH;
  const rc=rot.getContext('2d');rc.translate(rotW/2,rotH/2);rc.rotate(rad);rc.drawImage(sourceImage,-sw/2,-sh/2);
  const dAspect=DW/DH,sAspect=rotW/rotH;
  if(fitMode==='stretch'){ctx.drawImage(rot,0,0,DW,DH);}
  else if(fitMode==='fit'){let dw,dh;if(sAspect>dAspect){dw=DW;dh=DW/sAspect;}else{dh=DH;dw=DH*sAspect;}ctx.drawImage(rot,(DW-dw)/2,(DH-dh)/2,dw,dh);}
  else{const cx=+document.getElementById('crop-x').value/100,cy=+document.getElementById('crop-y').value/100;let srcW,srcH,srcX,srcY;if(sAspect>dAspect){srcH=rotH;srcW=rotH*dAspect;srcX=(rotW-srcW)*cx;srcY=0;}else{srcW=rotW;srcH=rotW/dAspect;srcX=0;srcY=(rotH-srcH)*cy;}ctx.drawImage(rot,srcX,srcY,srcW,srcH,0,0,DW,DH);}
  const isFill=fitMode==='fill',isWide=sAspect>dAspect;
  document.getElementById('crop-x-row').style.display=(isFill&&isWide)?'block':'none';
  document.getElementById('crop-y-row').style.display=(isFill&&!isWide)?'block':'none';
  return ctx.getImageData(0,0,DW,DH);
}

function nearestPalette(r,g,b){let best=0,bd=Infinity;for(let i=0;i<PALETTE.length;i++){const[pr,pg,pb]=PALETTE[i],d=(r-pr)**2+(g-pg)**2+(b-pb)**2;if(d<bd){bd=d;best=i;}}return best;}
function ditherImage(src){
  const w=DW,h=DH,r=new Float32Array(w*h),g=new Float32Array(w*h),b=new Float32Array(w*h);
  for(let i=0;i<w*h;i++){r[i]=src.data[i*4];g[i]=src.data[i*4+1];b[i]=src.data[i*4+2];}
  const result=new Uint8Array(w*h);
  for(let y=0;y<h;y++){for(let x=0;x<w;x++){
    const i=y*w+x,pr=Math.max(0,Math.min(255,r[i])),pg=Math.max(0,Math.min(255,g[i])),pb=Math.max(0,Math.min(255,b[i]));
    const ci=nearestPalette(pr,pg,pb);result[i]=ci;
    const[cr,cg,cb]=PALETTE[ci],er=pr-cr,eg=pg-cg,eb=pb-cb;
    if(x+1<w){r[i+1]+=er*7/16;g[i+1]+=eg*7/16;b[i+1]+=eb*7/16;}
    if(y+1<h){if(x>0){r[i+w-1]+=er*3/16;g[i+w-1]+=eg*3/16;b[i+w-1]+=eb*3/16;}r[i+w]+=er*5/16;g[i+w]+=eg*5/16;b[i+w]+=eb*5/16;if(x+1<w){r[i+w+1]+=er*1/16;g[i+w+1]+=eg*1/16;b[i+w+1]+=eb*1/16;}}
  }}return result;
}

function updatePreview(){
  if(!sourceImage)return;const PW=120,PH=180;
  const imgData=getScaledImageData();
  const orig=document.createElement('canvas');orig.width=DW;orig.height=DH;orig.getContext('2d').putImageData(imgData,0,0);
  document.getElementById('orig-canvas').getContext('2d').drawImage(orig,0,0,PW,PH);
  const dithered=ditherImage(imgData);lastDithered=dithered;
  const dd=new ImageData(DW,DH);for(let i=0;i<DW*DH;i++){const[pr,pg,pb]=PALETTE[dithered[i]];dd.data[i*4]=pr;dd.data[i*4+1]=pg;dd.data[i*4+2]=pb;dd.data[i*4+3]=255;}
  const dc=document.createElement('canvas');dc.width=DW;dc.height=DH;dc.getContext('2d').putImageData(dd,0,0);
  document.getElementById('dith-canvas').getContext('2d').drawImage(dc,0,0,PW,PH);
}

async function sendImage(){
  if(!lastDithered){toast('No image',false);return;}
  document.getElementById('send-btn').disabled=true;document.getElementById('send-progress').style.display='block';
  toast('Sending...');
  try{
    const form=new FormData();
    form.append('image',new Blob([new Uint8Array(lastDithered.buffer)],{type:'application/octet-stream'}),'display.bin');
    const r=await fetch('/api/image',{method:'POST',body:form});
    if(r.ok){toast('Image sent — saved to SD');document.getElementById('photo-banner').classList.add('show');fetchStatus();loadPhotoLibrary();}
    else toast('Send failed',false);
  }catch(e){toast('Error: '+e.message,false);}
  document.getElementById('send-btn').disabled=false;document.getElementById('send-progress').style.display='none';
}
function resetPhoto(){sourceImage=null;lastDithered=null;document.getElementById('photo-controls').classList.remove('show');document.getElementById('file-input').value='';}

// ── LEDs ──
function rgbToHex(r,g,b){return'#'+[r,g,b].map(v=>v.toString(16).padStart(2,'0')).join('');}
function hexToRgb(h){return{r:parseInt(h.slice(1,3),16),g:parseInt(h.slice(3,5),16),b:parseInt(h.slice(5,7),16)};}
function applyLedUi(){
  document.querySelectorAll('.led-mode-btn').forEach((b,i)=>b.classList.toggle('active',i===ledMode));
  document.getElementById('led-mode-label').textContent=LED_MODE_NAMES[ledMode]||'?';
  const showC=(ledMode===2||ledMode===3||ledMode===4);
  document.getElementById('led-colour-card').style.display=showC?'block':'none';
  const hex=rgbToHex(ledR,ledG,ledB);
  document.getElementById('led-colour-picker').value=hex;
  document.getElementById('led-hex').textContent=hex;
  document.getElementById('led-brightness').value=ledBri;
  document.getElementById('brightness-label').textContent=ledBri;
  document.getElementById('quick-brightness').value=ledBri;
  const dotCol=showC?`rgb(${ledR},${ledG},${ledB})`:ledMode===0?'#aaa':ledMode===5?'#ff44aa':'#333';
  document.getElementById('led-dot-1').style.background=dotCol;
  document.getElementById('led-dot-2').style.background=ledMode===5?'#44ccff':dotCol;
  document.getElementById('quick-led-dot').style.background=dotCol;
  document.getElementById('quick-led-label').textContent='Mode: '+LED_MODE_NAMES[ledMode];
}
async function sendLed(o={}){await fetch('/api/leds',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(Object.assign({mode:ledMode,r:ledR,g:ledG,b:ledB,brightness:ledBri},o))});}
async function setLedMode(m){ledMode=m;applyLedUi();await sendLed();}
async function setLedColour(r,g,b){ledR=r;ledG=g;ledB=b;applyLedUi();await sendLed();}
function colourPickerChange(hex){const{r,g,b}=hexToRgb(hex);ledR=r;ledG=g;ledB=b;document.getElementById('led-hex').textContent=hex;}
document.getElementById('led-colour-picker').addEventListener('change',async()=>{const{r,g,b}=hexToRgb(document.getElementById('led-colour-picker').value);await setLedColour(r,g,b);});
async function setBrightness(v){ledBri=+v;document.getElementById('brightness-label').textContent=v;document.getElementById('quick-brightness').value=v;await sendLed();}
async function quickBrightness(v){ledBri=+v;document.getElementById('led-brightness').value=v;document.getElementById('brightness-label').textContent=v;await sendLed();}

// ── Init ──
fetchStatus();
setInterval(fetchStatus,5000);
</script>
</body>
</html>
)HTMLEOF";





// ============================================================
// WEB SERVER — ROUTE HANDLERS
// ============================================================
void handleRoot()
{
    server.send_P(200, "text/html", WEB_HTML);
}

void handleStatus()
{
    StaticJsonDocument<512> doc;
    doc["temp_c"]       = gSensor.temp_c;
    doc["humidity"]     = gSensor.humidity;
    doc["battery"]      = M5.Power.getBatteryLevel();
    // Battery detail via M5Unified Power class
    int batt_mv  = M5.Power.getBatteryVoltage();
    int vbus_mv  = M5.Power.getVBUSVoltage();
    bool usb_in  = (vbus_mv > 1000);
    doc["batt_mv"]      = batt_mv;
    doc["usb_in"]       = usb_in;
    doc["charging"]     = (usb_in && M5.Power.getBatteryLevel() < 100);
    doc["rssi"]         = WiFi.RSSI();
    doc["page"]         = gCurrentPage;
    doc["photoMode"]    = gPhotoMode;
    doc["pageCount"]    = 4;
    doc["sdReady"]      = gSdReady;
    doc["photoCount"]   = gPhotoCount;
    doc["slideshow"]    = gSlideshowMode;
    doc["slideInterval"]= gSlideInterval;
    doc["slideIndex"]   = gSlideIndex;
    doc["ledMode"]      = (int)gLedMode;
    doc["ledR"]         = gLedR;
    doc["ledG"]         = gLedG;
    doc["ledB"]         = gLedB;
    doc["ledBrightness"]= gLedBrightness;
    doc["wakeStart"]    = gWakeStart;
    doc["wakeEnd"]      = gWakeEnd;
    doc["wakeInterval"] = gWakeInterval;
    doc["heap"]         = ESP.getFreeHeap();
    doc["psram"]        = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    doc["ip"]           = WiFi.localIP().toString();
    doc["uptime"]       = millis() / 1000;

    char timebuf[20];
    snprintf(timebuf, sizeof(timebuf), "%02d:%02d", gRtc.hour, gRtc.min);
    doc["updated"] = timebuf;

    String out;
    serializeJson(doc, out);
    server.send(200, "application/json", out);
}

void handleRefresh()
{
    if (gWifiOk) {
        ledFetching();
        fetchWeather();
    }
    readSensor();
    readRTC();
    ledRendering();
    renderCurrentPage();
    ledWebMode();
    // Respond after render is complete
    server.send(200, "application/json", "{\"ok\":true}");
}

void handlePage()
{
    if (!server.hasArg("plain")) { server.send(400, "application/json", "{\"error\":\"no body\"}"); return; }
    StaticJsonDocument<64> doc;
    if (deserializeJson(doc, server.arg("plain"))) { server.send(400, "application/json", "{\"error\":\"bad json\"}"); return; }
    int pg = doc["page"] | gCurrentPage;
    if (pg < 1 || pg > 4) { server.send(400, "application/json", "{\"error\":\"invalid page\"}"); return; }
    gCurrentPage = pg;
    savePrefs();
    // Render first — canvas is complete before we respond
    // screenshot fetch can follow immediately after 200
    ledRendering();
    renderCurrentPage();
    ledWebMode();
    server.send(200, "application/json", "{\"ok\":true}");
}

void handleSchedule()
{
    if (!server.hasArg("plain")) { server.send(400, "application/json", "{\"error\":\"no body\"}"); return; }
    StaticJsonDocument<128> doc;
    if (deserializeJson(doc, server.arg("plain"))) { server.send(400, "application/json", "{\"error\":\"bad json\"}"); return; }
    int s = doc["start"]    | gWakeStart;
    int e = doc["end"]      | gWakeEnd;
    int i = doc["interval"] | gWakeInterval;
    if (s < 0 || s > 22 || e < 1 || e > 23 || e <= s || i < 1 || i > 8) {
        server.send(400, "application/json", "{\"error\":\"invalid schedule\"}");
        return;
    }
    gWakeStart    = s;
    gWakeEnd      = e;
    gWakeInterval = i;
    savePrefs();
    server.send(200, "application/json", "{\"ok\":true}");
}

void handleSleep()
{
    server.send(200, "application/json", "{\"ok\":true}");
    delay(200);
    stopWebServer();
    gWebMode = false;
    ledSleeping();
    goToSleep();
}

void handleScreenshot()
{
    // Serve canvas framebuffer as a 24-bit BMP
    const int W = PC_W, H = PC_H;
    const int rowBytes   = W * 3;
    const int padBytes   = (4 - (rowBytes % 4)) % 4;
    const int rowStride  = rowBytes + padBytes;
    const int pixelBytes = rowStride * H;
    const int fileSize   = 54 + pixelBytes;

    uint8_t hdr[54] = {0};
    hdr[0]='B'; hdr[1]='M';
    hdr[2]=fileSize&0xFF; hdr[3]=(fileSize>>8)&0xFF;
    hdr[4]=(fileSize>>16)&0xFF; hdr[5]=(fileSize>>24)&0xFF;
    hdr[10]=54;
    hdr[14]=40;
    hdr[18]=W&0xFF; hdr[19]=(W>>8)&0xFF;
    int negH = -H;
    hdr[22]=negH&0xFF; hdr[23]=(negH>>8)&0xFF;
    hdr[24]=(negH>>16)&0xFF; hdr[25]=(negH>>24)&0xFF;
    hdr[26]=1; hdr[28]=24;
    hdr[34]=pixelBytes&0xFF; hdr[35]=(pixelBytes>>8)&0xFF;
    hdr[36]=(pixelBytes>>16)&0xFF; hdr[37]=(pixelBytes>>24)&0xFF;

    server.setContentLength(fileSize);
    server.sendHeader("Content-Disposition", "inline; filename=\"display.bmp\"");
    server.send(200, "image/bmp", "");
    server.client().write((const char*)hdr, 54);

    uint8_t row[rowStride];
    memset(row, 0, sizeof(row));
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            uint16_t px = canvas.readPixel(x, y);
            uint8_t r = ((px >> 11) & 0x1F) << 3;
            uint8_t g = ((px >>  5) & 0x3F) << 2;
            uint8_t b = ( px        & 0x1F) << 3;
            row[x*3+0] = b;
            row[x*3+1] = g;
            row[x*3+2] = r;
        }
        server.client().write((const char*)row, rowStride);
    }
}

void handleLeds()
{
    if (!server.hasArg("plain")) { server.send(400, "application/json", "{\"error\":\"no body\"}"); return; }
    StaticJsonDocument<128> doc;
    if (deserializeJson(doc, server.arg("plain"))) { server.send(400, "application/json", "{\"error\":\"bad json\"}"); return; }

    // mode: 0=status 1=off 2=solid 3=pulse 4=flash 5=rainbow
    if (doc.containsKey("mode")) {
        int m = doc["mode"] | (int)gLedMode;
        if (m >= LED_STATUS && m <= LED_RAINBOW) gLedMode = (LedMode)m;
        if (gLedMode == LED_OFF) ledsOff();
    }
    if (doc.containsKey("r"))          gLedR          = doc["r"];
    if (doc.containsKey("g"))          gLedG          = doc["g"];
    if (doc.containsKey("b"))          gLedB          = doc["b"];
    if (doc.containsKey("brightness")) gLedBrightness = doc["brightness"];

    pixels.setBrightness(gLedBrightness);
    savePrefs();
    server.send(200, "application/json", "{\"ok\":true}");
    Serial.printf("   LED: mode=%d  rgb=(%d,%d,%d)  bri=%d\n",
                  (int)gLedMode, gLedR, gLedG, gLedB, gLedBrightness);
}

// ============================================================
// WEB SERVER START / STOP
// ============================================================
void startWebServer()
{
    server.on("/",              HTTP_GET,  handleRoot);
    server.on("/api/status",    HTTP_GET,  handleStatus);
    server.on("/api/screenshot",HTTP_GET,  handleScreenshot);
    server.on("/api/refresh",   HTTP_POST, handleRefresh);
    server.on("/api/page",      HTTP_POST, handlePage);
    server.on("/api/schedule",  HTTP_POST, handleSchedule);
    server.on("/api/leds",      HTTP_POST, handleLeds);
    // Image upload — use onNotFound fallback to handle large binary body manually
    server.on("/api/image",     HTTP_POST,
        // completion handler — called after body is fully received
        []() {
            if (gImageBuffer == nullptr) {
                server.send(500, "application/json", "{\"error\":\"PSRAM alloc failed\"}");
                return;
            }
            gCurrentPage   = 4;
            gPhotoMode     = true;
            gSlideshowMode = false;  // single photo upload exits slideshow
            // Save to SD as BMP
            if (gSdReady) {
                String path = nextPhotoPath();
                saveBmpToSD(path.c_str());
                loadPhotoList();
            }
            savePrefs();
            ledRendering();
            renderPage4();
            ledWebMode();
            server.send(200, "application/json", "{\"ok\":true}");
            Serial.println("   Image rendered on page 4, photo mode active");
        },
        // upload handler — called for each chunk of the body
        []() {
            HTTPUpload& upload = server.upload();
            if (upload.status == UPLOAD_FILE_START) {
                // Allocate PSRAM buffer on first chunk
                if (gImageBuffer == nullptr) {
                    gImageBuffer = (uint8_t*)heap_caps_malloc(IMG_SIZE, MALLOC_CAP_SPIRAM);
                    if (gImageBuffer == nullptr) {
                        Serial.println("   ERROR: PSRAM alloc failed");
                        return;
                    }
                    Serial.printf("   Image buffer allocated: %d bytes\n", IMG_SIZE);
                }
                Serial.println("   Image upload started");
            } else if (upload.status == UPLOAD_FILE_WRITE) {
                // Write chunk to PSRAM buffer
                static int bytesReceived = 0;
                if (upload.totalSize == 0) bytesReceived = 0;  // reset on new upload
                if (bytesReceived + (int)upload.currentSize <= IMG_SIZE) {
                    memcpy(gImageBuffer + bytesReceived, upload.buf, upload.currentSize);
                    bytesReceived += upload.currentSize;
                }
            } else if (upload.status == UPLOAD_FILE_END) {
                Serial.printf("   Image upload done: %d bytes\n", upload.totalSize);
            }
        }
    );
    server.on("/api/photos",          HTTP_GET,  handleGetPhotos);
    server.on("/api/photos/view",     HTTP_GET,  handleViewPhoto);
    server.on("/api/photos/display",  HTTP_POST, handleDisplayPhoto);
    server.on("/api/photos/delete",   HTTP_POST, handleDeletePhoto);
    server.on("/api/slideshow",       HTTP_POST, handleSlideshow);
    server.on("/api/sensors/history", HTTP_GET,  handleSensorHistory);
    server.on("/api/sensors/reading", HTTP_POST, handleTakeReading);
    server.on("/api/sleep",           HTTP_POST, handleSleep);
    server.begin();
    Serial.println("   Web server started on port 80");
}

void stopWebServer()
{
    server.stop();
    Serial.println("   Web server stopped");
}

void handleWebLoop()
{
    server.handleClient();
    updateLeds();
    M5.update();
    uint32_t now = millis();

    // Button C press to exit web mode
    if (M5.BtnC.wasPressed()) {
        Serial.println(">> BtnC — exiting web mode");
        stopWebServer();
        gWebMode = false;
        ledRendering();
        renderCurrentPage();
        ledsOff();
        gLastInteraction = millis();
    }

    delay(10);
}


// ============================================================
// WEATHER ICON ROUTER
// WeatherAPI condition codes: https://www.weatherapi.com/docs/
// ============================================================
void drawWeatherIcon(M5Canvas* c, int cx, int cy, int code, int w, int h)
{
    int r = min(w, h) / 2;
    if (code == 1000) {
        drawSunIcon(c, cx, cy, r);
    } else if (code == 1003) {
        drawPartlyCloudyIcon(c, cx, cy);
    } else if (code >= 1003 && code <= 1009) {
        drawCloudIcon(c, cx, cy, w, h, PC_GREY);
    } else if (code >= 1063 && code <= 1201) {
        drawRainIcon(c, cx, cy);
    } else if (code >= 1273 && code <= 1282) {
        drawStormIcon(c, cx, cy);
    } else if (code > 1009) {
        // Any other non-clear, non-cloud code — default to rain
        drawRainIcon(c, cx, cy);
    } else {
        // code 0 or unknown — cloud
        drawCloudIcon(c, cx, cy, w, h, PC_GREY);
    }
}

void drawSunIcon(M5Canvas* c, int cx, int cy, int r)
{
    c->fillCircle(cx, cy, r, PC_AMBER);
    int rOuter = r + 5;
    int rInner = r + 2;
    for (int i = 0; i < 8; i++) {
        float angle = i * PI / 4.0f;
        int x1 = cx + (int)(rInner * cosf(angle));
        int y1 = cy + (int)(rInner * sinf(angle));
        int x2 = cx + (int)(rOuter * cosf(angle));
        int y2 = cy + (int)(rOuter * sinf(angle));
        c->drawLine(x1, y1, x2, y2, PC_AMBER);
    }
}

void drawCloudIcon(M5Canvas* c, int cx, int cy, int w, int h, uint16_t col)
{
    int cw = w * 3 / 4;
    int ch = h / 2;
    c->fillRect(cx - cw / 2, cy - ch / 4, cw, ch, col);
    c->fillCircle(cx - cw / 4, cy - ch / 4, ch / 2, col);
    c->fillCircle(cx + cw / 6, cy - ch / 2, ch / 2 + 1, col);
}

void drawPartlyCloudyIcon(M5Canvas* c, int cx, int cy)
{
    c->fillCircle(cx - 6, cy - 4, 7, PC_AMBER);
    drawCloudIcon(c, cx + 4, cy + 4, 26, 18, PC_GREY);
}

void drawRainIcon(M5Canvas* c, int cx, int cy)
{
    drawCloudIcon(c, cx, cy - 4, 28, 16, PC_GREY);
    for (int i = -1; i <= 1; i++) {
        c->drawLine(cx + i * 7, cy + 6, cx + i * 7 - 2, cy + 12, PC_BLUE);
    }
}

void drawStormIcon(M5Canvas* c, int cx, int cy)
{
    drawCloudIcon(c, cx, cy - 4, 28, 16, PC_GREY);
    int bx = cx, by = cy + 5;
    c->drawLine(bx + 3, by,     bx - 1, by + 7,  PC_AMBER);
    c->drawLine(bx - 1, by + 7, bx + 2, by + 7,  PC_AMBER);
    c->drawLine(bx + 2, by + 7, bx - 3, by + 14, PC_AMBER);
}

// ============================================================
// COMPASS ARROW
// Draws a filled arrow pointing in the wind direction
// degrees: 0=N, 90=E, 180=S, 270=W
// ============================================================
void drawCompassArrow(M5Canvas* c, int cx, int cy, int r, int degrees)
{
    // Draw outer circle
    c->drawCircle(cx, cy, r, PC_GREY);
    // Convert degrees to radians — 0° is North (up), clockwise
    float rad = (degrees - 90) * PI / 180.0f;
    // Arrow tip
    int tx = cx + (int)((r - 2) * cosf(rad));
    int ty = cy + (int)((r - 2) * sinf(rad));
    // Arrow tail
    float radTail = rad + PI;
    int bx = cx + (int)((r - 4) * cosf(radTail));
    int by = cy + (int)((r - 4) * sinf(radTail));
    // Arrow head triangle — two wing points perpendicular to direction
    float radL = rad + PI * 0.75f;
    float radR = rad - PI * 0.75f;
    int lx = cx + (int)(5 * cosf(radL));
    int ly = cy + (int)(5 * sinf(radL));
    int rx = cx + (int)(5 * cosf(radR));
    int ry = cy + (int)(5 * sinf(radR));
    // Draw filled triangle tip
    c->fillTriangle(tx, ty, lx, ly, rx, ry, PC_BLACK);
    // Draw shaft
    c->drawLine(cx, cy, bx, by, PC_BLACK);
}

// ============================================================
// MOON PHASE ICON
// ============================================================
void drawMoonIcon(M5Canvas* c, int cx, int cy, int r, float illum, bool waxing)
{
    if (illum < 0) illum = 0;
    if (illum > 1) illum = 1;
    uint16_t darkCol  = PC_BLACK;
    uint16_t lightCol = 0xFFFF;
    int shadowRx = (int)(r * fabsf(1.0f - illum * 2.0f));
    if (illum < 0.5f) {
        c->fillCircle(cx, cy, r, darkCol);
        int offset = waxing ? (r / 3) : -(r / 3);
        if (shadowRx > 0) c->fillEllipse(cx + offset, cy, shadowRx, r, lightCol);
    } else {
        c->fillCircle(cx, cy, r, lightCol);
        int offset = waxing ? -(r / 3) : (r / 3);
        if (shadowRx > 0) c->fillEllipse(cx + offset, cy, shadowRx, r, darkCol);
    }
    // Thick outline — draw 3 concentric circles for bold border
    c->drawCircle(cx, cy, r,     PC_BLACK);
    c->drawCircle(cx, cy, r - 1, PC_BLACK);
    c->drawCircle(cx, cy, r + 1, PC_BLACK);
}

// ============================================================
// HELPERS
// ============================================================
const char* aqiLabel(int aqi)
{
    switch (aqi) {
        case 1:  return "Good";
        case 2:  return "Moderate";
        case 3:  return "Sensitive";
        case 4:  return "Unhealthy";
        case 5:  return "Very High";
        default: return "Hazardous";
    }
}

uint16_t aqiColour(int aqi)
{
    if (aqi <= 2) return PC_GREEN;   // Good / Moderate
    if (aqi == 3) return PC_YELLOW;  // Sensitive groups — yellow caution
    return PC_RED;                   // 4-6 Unhealthy+
}

const char* uvLabel(int uv)
{
    if (uv <= 2)  return "Low";
    if (uv <= 5)  return "Moderate";
    if (uv <= 7)  return "High";
    if (uv <= 10) return "Very High";
    return "Extreme";
}

// Returns "—" if WeatherAPI sends "Does not rise/set", otherwise passes through
const char* moonTimeDisplay(const char* t)
{
    if (t == nullptr || strlen(t) == 0) return "—";
    if (strncmp(t, "Does not", 8) == 0) return "—";
    if (strncmp(t, "No moon", 7) == 0)  return "—";
    return t;
}

uint16_t uvColour(int uv)
{
    // Blue (low) → Yellow (moderate/high) → Red (very high/extreme)
    // Using exact hardware RGB565 values
    if (uv <= 2)  return PC_BLUE;    // Low      — blue
    if (uv <= 7)  return PC_YELLOW;  // Moderate/High — yellow (readable at bold size)
    return PC_RED;                   // Very High/Extreme — red
}

const char* fullDayName(int dow)
{
    static const char* names[] = {
        "Sunday","Monday","Tuesday","Wednesday",
        "Thursday","Friday","Saturday"
    };
    if (dow < 0 || dow > 6) return "---";
    return names[dow];
}

const char* monthName(int m)
{
    static const char* names[] = {
        "", "January","February","March","April","May","June",
        "July","August","September","October","November","December"
    };
    if (m < 1 || m > 12) return "---";
    return names[m];
}
