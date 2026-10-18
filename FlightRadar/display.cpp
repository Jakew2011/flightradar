#include "display.h"
#include "settings.h"
#include <Arduino.h>

// Catch a wrong / missing TFT_eSPI set-up early with a readable message.
#ifndef ST7796_DRIVER
  #error "TFT_eSPI is not configured for the ST7796S. Copy extras/User_Setup.h over TFT_eSPI/User_Setup.h - see TFT_SETUP.md."
#endif
#if (TFT_WIDTH != 320) || (TFT_HEIGHT != 480)
  #error "TFT_eSPI User_Setup.h must define TFT_WIDTH 320 and TFT_HEIGHT 480 - see TFT_SETUP.md."
#endif

#ifndef ESP_ARDUINO_VERSION_MAJOR
  #define ESP_ARDUINO_VERSION_MAJOR 1
#endif

TFT_eSPI    tft = TFT_eSPI();
TFT_eSprite spHeader = TFT_eSprite(&tft);
TFT_eSprite spRadar  = TFT_eSprite(&tft);
TFT_eSprite spPanel  = TFT_eSprite(&tft);

static bool    s_spritesOk = false;
static bool    s_inPsram = false;
static uint8_t s_depth = 16;
static bool    s_blInit = false;
#define BL_CHANNEL 0

// ---------------------------------------------------------------------------
//  Themes
// ---------------------------------------------------------------------------
static const Theme THEMES[THEME_COUNT] = {
  // name    bg  grid dim outer      text textDim aircraft select military warn ok        sweep RGB
  { "GREEN", 0x0000, RGB565(0, 95, 35),   RGB565(0, 50, 20),   RGB565(0, 170, 70),
    RGB565(0, 255, 110), RGB565(0, 150, 70), RGB565(140, 255, 150), RGB565(255, 255, 255),
    RGB565(255, 80, 50), RGB565(255, 90, 60), RGB565(0, 255, 110), 0, 255, 90 },
  { "AMBER", 0x0000, RGB565(120, 78, 0),  RGB565(60, 38, 0),   RGB565(190, 125, 0),
    RGB565(255, 176, 0), RGB565(160, 105, 0), RGB565(255, 205, 80), RGB565(255, 255, 255),
    RGB565(255, 70, 50), RGB565(255, 90, 60), RGB565(255, 176, 0), 255, 170, 0 },
  { "WHITE", 0x0000, RGB565(100, 100, 100), RGB565(50, 50, 50), RGB565(170, 170, 170),
    RGB565(235, 235, 235), RGB565(140, 140, 140), RGB565(255, 255, 255), RGB565(0, 210, 255),
    RGB565(255, 80, 60), RGB565(255, 110, 80), RGB565(235, 235, 235), 220, 220, 220 },
};

const Theme& themeGet(uint8_t idx) { return THEMES[idx < THEME_COUNT ? idx : 0]; }
const Theme& themeCurrent() { return themeGet(g_settings.theme); }

// ---------------------------------------------------------------------------
//  Backlight
// ---------------------------------------------------------------------------
static void backlightInit() {
#if BACKLIGHT_PWM_ENABLED
  #if ESP_ARDUINO_VERSION_MAJOR >= 3
    ledcAttach(TFT_BLK_PIN, BACKLIGHT_PWM_FREQ_HZ, 8);
  #else
    ledcSetup(BL_CHANNEL, BACKLIGHT_PWM_FREQ_HZ, 8);
    ledcAttachPin(TFT_BLK_PIN, BL_CHANNEL);
  #endif
#else
  pinMode(TFT_BLK_PIN, OUTPUT);
#endif
  s_blInit = true;
}

void displaySetBrightness(uint8_t percent) {
  if (!s_blInit) backlightInit();
  if (percent < 5) percent = 5;
  if (percent > 100) percent = 100;
#if BACKLIGHT_PWM_ENABLED
  uint32_t duty = (uint32_t)percent * 255u / 100u;
  if (!BACKLIGHT_ACTIVE_HIGH) duty = 255u - duty;
  #if ESP_ARDUINO_VERSION_MAJOR >= 3
    ledcWrite(TFT_BLK_PIN, duty);
  #else
    ledcWrite(BL_CHANNEL, duty);
  #endif
#else
  digitalWrite(TFT_BLK_PIN, BACKLIGHT_ACTIVE_HIGH ? HIGH : LOW);
#endif
}

// ---------------------------------------------------------------------------
//  Init
// ---------------------------------------------------------------------------
static bool makeSprite(TFT_eSprite& s, int w, int h) {
  s.setColorDepth(16);
  if (s.createSprite(w, h) != nullptr) return true;
  s.setColorDepth(8);                       // fallback: half the memory
  if (s.createSprite(w, h) != nullptr) { s_depth = 8; return true; }
  return false;
}

bool displayInit() {
  displaySetBrightness(100);                // backlight on while starting
  tft.init();
  tft.setRotation(DISPLAY_ROTATION);
  tft.invertDisplay(DISPLAY_INVERT_COLOURS ? true : false);
  tft.fillScreen(TFT_BLACK);

  const size_t psramBefore = ESP.getFreePsram();
  s_spritesOk = makeSprite(spRadar, SCREEN_W, RADAR_H) &&
                makeSprite(spPanel, SCREEN_W, PANEL_H) &&
                makeSprite(spHeader, SCREEN_W, HEADER_H);
  const size_t psramAfter = ESP.getFreePsram();
  s_inPsram = (psramBefore > psramAfter) && (psramBefore - psramAfter) > 100000;

  Serial.printf("[display] %dx%d  sprites %s  depth=%u  in PSRAM: %s (PSRAM free %u)\n",
                tft.width(), tft.height(), s_spritesOk ? "OK" : "FAILED", s_depth,
                s_inPsram ? "yes" : "no", (unsigned)psramAfter);
  if (!s_spritesOk) {
    displayFatal("NOT ENOUGH MEMORY", "Enable PSRAM: OPI PSRAM");
  }
  return s_spritesOk;
}

bool displaySpritesOk() { return s_spritesOk; }
bool displaySpritesInPsram() { return s_inPsram; }
uint8_t displaySpriteDepth() { return s_depth; }

void displayFatal(const char* line1, const char* line2) {
  Serial.printf("[display] %s / %s\n", line1, line2 ? line2 : "");
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_RED, TFT_BLACK);
  tft.setTextDatum(MC_DATUM);
  tft.drawString(line1, tft.width() / 2, tft.height() / 2 - 12, 4);
  if (line2) {
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawString(line2, tft.width() / 2, tft.height() / 2 + 20, 2);
  }
}
