// display.h - TFT (ST7796S 320x480) setup, sprites, backlight and themes.
#pragma once
#include <TFT_eSPI.h>
#include "config.h"

// Portrait layout (pixels)
#define SCREEN_W   320
#define SCREEN_H   480
#define HEADER_H   24
#define RADAR_Y    24            // top of radar sprite on screen
#define RADAR_H    312           // radar sprite height (24..336)
#define PANEL_Y    336           // top of info panel
#define PANEL_H    144           // panel height (336..480)
#define RADAR_CX   160           // radar centre inside the radar sprite
#define RADAR_CY   156
#define RADAR_R    136           // radar circle radius (pixels)

#define RGB565(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | (((b) & 0xFF) >> 3)))

struct Theme {
  const char* name;
  uint16_t bg, grid, gridDim, ringOuter;
  uint16_t text, textDim, aircraft, select, military, warn, ok;
  uint8_t  sweepR, sweepG, sweepB;
};

#define THEME_COUNT 3
const Theme& themeGet(uint8_t idx);
const Theme& themeCurrent();          // theme chosen in the settings

extern TFT_eSPI   tft;
extern TFT_eSprite spHeader;          // 320 x 24
extern TFT_eSprite spRadar;           // 320 x 312
extern TFT_eSprite spPanel;           // 320 x 144

bool displayInit();                   // false if sprites could not be created
bool displaySpritesOk();
bool displaySpritesInPsram();
uint8_t displaySpriteDepth();
void displaySetBrightness(uint8_t percent);   // 5..100
void displayFatal(const char* line1, const char* line2);
