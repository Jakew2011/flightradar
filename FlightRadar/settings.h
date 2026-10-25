// settings.h - persistent settings (ESP32 internal flash via Preferences),
// unit formatting helpers and the Serial Monitor command console.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "config.h"

#define RANGE_COUNT 9
extern const uint16_t RANGES_NM[RANGE_COUNT];   // 5,10,20,30,50,75,100,150,200

struct Settings {
  char    wifiSsid[33];
  char    wifiPass[65];
  char    wifiHostname[33];
  char    portalSsid[33];
  char    portalPass[65];
  char    timezone[48];
  char    audioUrl[256];
  char    adsbCustomUrl[192];
  char    trainStationCrs[8];   // 3-letter National Rail CRS code, e.g. "LIT"
  char    trainApiKey[96];      // Rail Data Marketplace "Consumer key" - editable from the web
                                 // setup portal, so no source/reflash is needed to set it.
                                 // Real keys run close to 48 characters; this was originally
                                 // sized at 48 bytes total, which left zero room for the NUL
                                 // terminator on a 48-character key and silently truncated the
                                 // last character (or made the web form reject it outright as
                                 // "too long") - sized generously now so that can't recur.
  double  homeLat;
  double  homeLon;
  uint8_t homeConfigured;
  uint32_t serialBaud;
  uint32_t wifiConnectTimeoutMs;
  uint32_t wifiRetryMinMs;
  uint32_t wifiRetryMaxMs;
  uint32_t portalTimeoutMs;
  uint32_t audioMaxBytes;
  uint32_t audioFetchTimeoutMs;
  uint32_t audioWifiWaitMs;
  uint32_t announceCooldownMs;
  uint32_t adsbUpdateIntervalMs;
  uint32_t adsbMinRequestGapMs;
  uint32_t adsbConnectTimeoutMs;
  uint32_t adsbHttpTimeoutMs;
  uint32_t aircraftMaxAgeMs;
  uint32_t aircraftMaxExtrapolationMs;
  uint32_t aircraftBlendMs;
  uint32_t radarSweepPeriodMs;
  uint8_t  portalEnabled;
  uint8_t  portalAfterFails;
  uint8_t  adsbMode;       // 0 both, 1 adsb.lol, 2 adsb.fi, 3 custom
  uint8_t  showGroundAircraft;
  uint8_t  nightModeEnabled;
  uint8_t  nightStartHour;
  uint8_t  nightEndHour;
  uint8_t  nightBrightness;
  uint8_t  encoderReverse;
  uint8_t  encoderStepsPerDetent;
  uint16_t buttonDebounceMs;
  uint16_t longPressMs;
  uint8_t  audioEnabled;
  uint8_t  debugAircraftTypes;
  uint8_t  adsbValidateTls;
  uint8_t  radarMaxFps;
  uint8_t  sweepTrailSegments;
  uint16_t sweepTrailDegrees;
  uint8_t  labelsEnabled;
  uint8_t  labelMax;
  float    aircraftIconScale;
  uint8_t  rangeIndex;   // index into RANGES_NM
  uint8_t theme;        // 0 green, 1 amber, 2 white
  uint8_t brightness;   // 5..100 percent
  uint8_t sweepSpeed;   // 0 slow, 1 normal, 2 fast, 3 off
  uint8_t distUnit;     // 0 nm, 1 km
  uint8_t altUnit;      // 0 ft, 1 m
  uint8_t speedUnit;    // 0 kt, 1 km/h
};

extern Settings g_settings;

void     settingsBegin();                       // load (or create) settings
void     settingsSave();                        // write everything to flash
void     settingsFactoryReset();                // erase stored values, reload defaults
void     settingsResetToUserDefaults();         // firmware defaults, but blank WiFi and coordinates
void     settingsSetWifi(const char* ssid, const char* pass);   // stores + saves
void     settingsSetTrainStation(const char* crs);              // validates (3 letters) + saves
void     settingsSetTrainApiKey(const char* key);                // stores + saves, no reflash needed
bool     settingsHomeValid(double lat, double lon);
bool     settingsSetHome(double lat, double lon);               // validates + saves
void     settingsClearHome();
uint32_t settingsHomeRev();                     // increments when home changes
float    settingsRangeNm();
// True right now if "Night dimming" is on AND the clock is within the
// configured night window. Shared by the display (dims the screen) and the
// audio task (mutes all tones/clips/announcements) so both follow exactly
// the same window from one place rather than two copies of the same hour
// check potentially drifting apart.
bool     settingsNightModeActive();
uint8_t  settingsRangeIndexFor(uint16_t nm);
void     settingsMarkDirty(uint32_t nowMs);     // save later (flash-wear friendly)
void     settingsService(uint32_t nowMs);       // call from loop()
void     settingsConsole();                     // Serial commands, call from loop()

// Formatting according to the selected units. "out" always NUL-terminated.
void fmtDistance(char* out, size_t n, float nm, bool withUnit);
void fmtAltitude(char* out, size_t n, int32_t ft, bool has, bool onGround);
void fmtSpeed(char* out, size_t n, float kt, bool has);
const char* unitDistanceName();
