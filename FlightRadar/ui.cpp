#include "ui.h"
#include "config.h"
#include "display.h"
#include "radar.h"
#include "aircraft.h"
#include "aircraft_photo.h"
#include "aircraft_route.h"
#include "adsb_api.h"
#include "wifi_manager.h"
#include "settings.h"
#include "audio.h"
#include "update.h"
#include "train.h"
#include "icons.h"
#include "util.h"
#include <Arduino.h>
#include <math.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <TJpg_Decoder.h>

// ===========================================================================
//  State
// ===========================================================================
enum Screen : uint8_t {
  SCR_BOOT, SCR_RADAR, SCR_MENU, SCR_HOME_EDIT, SCR_WIFI, SCR_ABOUT,
  SCR_REBOOT_PROMPT, SCR_DEFAULTS_CONFIRM, SCR_UPDATE_PROMPT, SCR_MILITARY_ALERT,
  SCR_TRAIN, SCR_TRAIN_STATION_EDIT, SCR_AIRCRAFT_SEARCH
};
// Radar UI modes (documented in README):
//   MODE_ZOOM   : encoder turns zoom the radar; short press selects nearest aircraft.
//   MODE_SELECT : encoder turns cycle the selected aircraft; short press confirms it
//                 (back to ZOOM mode, aircraft stays highlighted).
enum Mode : uint8_t { MODE_ZOOM, MODE_SELECT };

static Screen   s_screen = SCR_BOOT;
static Mode     s_mode = MODE_ZOOM;
static bool     s_dirty = true;            // static screens: redraw needed
static bool     s_panelDirty = true;
static uint32_t s_panelNext = 0;
static uint32_t s_lastFrame = 0;
static uint32_t s_lastAcUpdate = 0;
static uint32_t s_fpsCount = 0, s_fpsT0 = 0;
static uint8_t  s_fps = 0;
static char     s_hdrKey[104] = "";
static char     s_toast[36] = "";
static uint32_t s_toastUntil = 0;
static uint32_t s_alertedKey = 0;
static uint16_t s_alertedSquawk = 0;
static uint8_t  s_effBrightness = 0;
static uint32_t s_milAlertStart = 0;
static uint16_t s_milAlertCount = 0;
static constexpr uint32_t MIL_ALERT_MAX_MS = 12000;   // safety cap in case the clip never reports done
static char s_photoIcao[8] = "";
static char s_photoRegistration[13] = "";
static AircraftPhotoState s_photoState = AIRCRAFT_PHOTO_IDLE;
static uint16_t* s_photoPixels = nullptr;
static char s_routeIcao[8] = "";
static char s_routeCallsign[9] = "";
static AircraftRouteState s_routeState = AIRCRAFT_ROUTE_IDLE;
static char s_departure[5] = "";
static char s_arrival[5] = "";
static constexpr int PHOTO_W = 196;
static constexpr int PHOTO_H = 104;
static constexpr int PHOTO_X = SCREEN_W - PHOTO_W - 8;
static constexpr int PHOTO_Y = 26;
static int s_photoCropX = 0, s_photoCropY = 0;
static int s_photoInsetX = 0, s_photoInsetY = 0;

// boot
static uint8_t  s_bootStage = 0;
static uint32_t s_bootT0 = 0, s_bootStageT = 0;

// menu
enum ItemId : uint8_t {
  IT_RANGE, IT_UNITS, IT_THEME, IT_BRIGHT, IT_SPEED, IT_HOME, IT_WIFI, IT_ABOUT,
  IT_UNIT_DIST, IT_UNIT_ALT, IT_UNIT_SPD, IT_ADVANCED,
  IT_PORTAL_ENABLED, IT_PORTAL_FAILS, IT_ADSB_MODE, IT_NIGHT_MODE,
  IT_GROUND, IT_AUDIO, IT_LABELS, IT_ENCODER_REVERSE, IT_ENCODER_STEPS,
  IT_PORTAL_NOW, IT_SWITCH_MODE, IT_SERIAL_BAUD, IT_RESTORE_DEFAULTS, IT_SPEAKER_TEST,
  IT_CHECK_UPDATE, IT_TRAIN_STATION, IT_FIND_AIRCRAFT
};
struct MenuItem { const char* label; ItemId id; bool editable; };
static const MenuItem MAIN_ITEMS[] = {
  {"Switch mode", IT_SWITCH_MODE, false},
  {"Find aircraft", IT_FIND_AIRCRAFT, false},
  {"Range", IT_RANGE, true}, {"Units", IT_UNITS, false}, {"Theme", IT_THEME, true},
  {"Brightness", IT_BRIGHT, true}, {"Radar speed", IT_SPEED, true},
  {"Home location", IT_HOME, false}, {"Advanced", IT_ADVANCED, false},
  {"WiFi status", IT_WIFI, false}, {"About", IT_ABOUT, false},
};
static const MenuItem UNIT_ITEMS[] = {
  {"Distance", IT_UNIT_DIST, true}, {"Altitude", IT_UNIT_ALT, true}, {"Speed", IT_UNIT_SPD, true},
};
static const MenuItem ADVANCED_ITEMS[] = {
  {"Portal enabled", IT_PORTAL_ENABLED, true}, {"Portal after fails", IT_PORTAL_FAILS, true},
  {"ADS-B sources", IT_ADSB_MODE, true}, {"Night dimming", IT_NIGHT_MODE, true},
  {"Ground aircraft", IT_GROUND, true}, {"Audio", IT_AUDIO, true},
  {"Labels", IT_LABELS, true}, {"Encoder reverse", IT_ENCODER_REVERSE, true},
  {"Encoder steps", IT_ENCODER_STEPS, true}, {"Train station", IT_TRAIN_STATION, false},
  {"Open web settings", IT_PORTAL_NOW, false},
  {"Serial baud", IT_SERIAL_BAUD, true}, {"Test speaker", IT_SPEAKER_TEST, false},
  {"Check for update", IT_CHECK_UPDATE, false},
  {"Restore defaults", IT_RESTORE_DEFAULTS, false},
};
#define N_MAIN (sizeof(MAIN_ITEMS) / sizeof(MAIN_ITEMS[0]))
#define N_UNITS (sizeof(UNIT_ITEMS) / sizeof(UNIT_ITEMS[0]))
#define N_ADVANCED (sizeof(ADVANCED_ITEMS) / sizeof(ADVANCED_ITEMS[0]))
static uint8_t s_page = 0;                 // 0 main, 1 units
static uint8_t s_cursor[3] = {0, 0, 0};
static bool    s_editing = false;
static uint8_t s_editBackup = 0;
static const uint32_t SERIAL_BAUD_OPTIONS[] = {9600, 19200, 38400, 57600, 115200, 230400, 460800, 921600};

// home editor
static struct {
  uint8_t pos;                              // 0 lat sign, 1..6 lat digits, 7 lon sign, 8..14 lon digits, 15 SAVE
  bool latNeg, lonNeg;
  uint8_t lat[6];                           // DD.dddd
  uint8_t lon[7];                           // DDD.dddd
} he;

// train station (CRS code) editor
static struct {
  uint8_t pos;                              // 0..2 the three letters, 3 SAVE
  char    letters[3];
} te;

// train board screen
static bool s_trainMode = false;            // true once "Switch mode" has swapped the main screen to the train board

// aircraft search-and-lock (callsign/registration) editor
static const char AC_SEARCH_CHARSET[] = " ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-";
#define AC_SEARCH_CHARSET_LEN ((int)(sizeof(AC_SEARCH_CHARSET) - 1))
static struct {
  uint8_t pos;                              // 0..7 the eight characters, 8 SAVE
  char    chars[8];
} ae;

static void toast(const char* msg, uint32_t nowMs, uint32_t ms = 2000) {
  fr_copy(s_toast, sizeof(s_toast), msg);
  s_toastUntil = nowMs + ms;
  s_panelDirty = true;
}

static void panelBegin();
static void panelPush();
static void panelText(int y, const char* s, int font, uint16_t col, int datum = TL_DATUM, int x = 8);
static void drawHeader(const char* l, const char* m, const char* r, uint16_t dot);

static void drawDefaultsConfirm() {
  const Theme& th = themeCurrent();
  spRadar.fillSprite(th.bg);
  spRadar.setTextDatum(MC_DATUM);
  spRadar.setTextColor(th.warn);
  spRadar.drawString("RESTORE DEFAULTS?", SCREEN_W / 2, 70, 4);
  spRadar.setTextColor(th.text);
  spRadar.drawString("This clears saved WiFi credentials", SCREEN_W / 2, 120, 2);
  spRadar.drawString("and home coordinates.", SCREEN_W / 2, 150, 2);
  spRadar.drawString("Other options return to firmware defaults.", SCREEN_W / 2, 180, 2);
  spRadar.pushSprite(0, RADAR_Y);
  panelBegin();
  panelText(20, "PRESS: RESTORE", 2, th.select);
  panelText(48, "BACK: CANCEL", 2, th.textDim);
  panelPush();
  drawHeader("RESTORE DEFAULTS", "", "", th.warn);
}

static void drawUpdatePrompt() {
  const Theme& th = themeCurrent();
  const FwUpdateState st = updateState();
  char ver[16] = "";
  updateAvailableVersion(ver, sizeof(ver));
  spRadar.fillSprite(th.bg);
  spRadar.setTextDatum(MC_DATUM);

  if (st == FW_UPDATE_DOWNLOADING) {
    spRadar.setTextColor(th.select);
    spRadar.drawString("INSTALLING UPDATE", SCREEN_W / 2, 70, 4);
    spRadar.setTextColor(th.text);
    char pct[8];
    snprintf(pct, sizeof(pct), "%u%%", (unsigned)updateProgressPercent());
    spRadar.drawString(pct, SCREEN_W / 2, 130, 4);
    spRadar.drawRoundRect(40, 170, SCREEN_W - 80, 18, 4, th.ringOuter);
    const int fillW = (SCREEN_W - 84) * updateProgressPercent() / 100;
    if (fillW > 0) spRadar.fillRoundRect(42, 172, fillW, 14, 3, th.select);
    spRadar.setTextColor(th.textDim);
    spRadar.drawString("The device will restart automatically.", SCREEN_W / 2, 210, 2);
  } else if (st == FW_UPDATE_ERROR) {
    char err[40];
    updateLastError(err, sizeof(err));
    spRadar.setTextColor(th.warn);
    spRadar.drawString("UPDATE FAILED", SCREEN_W / 2, 70, 4);
    spRadar.setTextColor(th.text);
    spRadar.drawString(err, SCREEN_W / 2, 130, 2);
    spRadar.setTextColor(th.textDim);
    spRadar.drawString("The current firmware is unaffected.", SCREEN_W / 2, 160, 2);
  } else {
    spRadar.setTextColor(th.select);
    spRadar.drawString("UPDATE AVAILABLE", SCREEN_W / 2, 70, 4);
    spRadar.setTextColor(th.text);
    char line[48];
    snprintf(line, sizeof(line), "Version %s (installed: %s)", ver, FW_VERSION);
    spRadar.drawString(line, SCREEN_W / 2, 120, 2);
    spRadar.setTextColor(th.textDim);
    spRadar.drawString("Takes about a minute; do not power off.", SCREEN_W / 2, 150, 2);
  }
  spRadar.pushSprite(0, RADAR_Y);

  panelBegin();
  if (st == FW_UPDATE_DOWNLOADING) {
    panelText(20, "INSTALLING - PLEASE WAIT", 2, th.textDim);
  } else if (st == FW_UPDATE_ERROR) {
    panelText(20, "PRESS OR BACK: DISMISS", 2, th.text);
  } else {
    panelText(20, "PRESS: INSTALL NOW", 2, th.select);
    panelText(48, "BACK: NOT NOW", 2, th.textDim);
  }
  panelPush();
  drawHeader("FIRMWARE UPDATE", "", "", st == FW_UPDATE_ERROR ? th.warn : th.select);
}

static void drawMilitaryAlert() {
  const uint16_t alertRed = RGB565(180, 0, 0);
  const uint16_t triFill  = RGB565(255, 196, 0);
  spRadar.fillSprite(alertRed);

  // Warning triangle with "!" - centred in the upper half of the radar area.
  const int cx = SCREEN_W / 2, cy = 110, half = 46;
  spRadar.fillTriangle(cx, cy - half, cx - half, cy + half, cx + half, cy + half, triFill);
  spRadar.drawTriangle(cx, cy - half, cx - half, cy + half, cx + half, cy + half, TFT_BLACK);
  spRadar.setTextDatum(MC_DATUM);
  spRadar.setTextColor(TFT_BLACK);
  spRadar.drawString("!", cx, cy + 14, 4);

  spRadar.setTextColor(TFT_WHITE);
  spRadar.setTextSize(2);
  spRadar.drawString("MILITARY", SCREEN_W / 2, 200, 4);
  spRadar.setTextSize(1);
  spRadar.drawString("AIRCRAFT DETECTED", SCREEN_W / 2, 240, 4);

  char line[32];
  snprintf(line, sizeof(line), "%u MILITARY AIRCRAFT IN RANGE",
           (unsigned)(s_milAlertCount ? s_milAlertCount : aircraftVisibleMilitaryCount()));
  spRadar.drawString(line, SCREEN_W / 2, 280, 2);
  spRadar.pushSprite(0, RADAR_Y);

  panelBegin();
  panelText(20, "PLAYING ANNOUNCEMENT...", 2, TFT_WHITE);
  panelText(48, "PRESS ANY BUTTON TO DISMISS", 2, RGB565(220, 220, 220));
  panelPush();
  drawHeader("MILITARY ALERT", "", "", TFT_WHITE);
}

// ===========================================================================
//  Brightness (with optional night mode)
// ===========================================================================
static void applyBrightness() {
  uint8_t target = g_settings.brightness;
  if (g_settings.nightModeEnabled) {
    struct tm t;
    if (clockGet(&t)) {
    const int h = t.tm_hour;
    const bool night = (g_settings.nightStartHour > g_settings.nightEndHour)
                         ? (h >= g_settings.nightStartHour || h < g_settings.nightEndHour)
                         : (h >= g_settings.nightStartHour && h < g_settings.nightEndHour);
    if (night) target = g_settings.nightBrightness;
    }
  }
  if (target != s_effBrightness) {
    s_effBrightness = target;
    displaySetBrightness(target);
  }
}

// ===========================================================================
//  Header (top 24 px) - redrawn only when its text changes
// ===========================================================================
static void drawHeader(const char* l, const char* m, const char* r, uint16_t dot) {
  char key[104];
  snprintf(key, sizeof(key), "%s|%s|%s|%u|%u", l, m, r, (unsigned)dot, (unsigned)g_settings.theme);
  if (!strcmp(key, s_hdrKey)) return;
  fr_copy(s_hdrKey, sizeof(s_hdrKey), key);
  const Theme& th = themeCurrent();
  TFT_eSprite& s = spHeader;
  s.fillSprite(th.bg);
  s.drawFastHLine(0, HEADER_H - 1, SCREEN_W, th.grid);
  s.setTextColor(aircraftEmergencyActive() ? RGB565(255, 0, 0) : th.text);
  s.setTextDatum(ML_DATUM);
  s.drawString(l, 6, 11, 2);
  s.setTextColor(th.select);
  s.setTextDatum(MC_DATUM);
  s.drawString(m, 165, 11, 2);
  s.setTextColor(th.textDim);
  s.setTextDatum(MR_DATUM);
  s.drawString(r, 294, 11, 2);
  s.fillCircle(308, 11, 5, dot);
  s.pushSprite(0, 0);
}

static uint16_t statusDot() {
  const Theme& th = themeCurrent();
  if (!wifiIsConnected()) return th.warn;
  AdsbStatus st;
  adsbGetStatus(st);
  return st.lastOk ? th.ok : RGB565(255, 180, 0);
}

static const char* adsbSourceLabel() {
  switch (g_settings.adsbMode) {
    case 0: return "adsb.lol + adsb.fi";
    case 1: return "adsb.lol";
    case 2: return "adsb.fi";
    case 3: return "custom";
    case 4: return "airplanes.live";
    default: return "unknown";
  }
}

// ===========================================================================
//  Bottom panel helpers
// ===========================================================================
static void panelBegin() {
  const Theme& th = themeCurrent();
  spPanel.fillSprite(th.bg);
  spPanel.drawFastHLine(0, 0, SCREEN_W, th.grid);
}
static void panelPush() { spPanel.pushSprite(0, PANEL_Y); }

static void panelText(int y, const char* s, int font, uint16_t col, int datum, int x) {
  spPanel.setTextDatum(datum);
  spPanel.setTextColor(col);
  spPanel.drawString(s, x, y, font);
}

static void panelCompactRow(int y, const char* label, const char* value) {
  const Theme& th = themeCurrent();
  char compactValue[11];
  snprintf(compactValue, sizeof(compactValue), "%.10s", value);
  spPanel.setTextDatum(TL_DATUM);
  spPanel.setTextColor(th.textDim);
  spPanel.drawString(label, 8, y, 1);
  spPanel.setTextColor(th.text);
  spPanel.drawString(compactValue, 54, y, 1);
}

static bool photoJpegOutput(int16_t x, int16_t y, uint16_t width,
                            uint16_t height, uint16_t* pixels) {
  if (!s_photoPixels || !pixels) return false;
  for (uint16_t row = 0; row < height; ++row) {
    const int destY = s_photoInsetY + y + row - s_photoCropY;
    if (destY < 0 || destY >= PHOTO_H) continue;
    for (uint16_t col = 0; col < width; ++col) {
      const int destX = s_photoInsetX + x + col - s_photoCropX;
      if (destX >= 0 && destX < PHOTO_W) {
        s_photoPixels[destY * PHOTO_W + destX] =
          pixels[row * width + col];
      }
    }
  }
  return true;
}

static bool decodePhoto(const AircraftPhotoResult& photo) {
  if (!photo.jpeg || !photo.jpegLength || photo.jpegLength > UINT32_MAX) return false;
  Serial.printf("[photo] decoding %u bytes (free heap %u KB, free PSRAM %u KB)\n",
                (unsigned)photo.jpegLength, (unsigned)(ESP.getFreeHeap() / 1024),
                (unsigned)(ESP.getFreePsram() / 1024));

  uint16_t width = 0;
  uint16_t height = 0;
  const JRESULT sizeResult =
    TJpgDec.getJpgSize(&width, &height, photo.jpeg, (uint32_t)photo.jpegLength);
  if (sizeResult != JDR_OK || !width || !height) {
    Serial.printf("[photo] JPEG size read failed (decoder error %d)\n", sizeResult);
    return false;
  }

  uint8_t scale = 1;
  while (scale < 8 &&
         (width / scale > PHOTO_W * 2 || height / scale > PHOTO_H * 2)) scale *= 2;
  const int scaledWidth = (width + scale - 1) / scale;
  const int scaledHeight = (height + scale - 1) / scale;
  s_photoCropX = scaledWidth > PHOTO_W ? (scaledWidth - PHOTO_W) / 2 : 0;
  s_photoCropY = scaledHeight > PHOTO_H ? (scaledHeight - PHOTO_H) / 2 : 0;
  s_photoInsetX = scaledWidth < PHOTO_W ? (PHOTO_W - scaledWidth) / 2 : 0;
  s_photoInsetY = scaledHeight < PHOTO_H ? (PHOTO_H - scaledHeight) / 2 : 0;

  if (s_photoPixels) free(s_photoPixels);
  s_photoPixels = (uint16_t*)(psramFound() ? ps_malloc(PHOTO_W * PHOTO_H * sizeof(uint16_t)) : nullptr);
  if (!s_photoPixels) s_photoPixels = (uint16_t*)malloc(PHOTO_W * PHOTO_H * sizeof(uint16_t));
  if (!s_photoPixels) {
    return false;
  }
  memset(s_photoPixels, 0, PHOTO_W * PHOTO_H * sizeof(uint16_t));

  TJpgDec.setCallback(photoJpegOutput);
  TJpgDec.setJpgScale(scale);
  TJpgDec.setSwapBytes(true);
  const JRESULT decodeResult =
    TJpgDec.drawJpg(0, 0, photo.jpeg, (uint32_t)photo.jpegLength);
  if (decodeResult != JDR_OK) {
    Serial.printf("[photo] JPEG decode failed (decoder error %d)\n", decodeResult);
  }
  return decodeResult == JDR_OK;
}

static void clearPhotoPixels() {
  if (s_photoPixels) {
    free(s_photoPixels);
    s_photoPixels = nullptr;
  }
}

static void updatePhotoSelection(const Aircraft* selected) {
  const char* icao = selected ? selected->icao : "";
  const char* registration = selected ? selected->registration : "";
  if (!strcmp(icao, s_photoIcao) &&
      !strcmp(registration, s_photoRegistration)) return;
  fr_copy(s_photoIcao, sizeof(s_photoIcao), icao);
  fr_copy(s_photoRegistration, sizeof(s_photoRegistration), registration);
  clearPhotoPixels();
  s_photoState = selected ? AIRCRAFT_PHOTO_LOADING : AIRCRAFT_PHOTO_IDLE;
  aircraftPhotoRequest(icao, registration);
}

static void receivePhotoResult() {
  AircraftPhotoResult result = {};
  bool changed = false;
  while (aircraftPhotoPoll(result)) {
    if (!strcmp(result.icao, s_photoIcao)) {
      clearPhotoPixels();
      s_photoState = result.state;
      if (result.state == AIRCRAFT_PHOTO_READY) {
        if (decodePhoto(result)) {
          s_photoState = AIRCRAFT_PHOTO_READY;
        } else {
          clearPhotoPixels();
          s_photoState = AIRCRAFT_PHOTO_FAILED;
          Serial.printf("[photo] could not decode JPEG for %s\n", result.icao);
        }
      }
      changed = true;
    }
    free(result.jpeg);
    result = {};
  }
  if (s_photoIcao[0] && s_photoState == AIRCRAFT_PHOTO_LOADING &&
      aircraftPhotoState() == AIRCRAFT_PHOTO_FAILED) {
    s_photoState = AIRCRAFT_PHOTO_FAILED;
    changed = true;
  }
  if (changed) s_panelDirty = true;
}

static void updateRouteSelection(const Aircraft* selected) {
  const char* icao = selected ? selected->icao : "";
  const char* callsign = selected ? selected->callsign : "";
  if (!strcmp(icao, s_routeIcao) && !strcmp(callsign, s_routeCallsign)) return;
  fr_copy(s_routeIcao, sizeof(s_routeIcao), icao);
  fr_copy(s_routeCallsign, sizeof(s_routeCallsign), callsign);
  s_departure[0] = 0;
  s_arrival[0] = 0;
  s_routeState = selected ? AIRCRAFT_ROUTE_LOADING : AIRCRAFT_ROUTE_IDLE;
  aircraftRouteRequest(icao, callsign);
  s_panelDirty = true;
}

static void receiveRouteResult() {
  AircraftRouteResult result = {};
  bool changed = false;
  while (aircraftRoutePoll(result)) {
    if (!strcmp(result.icao, s_routeIcao) && !strcmp(result.callsign, s_routeCallsign)) {
      s_routeState = result.state;
      fr_copy(s_departure, sizeof(s_departure), result.departure);
      fr_copy(s_arrival, sizeof(s_arrival), result.arrival);
      changed = true;
    }
    result = {};
  }
  if (s_routeIcao[0] && s_routeState == AIRCRAFT_ROUTE_LOADING &&
      aircraftRouteState() == AIRCRAFT_ROUTE_FAILED) {
    s_routeState = AIRCRAFT_ROUTE_FAILED;
    changed = true;
  }
  if (changed) s_panelDirty = true;
}

// Data / connection status -> short reason text (empty when everything is fine)
static void statusReason(char* out, size_t n) {
  AdsbStatus st;
  adsbGetStatus(st);
  if (!wifiIsConnected()) {
    fr_copy(out, n, wifiState() == WIFI_ST_CONNECTING ? "CONNECTING TO WIFI..." : "WIFI DISCONNECTED");
  } else if (!st.lastOk) {
    if (st.failures == 0 && !st.everOk) fr_copy(out, n, "LOADING AIRCRAFT...");
    else snprintf(out, n, "API ERROR: %s", st.lastError[0] ? st.lastError : "UNKNOWN");
  } else {
    out[0] = 0;
  }
}

static void drawPanelAircraft(const Aircraft& a) {
  const Theme& th = themeCurrent();
  char buf[32];
  panelBegin();
  iconDraw(spPanel, a.cls, 24, 24, a.hasHeading ? a.heading : 0.0f, a.military ? th.military : th.select, 1.5f);
  panelText(6, a.callsign[0] ? a.callsign : "N/A", 2, th.select, TL_DATUM, 50);
  panelText(6, a.aircraftType[0] ? a.aircraftType : "N/A", 2, th.text, TR_DATUM, 314);
  snprintf(buf, sizeof(buf), "%.17s", a.operatorName[0] ? a.operatorName : "N/A");
  panelText(38, buf, 1, th.textDim, TL_DATUM, 8);
  panelCompactRow(49, "REG", a.registration[0] ? a.registration : "N/A");

  fmtAltitude(buf, sizeof(buf), a.altitude, a.hasAltitude, a.onGround);
  panelCompactRow(62, "ALT", buf);
  fmtSpeed(buf, sizeof(buf), a.groundSpeed, a.hasSpeed);
  panelCompactRow(73, "SPD", buf);
  if (a.hasHeading) snprintf(buf, sizeof(buf), "%03d", (int)lroundf(a.heading) % 360);
  else fr_copy(buf, sizeof(buf), "N/A");
  panelCompactRow(84, "HDG", buf);
  fmtDistance(buf, sizeof(buf), a.distance, true);
  panelCompactRow(95, "DIST", buf);
  panelCompactRow(106, "ICAO", a.icao[0] ? a.icao : "N/A");
  snprintf(buf, sizeof(buf), "%03d", (int)lroundf(a.bearing) % 360);
  panelCompactRow(117, "BRG", buf);
  if (a.squawk == 7500 || a.squawk == 7700) {
    snprintf(buf, sizeof(buf), "SQUAWK: %u - %s", (unsigned)a.squawk,
             a.squawk == 7500 ? "HIJACK" : "EMERGENCY");
  } else if (a.service != SVC_NONE) {
    snprintf(buf, sizeof(buf), "SERVICE: %s", aircraftServiceName(a.service));
  } else if (s_routeState == AIRCRAFT_ROUTE_READY) {
    snprintf(buf, sizeof(buf), "ROUTE: %s > %s", s_departure, s_arrival);
  } else if (s_routeState == AIRCRAFT_ROUTE_LOADING) {
    fr_copy(buf, sizeof(buf), "ROUTE: LOADING...");
  } else if (s_routeState == AIRCRAFT_ROUTE_FAILED) {
    fr_copy(buf, sizeof(buf), "ROUTE: ERROR");
  } else if (s_routeState == AIRCRAFT_ROUTE_NOT_FOUND) {
    fr_copy(buf, sizeof(buf), "ROUTE: N/A");
  } else {
    fr_copy(buf, sizeof(buf), "ROUTE: --");
  }
  panelText(128, buf, 1, s_routeState == AIRCRAFT_ROUTE_FAILED ? th.warn : th.textDim);

  spPanel.drawRect(PHOTO_X - 1, PHOTO_Y - 1, PHOTO_W + 2, PHOTO_H + 2, th.grid);
  if (s_photoPixels) {
    spPanel.pushImage(PHOTO_X, PHOTO_Y, PHOTO_W, PHOTO_H, s_photoPixels);
  } else {
    const char* photoText = s_photoState == AIRCRAFT_PHOTO_LOADING ? "LOADING"
                          : s_photoState == AIRCRAFT_PHOTO_NOT_FOUND ? "NO PHOTO"
                          : s_photoState == AIRCRAFT_PHOTO_FAILED ? "UNAVAILABLE" : "PHOTO";
    panelText(PHOTO_Y + PHOTO_H / 2, photoText, 1, th.textDim, MC_DATUM,
              PHOTO_X + PHOTO_W / 2);
  }
  const char* hint = (s_mode == MODE_SELECT) ? "TURN: NEXT/PREV   PRESS: CONFIRM   BACK: CANCEL"
                                            : "PRESS: RESELECT   BACK: CLEAR   TURN: ZOOM";
  panelText(136, hint, 1, th.textDim, TL_DATUM, 8);
  panelPush();
}

static void drawPanelStatus(uint32_t now) {
  const Theme& th = themeCurrent();
  char buf[48], reason[40];
  AdsbStatus st;
  adsbGetStatus(st);
  statusReason(reason, sizeof(reason));
  panelBegin();

  const uint16_t n = aircraftVisibleCount();
  if (n > 0) snprintf(buf, sizeof(buf), "%u AIRCRAFT", (unsigned)n);
  else fr_copy(buf, sizeof(buf), (st.lastOk && wifiIsConnected()) ? "NO AIRCRAFT IN RANGE" : "NO DATA");
  panelText(6, buf, 4, (n > 0 || reason[0] == 0) ? th.text : th.warn);

  if (reason[0]) panelText(36, reason, 2, th.warn);
  else panelText(36, "STATUS: OK", 2, th.ok);

  char clk[16];
  clockFormat(clk, sizeof(clk), st.lastSuccessEpoch);
  if (st.lastSuccessMs) snprintf(buf, sizeof(buf), "LAST UPDATE: %s  (%lus ago)", clk, (unsigned long)((now - st.lastSuccessMs) / 1000));
  else fr_copy(buf, sizeof(buf), "LAST UPDATE: NEVER");
  panelText(56, buf, 2, th.textDim);

  if (s_toast[0] && (int32_t)(s_toastUntil - now) > 0) {
    panelText(80, s_toast, 2, th.select);
  } else if (wifiPortalActive()) {
    char ip[20];
    wifiPortalIpString(ip, sizeof(ip));
    snprintf(buf, sizeof(buf), "WIFI SETUP: JOIN '%s'", wifiPortalSsid());
    panelText(80, buf, 2, th.select);
    snprintf(buf, sizeof(buf), "THEN OPEN  http://%s", ip);
    panelText(98, buf, 2, th.select);
  } else {
    panelText(80, "TURN: ZOOM     PRESS: SELECT AIRCRAFT", 1, th.textDim);
    panelText(94, "HOLD / MENU: SETTINGS     HOME: RESET", 1, th.textDim);
  }
  char sourceLine[36];
  snprintf(sourceLine, sizeof(sourceLine), "SOURCE: %s", adsbSourceLabel());
  panelText(130, sourceLine, 1, th.textDim);
  panelPush();
}

static void drawPanelRadar(uint32_t now) {
  const Aircraft* sel = aircraftSelected();
  if (sel) drawPanelAircraft(*sel);
  else drawPanelStatus(now);
}

// ===========================================================================
//  Range / mode helpers
// ===========================================================================
static void setRangeIndex(int idx, uint32_t now) {
  idx = fr_clamp(idx, 0, (int)RANGE_COUNT - 1);
  if (idx == g_settings.rangeIndex) return;
  g_settings.rangeIndex = (uint8_t)idx;
  adsbSetRange(RANGES_NM[idx]);
  settingsMarkDirty(now);
  s_panelDirty = true;
}

static void openMenu() {
  s_screen = SCR_MENU;
  s_page = 0;
  s_editing = false;
  s_dirty = true;
}
// Returns to whichever "main" screen is active - the radar, or the train
// board if "Switch mode" has swapped it in - from menus, editors and prompts.
static void closeToRadar() {
  s_screen = s_trainMode ? SCR_TRAIN : SCR_RADAR;
  s_editing = false;
  s_dirty = true;
  s_panelDirty = true;
  s_hdrKey[0] = 0;
  trainSetActive(s_trainMode);
}

void uiShowMilitaryAlert() {
  // Don't interrupt menu navigation, home-location editing, or another
  // already-active full-screen prompt - only an idle main screen (radar or
  // the train board) gets pre-empted, same priority rule as the
  // firmware-update prompt below.
  if (s_screen != SCR_RADAR && s_screen != SCR_TRAIN) return;
  s_screen = SCR_MILITARY_ALERT;
  s_milAlertStart = millis();
  s_milAlertCount = aircraftVisibleMilitaryCount();
  s_dirty = true;
  s_hdrKey[0] = 0;
}

// ===========================================================================
//  Menu items
// ===========================================================================
static const char* const SPEED_NAMES[4] = {"SLOW", "NORMAL", "FAST", "OFF"};

static uint8_t itemGet(uint8_t id) {
  switch (id) {
    case IT_RANGE:     return g_settings.rangeIndex;
    case IT_THEME:     return g_settings.theme;
    case IT_BRIGHT:    return g_settings.brightness;
    case IT_SPEED:     return g_settings.sweepSpeed;
    case IT_UNIT_DIST: return g_settings.distUnit;
    case IT_UNIT_ALT:  return g_settings.altUnit;
    case IT_UNIT_SPD:  return g_settings.speedUnit;
    case IT_PORTAL_ENABLED: return g_settings.portalEnabled;
    case IT_PORTAL_FAILS: return g_settings.portalAfterFails;
    case IT_ADSB_MODE: return g_settings.adsbMode;
    case IT_NIGHT_MODE: return g_settings.nightModeEnabled;
    case IT_GROUND: return g_settings.showGroundAircraft;
    case IT_AUDIO: return g_settings.audioEnabled;
    case IT_LABELS: return g_settings.labelsEnabled;
    case IT_ENCODER_REVERSE: return g_settings.encoderReverse;
    case IT_ENCODER_STEPS: return g_settings.encoderStepsPerDetent;
    case IT_SERIAL_BAUD:
      for (uint8_t i = 0; i < sizeof(SERIAL_BAUD_OPTIONS) / sizeof(SERIAL_BAUD_OPTIONS[0]); ++i)
        if (g_settings.serialBaud == SERIAL_BAUD_OPTIONS[i]) return i;
      return 4;
  }
  return 0;
}

static void itemSet(uint8_t id, uint8_t v, uint32_t now) {
  switch (id) {
    case IT_RANGE:     setRangeIndex(v, now); break;
    case IT_THEME:     g_settings.theme = v; s_hdrKey[0] = 0; break;
    case IT_BRIGHT:    g_settings.brightness = v; applyBrightness(); break;
    case IT_SPEED:     g_settings.sweepSpeed = v; break;
    case IT_UNIT_DIST: g_settings.distUnit = v; break;
    case IT_UNIT_ALT:  g_settings.altUnit = v; break;
    case IT_UNIT_SPD:  g_settings.speedUnit = v; break;
    case IT_PORTAL_ENABLED:
      g_settings.portalEnabled = v;
      break;
    case IT_PORTAL_FAILS: g_settings.portalAfterFails = fr_clamp<uint8_t>(v, 1, 20); break;
    case IT_ADSB_MODE: g_settings.adsbMode = v % 5; adsbRequestNow(); break;
    case IT_NIGHT_MODE: g_settings.nightModeEnabled = v; applyBrightness(); break;
    case IT_GROUND: g_settings.showGroundAircraft = v; break;
    case IT_AUDIO: g_settings.audioEnabled = v; break;
    case IT_LABELS: g_settings.labelsEnabled = v; break;
    case IT_ENCODER_REVERSE: g_settings.encoderReverse = v; break;
    case IT_ENCODER_STEPS: g_settings.encoderStepsPerDetent = v; break;
    case IT_SERIAL_BAUD:
      g_settings.serialBaud = SERIAL_BAUD_OPTIONS[v % (sizeof(SERIAL_BAUD_OPTIONS) / sizeof(SERIAL_BAUD_OPTIONS[0]))];
      break;
  }
}

static void itemAdjust(uint8_t id, int dir, uint32_t now) {
  int v = itemGet(id);
  switch (id) {
    case IT_RANGE:  v = fr_clamp(v + dir, 0, (int)RANGE_COUNT - 1); break;
    case IT_THEME:  v = (v + dir + THEME_COUNT) % THEME_COUNT; break;
    case IT_BRIGHT: v = fr_clamp(v + dir * 5, 5, 100); break;
    case IT_SPEED:  v = (v + dir + 4) % 4; break;
    case IT_SERIAL_BAUD:
      v = (v + dir + (int)(sizeof(SERIAL_BAUD_OPTIONS) / sizeof(SERIAL_BAUD_OPTIONS[0]))) %
          (int)(sizeof(SERIAL_BAUD_OPTIONS) / sizeof(SERIAL_BAUD_OPTIONS[0]));
      break;
    case IT_PORTAL_FAILS: v = fr_clamp(v + dir, 1, 20); break;
    case IT_ADSB_MODE: v = (v + dir + 5) % 5; break;
    case IT_ENCODER_STEPS:
      v = v == 1 ? (dir > 0 ? 2 : 4) : v == 2 ? (dir > 0 ? 4 : 1) : (dir > 0 ? 1 : 2);
      break;
    default:        v = v ? 0 : 1; break;                 // two-state unit toggles
  }
  itemSet(id, (uint8_t)v, now);
}

static void itemValue(uint8_t id, char* out, size_t n) {
  switch (id) {
    case IT_RANGE:     fmtDistance(out, n, (float)RANGES_NM[g_settings.rangeIndex], true); break;
    case IT_THEME:     fr_copy(out, n, themeGet(g_settings.theme).name); break;
    case IT_BRIGHT:
#if BACKLIGHT_PWM_ENABLED
      snprintf(out, n, "%u%%", (unsigned)g_settings.brightness);
#else
      fr_copy(out, n, "FIXED (PWM OFF)");
#endif
      break;
    case IT_SPEED:     fr_copy(out, n, SPEED_NAMES[g_settings.sweepSpeed & 3]); break;
    case IT_UNIT_DIST: fr_copy(out, n, g_settings.distUnit ? "KILOMETRES" : "NAUTICAL MILES"); break;
    case IT_UNIT_ALT:  fr_copy(out, n, g_settings.altUnit ? "METRES" : "FEET"); break;
    case IT_UNIT_SPD:  fr_copy(out, n, g_settings.speedUnit ? "KM/H" : "KNOTS"); break;
    case IT_HOME:
      if (g_settings.homeConfigured) snprintf(out, n, "%.2f %.2f", g_settings.homeLat, g_settings.homeLon);
      else fr_copy(out, n, "NOT SET");
      break;
    case IT_WIFI:      fr_copy(out, n, wifiStateName()); break;
    case IT_PORTAL_ENABLED: fr_copy(out, n, g_settings.portalEnabled ? "ON" : "OFF"); break;
    case IT_PORTAL_FAILS: snprintf(out, n, "%u", g_settings.portalAfterFails); break;
    case IT_ADSB_MODE: fr_copy(out, n, g_settings.adsbMode == 0 ? "BOTH" : g_settings.adsbMode == 1 ? "ADSB.LOL" : g_settings.adsbMode == 2 ? "ADSB.FI" : g_settings.adsbMode == 3 ? "CUSTOM" : "AIRPLANES.LIVE"); break;
    case IT_NIGHT_MODE: fr_copy(out, n, g_settings.nightModeEnabled ? "ON" : "OFF"); break;
    case IT_GROUND: fr_copy(out, n, g_settings.showGroundAircraft ? "ON" : "OFF"); break;
    case IT_AUDIO: fr_copy(out, n, g_settings.audioEnabled ? "ON" : "OFF"); break;
    case IT_LABELS: fr_copy(out, n, g_settings.labelsEnabled ? "ON" : "OFF"); break;
    case IT_ENCODER_REVERSE: fr_copy(out, n, g_settings.encoderReverse ? "YES" : "NO"); break;
    case IT_ENCODER_STEPS: snprintf(out, n, "%u", g_settings.encoderStepsPerDetent); break;
    case IT_SERIAL_BAUD: snprintf(out, n, "%lu", (unsigned long)g_settings.serialBaud); break;
    case IT_PORTAL_NOW: fr_copy(out, n, "OPEN"); break;
    case IT_SPEAKER_TEST: fr_copy(out, n, "TEST"); break;
    case IT_RESTORE_DEFAULTS: fr_copy(out, n, "RESET"); break;
    case IT_SWITCH_MODE: fr_copy(out, n, "CLICK"); break;
    case IT_CHECK_UPDATE: fr_copy(out, n, updateIsEnabled() ? "CHECK" : "DISABLED"); break;
    case IT_TRAIN_STATION: fr_copy(out, n, g_settings.trainStationCrs); break;
    case IT_FIND_AIRCRAFT:
      if (aircraftLockActive()) snprintf(out, n, "LOCKED: %s", aircraftLockQuery());
      else if (aircraftLockPending()) snprintf(out, n, "SEARCHING: %s", aircraftLockQuery());
      else fr_copy(out, n, "SET");
      break;
    default:           out[0] = 0;
  }
}

static void drawMenu() {
  const Theme& th = themeCurrent();
  TFT_eSprite& s = spRadar;
  const MenuItem* items = s_page == 1 ? UNIT_ITEMS : s_page == 2 ? ADVANCED_ITEMS : MAIN_ITEMS;
  const int n = s_page == 1 ? (int)N_UNITS : s_page == 2 ? (int)N_ADVANCED : (int)N_MAIN;
  const int rowHeight = s_page == 2 ? 18 : s_page == 0 ? 26 : 38;
  s.fillSprite(th.bg);
  for (int i = 0; i < n; i++) {
    const int y = 4 + i * rowHeight;
    const int boxHeight = rowHeight - 2;
    const bool cur = (i == s_cursor[s_page]);
    const bool ed = cur && s_editing;
    if (cur) {
      s.fillRoundRect(4, y, 312, boxHeight, 4, th.gridDim);
      s.drawRoundRect(4, y, 312, boxHeight, 4, ed ? th.select : th.ringOuter);
    }
    s.setTextDatum(ML_DATUM);
    s.setTextColor(ed ? th.select : th.text);
    const int font = s_page == 2 ? 2 : s_page == 0 ? 2 : 4;
    s.drawString(items[i].label, 14, y + boxHeight / 2, font);
    char v[28];
    itemValue(items[i].id, v, sizeof(v));
    s.setTextDatum(MR_DATUM);
    if (items[i].editable) {
      char t[36];
      if (ed) snprintf(t, sizeof(t), "< %s >", v); else fr_copy(t, sizeof(t), v);
      s.setTextColor(ed ? th.select : th.textDim);
      s.drawString(t, 306, y + boxHeight / 2, 2);
    } else if (items[i].id == IT_PORTAL_NOW || items[i].id == IT_SPEAKER_TEST ||
               items[i].id == IT_RESTORE_DEFAULTS || items[i].id == IT_SWITCH_MODE ||
               items[i].id == IT_CHECK_UPDATE) {
      const int buttonW = items[i].id == IT_SWITCH_MODE ? 48 : 46;
      const int buttonX = 298 - buttonW;
      const uint16_t buttonColor = cur ? th.select : th.ringOuter;
      s.drawRoundRect(buttonX, y + 2, buttonW, boxHeight - 4, 3, buttonColor);
      s.setTextDatum(MC_DATUM);
      s.setTextColor(cur ? th.select : th.textDim);
      s.drawString(v, buttonX + buttonW / 2, y + boxHeight / 2, 1);
    } else {
      s.setTextColor(th.textDim);
      if (v[0]) { s.drawString(v, 282, y + boxHeight / 2, 2); }
      s.drawString(">", 306, y + boxHeight / 2, 2);
    }
  }
  s.pushSprite(0, RADAR_Y);

  panelBegin();
  if (s_editing) {
    panelText(20, "TURN: CHANGE VALUE", 2, themeCurrent().text);
    panelText(44, "PRESS: SAVE     BACK: CANCEL", 2, themeCurrent().text);
  } else {
    panelText(20, "TURN: MOVE     PRESS: CLICK / EDIT", 2, themeCurrent().text);
    panelText(44, "BACK: UP     HOME / HOLD: EXIT", 2, themeCurrent().text);
  }
  panelPush();
  drawHeader(s_page == 1 ? "UNITS" : s_page == 2 ? "ADVANCED" : "SETTINGS", "", "", themeCurrent().ok);
}

// ===========================================================================
//  Home position editor
// ===========================================================================
static void homeEditInit() {
  auto split = [](double v, uint8_t* d, int intDigits) {
    long scaled = lround(fabs(v) * 10000.0);
    long ip = scaled / 10000, fp = scaled % 10000;
    for (int i = intDigits - 1; i >= 0; i--) { d[i] = (uint8_t)(ip % 10); ip /= 10; }
    for (int i = 3; i >= 0; i--) { d[intDigits + i] = (uint8_t)(fp % 10); fp /= 10; }
  };
  he.latNeg = g_settings.homeLat < 0;
  he.lonNeg = g_settings.homeLon < 0;
  split(g_settings.homeLat, he.lat, 2);
  split(g_settings.homeLon, he.lon, 3);
  he.pos = 0;
}

static double homeEditValue(const uint8_t* d, int intDigits, bool neg) {
  long ip = 0;
  for (int i = 0; i < intDigits; i++) ip = ip * 10 + d[i];
  long fp = 0;
  for (int i = 0; i < 4; i++) fp = fp * 10 + d[intDigits + i];
  double v = ip + fp / 10000.0;
  return neg ? -v : v;
}

static void drawCell(TFT_eSprite& s, int x, int y, char ch, bool hi) {
  const Theme& th = themeCurrent();
  if (hi) {
    s.fillRoundRect(x, y - 2, 22, 30, 3, th.gridDim);
    s.drawRoundRect(x, y - 2, 22, 30, 3, th.select);
  }
  char t[2] = {ch, 0};
  s.setTextDatum(MC_DATUM);
  s.setTextColor(hi ? th.select : th.text);
  s.drawString(t, x + 11, y + 13, 4);
}

static void drawCoordRow(int y, const char* label, int posBase, bool neg, char negCh, char posCh,
                         const uint8_t* d, int intDigits) {
  const Theme& th = themeCurrent();
  TFT_eSprite& s = spRadar;
  s.setTextDatum(ML_DATUM);
  s.setTextColor(th.textDim);
  s.drawString(label, 8, y + 13, 2);
  int x = 52;
  drawCell(s, x, y, neg ? negCh : posCh, he.pos == posBase);
  x += 24;
  for (int k = 0; k < intDigits + 4; k++) {
    if (k == intDigits) {
      s.setTextDatum(MC_DATUM);
      s.setTextColor(th.text);
      s.drawString(".", x + 4, y + 20, 4);
      x += 10;
    }
    drawCell(s, x, y, (char)('0' + d[k]), he.pos == posBase + 1 + k);
    x += 24;
  }
}

static void drawHomeEdit() {
  const Theme& th = themeCurrent();
  TFT_eSprite& s = spRadar;
  s.fillSprite(th.bg);
  s.setTextDatum(TL_DATUM);
  s.setTextColor(th.text);
  s.drawString("SET HOME POSITION", 8, 10, 4);
  drawCoordRow(70, "LAT", 0, he.latNeg, 'S', 'N', he.lat, 2);
  drawCoordRow(130, "LON", 7, he.lonNeg, 'W', 'E', he.lon, 3);
  const bool hi = (he.pos == 15);
  s.fillRoundRect(90, 205, 140, 38, 6, hi ? th.gridDim : th.bg);
  s.drawRoundRect(90, 205, 140, 38, 6, hi ? th.select : th.grid);
  s.setTextDatum(MC_DATUM);
  s.setTextColor(hi ? th.select : th.text);
  s.drawString("SAVE", 160, 224, 4);
  s.pushSprite(0, RADAR_Y);

  panelBegin();
  panelText(14, "TURN: CHANGE DIGIT / N-S / E-W", 2, th.text);
  panelText(38, "PRESS: NEXT     BACK: CANCEL", 2, th.text);
  panelText(62, "Decimal degrees, 4 decimals (~10 m)", 1, th.textDim);
  if (s_toast[0] && (int32_t)(s_toastUntil - millis()) > 0) panelText(90, s_toast, 2, th.warn);
  panelPush();
  drawHeader("HOME LOCATION", "", "", th.ok);
}

static void homeEditTurn(int dir) {
  const int p = he.pos;
  if (p == 0) he.latNeg = !he.latNeg;
  else if (p == 7) he.lonNeg = !he.lonNeg;
  else if (p >= 1 && p <= 6) he.lat[p - 1] = (he.lat[p - 1] + 10 + dir) % 10;
  else if (p >= 8 && p <= 14) he.lon[p - 8] = (he.lon[p - 8] + 10 + dir) % 10;
}

static void homeEditPress(uint32_t now) {
  if (he.pos < 15) { he.pos++; return; }
  const double la = homeEditValue(he.lat, 2, he.latNeg);
  const double lo = homeEditValue(he.lon, 3, he.lonNeg);
  if (settingsSetHome(la, lo)) {
    s_screen = SCR_MENU;
    adsbRequestNow();
  } else {
    toast("INVALID: LAT<=90, LON<=180", now, 3000);
    he.pos = 0;
  }
}

// ===========================================================================
//  Train station (CRS code) editor
// ===========================================================================
static void trainEditInit() {
  for (int i = 0; i < 3; i++)
    te.letters[i] = g_settings.trainStationCrs[i] ? g_settings.trainStationCrs[i] : 'A';
  te.pos = 0;
}

static void drawTrainEdit() {
  const Theme& th = themeCurrent();
  TFT_eSprite& s = spRadar;
  s.fillSprite(th.bg);
  s.setTextDatum(TL_DATUM);
  s.setTextColor(th.text);
  s.drawString("SET TRAIN STATION (CRS CODE)", 8, 10, 4);
  int x = (SCREEN_W - (3 * 24)) / 2;
  for (int i = 0; i < 3; i++) {
    drawCell(s, x, 90, te.letters[i], te.pos == i);
    x += 28;
  }
  const bool hi = (te.pos == 3);
  s.fillRoundRect(90, 160, 140, 38, 6, hi ? th.gridDim : th.bg);
  s.drawRoundRect(90, 160, 140, 38, 6, hi ? th.select : th.grid);
  s.setTextDatum(MC_DATUM);
  s.setTextColor(hi ? th.select : th.text);
  s.drawString("SAVE", 160, 179, 4);
  s.pushSprite(0, RADAR_Y);

  panelBegin();
  panelText(14, "TURN: CHANGE LETTER (A-Z)", 2, th.text);
  panelText(38, "PRESS: NEXT     BACK: CANCEL", 2, th.text);
  panelText(62, "3-letter National Rail station code", 1, th.textDim);
  panelText(80, "e.g. LIT = Littlehampton, VIC = London Victoria", 1, th.textDim);
  if (s_toast[0] && (int32_t)(s_toastUntil - millis()) > 0) panelText(108, s_toast, 2, th.warn);
  panelPush();
  drawHeader("TRAIN STATION", "", "", th.ok);
}

static void trainEditTurn(int dir) {
  if (te.pos >= 3) return;
  char& c = te.letters[te.pos];
  c = (char)('A' + ((c - 'A') + 26 + dir) % 26);
}

static void trainEditPress(uint32_t now) {
  if (te.pos < 3) { te.pos++; return; }
  char crs[4] = {te.letters[0], te.letters[1], te.letters[2], 0};
  settingsSetTrainStation(crs);
  trainRequestNow();
  s_screen = SCR_MENU;
  toast("STATION SAVED", now, 2000);
}

// ===========================================================================
//  Aircraft search-and-lock (callsign/registration) editor
// ===========================================================================
static void aircraftSearchEditInit() {
  const char* q = aircraftLockQuery();
  for (int i = 0; i < 8; i++) ae.chars[i] = q[i] ? q[i] : ' ';
  ae.pos = 0;
}

static void drawAircraftSearchEdit() {
  const Theme& th = themeCurrent();
  TFT_eSprite& s = spRadar;
  s.fillSprite(th.bg);
  s.setTextDatum(TL_DATUM);
  s.setTextColor(th.text);
  s.drawString("FIND AIRCRAFT (CALLSIGN/REG)", 8, 10, 4);
  int x = (SCREEN_W - (8 * 28)) / 2;
  for (int i = 0; i < 8; i++) {
    drawCell(s, x, 90, ae.chars[i], ae.pos == i);
    x += 28;
  }
  const bool hi = (ae.pos == 8);
  s.fillRoundRect(90, 160, 140, 38, 6, hi ? th.gridDim : th.bg);
  s.drawRoundRect(90, 160, 140, 38, 6, hi ? th.select : th.grid);
  s.setTextDatum(MC_DATUM);
  s.setTextColor(hi ? th.select : th.text);
  s.drawString("LOCK", 160, 179, 4);
  s.pushSprite(0, RADAR_Y);

  panelBegin();
  panelText(14, "TURN: CHANGE CHARACTER", 2, th.text);
  panelText(38, "PRESS: NEXT     BACK: CANCEL", 2, th.text);
  panelText(62, "Callsign (e.g. BAW123) or reg (e.g. G-ABCD)", 1, th.textDim);
  if (aircraftLockActive()) panelText(80, "Locked - leave blank and press LOCK to clear", 1, th.ok);
  else if (aircraftLockPending()) panelText(80, "Searching - will lock on when it appears", 1, th.select);
  else panelText(80, "Leave blank and press LOCK to clear an active lock", 1, th.textDim);
  if (s_toast[0] && (int32_t)(s_toastUntil - millis()) > 0) panelText(108, s_toast, 2, th.warn);
  panelPush();
  drawHeader("FIND AIRCRAFT", "", "", th.ok);
}

static void aircraftSearchEditTurn(int dir) {
  if (ae.pos >= 8) return;
  char& c = ae.chars[ae.pos];
  const char* p = strchr(AC_SEARCH_CHARSET, c);
  int idx = p ? (int)(p - AC_SEARCH_CHARSET) : 0;
  idx = (idx + AC_SEARCH_CHARSET_LEN + dir) % AC_SEARCH_CHARSET_LEN;
  c = AC_SEARCH_CHARSET[idx];
}

static void aircraftSearchEditPress(uint32_t now) {
  if (ae.pos < 8) { ae.pos++; return; }
  char q[9];
  int n = 0;
  for (int i = 0; i < 8; i++) if (ae.chars[i] != ' ') q[n++] = ae.chars[i];
  q[n] = 0;
  if (n == 0) {
    aircraftClearLock();
    toast("AIRCRAFT LOCK CLEARED", now, 2000);
  } else {
    aircraftLockSearch(q);
    toast("SEARCHING...", now, 2000);
  }
  closeToRadar();
}

// ===========================================================================
//  WiFi + About screens
// ===========================================================================
static void infoLine(int y, const char* label, const char* value, uint16_t vcol) {
  const Theme& th = themeCurrent();
  spRadar.setTextDatum(TL_DATUM);
  spRadar.setTextColor(th.textDim);
  spRadar.drawString(label, 10, y, 2);
  spRadar.setTextColor(vcol);
  spRadar.drawString(value, 112, y, 2);
}

static void drawWifiScreen() {
  const Theme& th = themeCurrent();
  char b[48];
  AdsbStatus st;
  adsbGetStatus(st);
  spRadar.fillSprite(th.bg);
  infoLine(12, "STATE", wifiStateName(), wifiIsConnected() ? th.ok : th.warn);
  infoLine(38, "NETWORK", g_settings.wifiSsid[0] ? g_settings.wifiSsid : "(none set)", th.text);
  wifiIpString(b, sizeof(b));
  infoLine(64, "IP ADDRESS", b, th.text);
  if (wifiIsConnected()) snprintf(b, sizeof(b), "%d dBm", wifiRssi()); else fr_copy(b, sizeof(b), "-");
  infoLine(90, "SIGNAL", b, th.text);
  snprintf(b, sizeof(b), "%u", (unsigned)wifiFailCount());
  infoLine(116, "ATTEMPTS FAILED", b, th.text);
  infoLine(142, "AIRCRAFT DATA", st.lastOk ? "OK" : (st.lastError[0] ? st.lastError : "NO DATA"), st.lastOk ? th.ok : th.warn);
  infoLine(168, "SOURCE", adsbSourceLabel(), th.text);
  if (wifiPortalActive()) {
    wifiPortalIpString(b, sizeof(b));
    spRadar.setTextDatum(TL_DATUM);
    spRadar.setTextColor(th.select);
    spRadar.drawString("SETUP AP ACTIVE", 10, 204, 2);
    char l[48];
    snprintf(l, sizeof(l), "1. JOIN WIFI: %s", wifiPortalSsid());
    spRadar.drawString(l, 10, 226, 2);
    snprintf(l, sizeof(l), "2. OPEN  http://%s", b);
    spRadar.drawString(l, 10, 246, 2);
  } else if (wifiIsConnected()) {
    // The settings page keeps running on the home network even once the
    // one-time setup AP has shut down - it's always reachable here, not
    // just while that AP is broadcasting.
    wifiIpString(b, sizeof(b));
    spRadar.setTextDatum(TL_DATUM);
    spRadar.setTextColor(th.textDim);
    spRadar.drawString("SETTINGS PAGE ALWAYS ON:", 10, 204, 2);
    char l[48];
    snprintf(l, sizeof(l), "http://%s/settings", b);
    spRadar.drawString(l, 10, 226, 2);
  }
  spRadar.pushSprite(0, RADAR_Y);
  panelBegin();
  panelText(14, wifiPortalActive() ? "PRESS: STOP SETUP AP" : "PRESS: START SETUP AP", 2, th.text);
  panelText(38, "BACK: UP     HOME / HOLD: EXIT", 2, th.text);
  panelPush();
  drawHeader("WIFI STATUS", "", "", statusDot());
}

static void drawAboutScreen(uint32_t now) {
  const Theme& th = themeCurrent();
  char b[48];
  spRadar.fillSprite(th.bg);
  snprintf(b, sizeof(b), FW_NAME " v" FW_VERSION);
  infoLine(12, "FIRMWARE", b, th.text);
  snprintf(b, sizeof(b), "%dx%d ST7796S", tft.width(), tft.height());
  infoLine(38, "DISPLAY", b, th.text);
  snprintf(b, sizeof(b), "%u KB", (unsigned)(ESP.getFreeHeap() / 1024));
  infoLine(64, "FREE HEAP", b, th.text);
  snprintf(b, sizeof(b), "%u KB of %u KB", (unsigned)(ESP.getFreePsram() / 1024), (unsigned)(ESP.getPsramSize() / 1024));
  infoLine(90, "FREE PSRAM", b, ESP.getPsramSize() ? th.text : th.warn);
  snprintf(b, sizeof(b), "%u-bit, %s", displaySpriteDepth(), displaySpritesInPsram() ? "PSRAM" : "internal RAM");
  infoLine(116, "SPRITES", b, th.text);
  snprintf(b, sizeof(b), "%u / %u", (unsigned)aircraftVisibleCount(), (unsigned)aircraftCount());
  infoLine(142, "SHOWN / TRACKED", b, th.text);
  snprintf(b, sizeof(b), "%u fps", (unsigned)s_fps);
  infoLine(168, "FRAME RATE", b, th.text);
  snprintf(b, sizeof(b), "%lu min", (unsigned long)(now / 60000UL));
  infoLine(194, "UPTIME", b, th.text);
  infoLine(220, "DATA SOURCE", adsbSourceLabel(), th.text);
  spRadar.pushSprite(0, RADAR_Y);
  panelBegin();
  panelText(14, "BACK: UP     HOME / HOLD: EXIT", 2, th.text);
  panelPush();
  drawHeader("ABOUT", "", "", th.ok);
}

// ===========================================================================
//  Train departure board
// ===========================================================================
static void drawTrainScreen() {
  const Theme& th = themeCurrent();
  TFT_eSprite& s = spRadar;
  s.fillSprite(th.bg);

  if (!trainIsConfigured()) {
    s.setTextDatum(MC_DATUM);
    s.setTextColor(th.warn);
    s.drawString("NO RAIL DATA API KEY CONFIGURED", SCREEN_W / 2, 100, 2);
    s.setTextColor(th.textDim);
    s.drawString("Set OPENLDBWS_API_KEY in config.h", SCREEN_W / 2, 130, 2);
  } else {
    TrainStatus st;
    trainGetStatus(st);
    TrainService rows[TRAIN_MAX_SERVICES];
    const uint16_t n = trainGetServices(rows, TRAIN_MAX_SERVICES);

    s.setTextDatum(TL_DATUM);
    s.setTextColor(th.text);
    char title[48];
    snprintf(title, sizeof(title), "%s",
             st.locationName[0] ? st.locationName : g_settings.trainStationCrs);
    s.drawString(title, 6, 4, 2);

    // The panel is only SCREEN_W (320) px wide, so each departure gets two
    // short lines rather than one cramped row: "HH:MM  PLAT n  STATUS" and
    // the destination name underneath (truncated if it doesn't fit).
    s.drawFastHLine(4, 22, SCREEN_W - 8, th.grid);

    if (!st.lastOk && n == 0) {
      s.setTextDatum(MC_DATUM);
      s.setTextColor(th.warn);
      s.drawString(st.lastError[0] ? st.lastError : "NO DATA YET", SCREEN_W / 2, 90, 2);
    } else if (n == 0) {
      s.setTextDatum(MC_DATUM);
      s.setTextColor(th.textDim);
      s.drawString("NO DEPARTURES DUE", SCREEN_W / 2, 90, 2);
    } else {
      const int rowH = 32;
      const uint16_t maxRows = (uint16_t)fr_clamp((RADAR_H - 28) / rowH, 1, (int)n);
      for (uint16_t i = 0; i < maxRows; i++) {
        const int y = 28 + i * rowH;
        const TrainService& r = rows[i];
        const bool cancelled = strcasecmp(r.etd, "Cancelled") == 0;
        const bool delayed = !cancelled && strcasecmp(r.etd, "On time") != 0 && strcasecmp(r.etd, "?") != 0;
        s.setTextDatum(TL_DATUM);
        s.setTextColor(cancelled ? th.textDim : th.text);
        s.drawString(r.std, 6, y, 2);
        char plat[12];
        snprintf(plat, sizeof(plat), "PL %s", r.platform);
        s.setTextColor(th.textDim);
        s.drawString(plat, 60, y + 3, 1);
        s.setTextColor(cancelled ? th.warn : delayed ? th.warn : th.ok);
        s.drawString(r.etd, 60, y + 14, 1);
        char dest[30];
        fr_copy(dest, sizeof(dest), r.destination);
        s.setTextColor(cancelled ? th.textDim : th.text);
        s.drawString(dest, 150, y + 6, 2);
        if (i + 1 < maxRows) s.drawFastHLine(4, y + rowH - 2, SCREEN_W - 8, th.gridDim);
      }
    }
    if (st.nrccMessage[0]) {
      s.setTextDatum(TL_DATUM);
      s.setTextColor(th.warn);
      char msg[46];
      fr_copy(msg, sizeof(msg), st.nrccMessage);
      s.drawString(msg, 6, RADAR_H - 12, 1);
    }
  }

  s.pushSprite(0, RADAR_Y);
  panelBegin();
  panelText(14, "PRESS: REFRESH   MENU: SETTINGS", 2, th.text);
  panelText(38, "BACK: BACK TO RADAR", 2, th.text);
  panelPush();
  drawHeader("TRAIN BOARD", "", "", trainIsConfigured() ? th.ok : th.warn);
}

static void drawRebootPrompt() {
  const Theme& th = themeCurrent();
  spRadar.fillSprite(th.bg);
  spRadar.setTextDatum(MC_DATUM);
  spRadar.setTextColor(th.warn);
  spRadar.drawString("REBOOT REQUIRED", SCREEN_W / 2, 75, 4);
  spRadar.setTextColor(th.text);
  spRadar.drawString("Reboot now to apply this setting?", SCREEN_W / 2, 125, 2);
  spRadar.drawString("Declining keeps it saved, but it", SCREEN_W / 2, 155, 2);
  spRadar.drawString("may not take effect until reboot.", SCREEN_W / 2, 180, 2);
  spRadar.pushSprite(0, RADAR_Y);
  panelBegin();
  panelText(20, "PRESS: REBOOT NOW", 2, th.select);
  panelText(48, "BACK: LATER (WARNING)", 2, th.warn);
  panelPush();
  drawHeader("REBOOT", "", "", th.warn);
}

// ===========================================================================
//  Boot screens
// ===========================================================================
static void drawBoot(uint32_t now) {
  const Theme& th = themeCurrent();
  char l1[40], l2[48], l3[48];
  l1[0] = l2[0] = l3[0] = 0;
  uint16_t c1 = th.text;

  switch (s_bootStage) {
    case 0:
      fr_copy(l1, sizeof(l1), "STARTING...");
      break;
    case 1:
      if (wifiState() == WIFI_ST_DISCONNECTED) {
        fr_copy(l1, sizeof(l1), "WIFI DISCONNECTED");
        c1 = th.warn;
        fr_copy(l2, sizeof(l2), "RETRYING...");
      } else {
        fr_copy(l1, sizeof(l1), "CONNECTING TO WIFI");
        snprintf(l2, sizeof(l2), "%s", g_settings.wifiSsid[0] ? g_settings.wifiSsid : "(no network set)");
      }
      break;
    case 2: {
      fr_copy(l1, sizeof(l1), "CONNECTED");
      char ip[20];
      wifiIpString(ip, sizeof(ip));
      snprintf(l2, sizeof(l2), "IP %s", ip);
      break;
    }
    default:
      fr_copy(l1, sizeof(l1), "Loading aircraft...");
      break;
  }
  if (wifiPortalActive()) {
    char ip[20];
    wifiPortalIpString(ip, sizeof(ip));
    snprintf(l2, sizeof(l2), "SETUP: JOIN '%s'", wifiPortalSsid());
    snprintf(l3, sizeof(l3), "OPEN http://%s", ip);
  }

  radarDrawBase(now, 20.0f, false);
  spRadar.fillRoundRect(40, RADAR_CY - 34, 240, 68, 8, th.bg);
  spRadar.drawRoundRect(40, RADAR_CY - 34, 240, 68, 8, th.ringOuter);
  spRadar.setTextDatum(MC_DATUM);
  spRadar.setTextColor(th.text);
  spRadar.drawString(FW_NAME, RADAR_CX, RADAR_CY - 10, 4);
  spRadar.setTextColor(th.textDim);
  spRadar.drawString("v" FW_VERSION, RADAR_CX, RADAR_CY + 18, 2);
  radarPush();

  if ((int32_t)(now - s_panelNext) >= 0 || s_panelDirty) {
    s_panelNext = now + 250;
    s_panelDirty = false;
    panelBegin();
    panelText(20, l1, 4, c1);
    if (l2[0]) panelText(60, l2, 2, th.text);
    if (l3[0]) panelText(82, l3, 2, th.text);
    panelPush();
  }
  drawHeader(FW_NAME, "", "", wifiIsConnected() ? th.ok : th.warn);
}

static void bootStep(uint32_t now) {
  AdsbStatus st;
  switch (s_bootStage) {
    case 0:
      if (now - s_bootT0 > 1500) {
        if (g_settings.wifiSsid[0] == 0) { closeToRadar(); return; }
        s_bootStage = 1; s_bootStageT = now;
      }
      break;
    case 1:
      if (wifiIsConnected()) { s_bootStage = 2; s_bootStageT = now; }
      else if (now - s_bootStageT > 25000) closeToRadar();
      break;
    case 2:
      if (now - s_bootStageT > 900) { s_bootStage = 3; s_bootStageT = now; }
      break;
    default:
      adsbGetStatus(st);
      if (st.everOk || st.failures > 0 || now - s_bootStageT > 12000) closeToRadar();
      break;
  }
}

// ===========================================================================
//  Radar screen
// ===========================================================================
static void drawRadarScreen(uint32_t now) {
  const bool emergency = aircraftEmergencyActive();
  const float range = settingsRangeNm();   // user's chosen range, always - see the comment in uiUpdate()
  radarRender(now, range);

  char left[24], right[12];
  char rng[16];
  if (emergency) {
    snprintf(left, sizeof(left), "%s %u", aircraftEmergencySquawk() == 7500 ? "HIJACK" : "EMERGENCY",
             (unsigned)aircraftEmergencySquawk());
  } else {
    fmtDistance(rng, sizeof(rng), range, true);
    snprintf(left, sizeof(left), "RANGE %s", rng);
  }
  time_t t = time(nullptr);
  clockFormat(right, sizeof(right), t);
  const uint16_t dot = emergency && ((now / 350) & 1) ? RGB565(255, 0, 0) :
                       emergency ? RGB565(255, 96, 96) : statusDot();
  char mid[16] = "";
  if (s_mode == MODE_SELECT) {
    fr_copy(mid, sizeof(mid), "FOLLOW");
  } else if (aircraftLockActive()) {
    snprintf(mid, sizeof(mid), "LOCK %s", aircraftLockQuery());
  } else if (aircraftLockPending()) {
    fr_copy(mid, sizeof(mid), "SEARCHING...");
  } else {
    const uint16_t milCount = aircraftVisibleMilitaryCount();
    const uint16_t svcCount = aircraftVisibleServiceCount();
    if (milCount > 0) snprintf(mid, sizeof(mid), "MIL %u", (unsigned)milCount);
    else if (svcCount > 0) snprintf(mid, sizeof(mid), "EMS %u", (unsigned)svcCount);
  }
  drawHeader(left, mid, right, dot);

  if (s_panelDirty || (int32_t)(now - s_panelNext) >= 0) {
    s_panelDirty = false;
    s_panelNext = now + 250;
    drawPanelRadar(now);
  }
}

// ===========================================================================
//  Public API
// ===========================================================================
void uiInit() {
  s_screen = SCR_BOOT;
  s_bootStage = 0;
  s_bootT0 = s_bootStageT = millis();
  adsbSetRange(RANGES_NM[g_settings.rangeIndex]);
  applyBrightness();
  s_dirty = true;
}

void uiHandleEvent(const InputEvent& ev, uint32_t now) {
  s_dirty = true;
  switch (s_screen) {
    case SCR_BOOT:
      // any button skips the start-up sequence
      if (ev.type == EVT_MENU || ev.type == EVT_BACK || ev.type == EVT_HOME || ev.type == EVT_ENC_SHORT)
        closeToRadar();
      return;

    case SCR_RADAR:
      s_panelDirty = true;
      switch (ev.type) {
        case EVT_ENC_CW:
        case EVT_ENC_CCW:
          if (s_mode == MODE_SELECT) aircraftCycle(ev.type == EVT_ENC_CW ? +1 : -1);
          else setRangeIndex(g_settings.rangeIndex + (ev.type == EVT_ENC_CW ? -1 : +1), now);  // CW = zoom in
          break;
        case EVT_ENC_SHORT:
          if (s_mode == MODE_ZOOM) {
            if (aircraftSelectNearest()) s_mode = MODE_SELECT;
            else toast("NO AIRCRAFT TO SELECT", now);
          } else {
            s_mode = MODE_ZOOM;                     // confirm: keep aircraft highlighted
          }
          break;
        case EVT_ENC_LONG:
        case EVT_MENU:
          openMenu();
          break;
        case EVT_BACK:
        case EVT_HOME:
          aircraftClearSelection();
          aircraftDismissEmergencyFollow();   // let go of a 7500/7700 auto-centre lock too, if one's active
          s_mode = MODE_ZOOM;
          break;
        default: break;
      }
      return;

    case SCR_TRAIN:
      switch (ev.type) {
        case EVT_ENC_SHORT:
          trainRequestNow();
          toast("REFRESHING...", now, 1500);
          break;
        case EVT_ENC_LONG:
        case EVT_MENU:
          openMenu();
          break;
        case EVT_BACK:
        case EVT_HOME:
          // quick way back to the radar without going through the settings menu
          s_trainMode = false;
          closeToRadar();
          break;
        default: break;
      }
      return;

    case SCR_TRAIN_STATION_EDIT:
      switch (ev.type) {
        case EVT_ENC_CW:    trainEditTurn(+1); break;
        case EVT_ENC_CCW:   trainEditTurn(-1); break;
        case EVT_ENC_SHORT: trainEditPress(now); break;
        case EVT_BACK:      s_screen = SCR_MENU; break;
        case EVT_ENC_LONG:
        case EVT_HOME:
        case EVT_MENU:      closeToRadar(); break;
        default: break;
      }
      return;

    case SCR_AIRCRAFT_SEARCH:
      switch (ev.type) {
        case EVT_ENC_CW:    aircraftSearchEditTurn(+1); break;
        case EVT_ENC_CCW:   aircraftSearchEditTurn(-1); break;
        case EVT_ENC_SHORT: aircraftSearchEditPress(now); break;
        case EVT_BACK:      s_screen = SCR_MENU; break;
        case EVT_ENC_LONG:
        case EVT_HOME:
        case EVT_MENU:      closeToRadar(); break;
        default: break;
      }
      return;

    case SCR_MENU: {
      const int n = s_page == 1 ? (int)N_UNITS : s_page == 2 ? (int)N_ADVANCED : (int)N_MAIN;
      const MenuItem* items = s_page == 1 ? UNIT_ITEMS : s_page == 2 ? ADVANCED_ITEMS : MAIN_ITEMS;
      uint8_t& cur = s_cursor[s_page];
      switch (ev.type) {
        case EVT_ENC_CW:
        case EVT_ENC_CCW: {
          const int dir = ev.type == EVT_ENC_CW ? +1 : -1;
          if (s_editing) itemAdjust(items[cur].id, dir, now);
          else cur = (uint8_t)((cur + n + dir) % n);
          break;
        }
        case EVT_ENC_SHORT: {
          const MenuItem& it = items[cur];
          if (it.editable) {
            if (s_editing) {
              const bool rebootChanged = (it.id == IT_SERIAL_BAUD || it.id == IT_AUDIO) &&
                                         itemGet(it.id) != s_editBackup;
              s_editing = false;
              settingsSave();
              if (it.id == IT_PORTAL_ENABLED && !g_settings.portalEnabled && wifiPortalActive())
                wifiStopPortal();
              if (rebootChanged) s_screen = SCR_REBOOT_PROMPT;
            }
            else { s_editing = true; s_editBackup = itemGet(it.id); }
          } else if (it.id == IT_UNITS) { s_page = 1; }
          else if (it.id == IT_ADVANCED) { s_page = 2; }
          else if (it.id == IT_HOME) { homeEditInit(); s_screen = SCR_HOME_EDIT; }
          else if (it.id == IT_TRAIN_STATION) { trainEditInit(); s_screen = SCR_TRAIN_STATION_EDIT; }
          else if (it.id == IT_FIND_AIRCRAFT) { aircraftSearchEditInit(); s_screen = SCR_AIRCRAFT_SEARCH; }
          else if (it.id == IT_WIFI) s_screen = SCR_WIFI;
          else if (it.id == IT_ABOUT) s_screen = SCR_ABOUT;
          else if (it.id == IT_PORTAL_NOW) {
            wifiStartPortal(true);
            s_screen = SCR_WIFI;
          }
          else if (it.id == IT_SPEAKER_TEST)
            toast(audioTestSpeaker() ? "CONFIGURED WAV TEST QUEUED" : "SPEAKER TEST UNAVAILABLE", now, 2500);
          else if (it.id == IT_SWITCH_MODE) {
            if (!s_trainMode && !trainIsConfigured())
              toast("TRAIN BOARD: NO API KEY CONFIGURED", now, 3000);
            else { s_trainMode = !s_trainMode; closeToRadar(); }
          }
          else if (it.id == IT_CHECK_UPDATE) {
            if (updateIsEnabled()) { updateCheckNow(); toast("CHECKING FOR UPDATE...", now, 2000); }
            else toast("UPDATE URL NOT CONFIGURED", now, 2500);
          }
          else if (it.id == IT_RESTORE_DEFAULTS) s_screen = SCR_DEFAULTS_CONFIRM;
          break;
        }
        case EVT_BACK:
          if (s_editing) { itemSet(items[cur].id, s_editBackup, now); s_editing = false; }
          else if (s_page) s_page = 0;
          else closeToRadar();
          break;
        case EVT_ENC_LONG:
        case EVT_HOME:
        case EVT_MENU:
          if (s_editing) {
            const bool rebootChanged = (items[cur].id == IT_SERIAL_BAUD || items[cur].id == IT_AUDIO) &&
                                       itemGet(items[cur].id) != s_editBackup;
            settingsSave();
            s_editing = false;
            if (rebootChanged) s_screen = SCR_REBOOT_PROMPT;
            else closeToRadar();
          } else closeToRadar();
          break;
        default: break;
      }
      return;
    }

    case SCR_HOME_EDIT:
      switch (ev.type) {
        case EVT_ENC_CW:    homeEditTurn(+1); break;
        case EVT_ENC_CCW:   homeEditTurn(-1); break;
        case EVT_ENC_SHORT: homeEditPress(now); break;
        case EVT_BACK:      s_screen = SCR_MENU; break;
        case EVT_ENC_LONG:
        case EVT_HOME:
        case EVT_MENU:      closeToRadar(); break;
        default: break;
      }
      return;

    case SCR_WIFI:
    case SCR_ABOUT:
      switch (ev.type) {
        case EVT_ENC_SHORT:
          if (s_screen == SCR_WIFI) { if (wifiPortalActive()) wifiStopPortal(); else wifiStartPortal(true); }
          break;
        case EVT_BACK:      s_screen = SCR_MENU; break;
        case EVT_ENC_LONG:
        case EVT_HOME:
        case EVT_MENU:      closeToRadar(); break;
        default: break;
      }
      return;

    case SCR_REBOOT_PROMPT:
      switch (ev.type) {
        case EVT_ENC_SHORT:
          ESP.restart();
          break;
        case EVT_BACK:
        case EVT_HOME:
        case EVT_MENU:
        case EVT_ENC_LONG:
          s_screen = SCR_MENU;
          toast("REBOOT LATER: SETTING MAY NOT APPLY", now, 4000);
          break;
        default: break;
      }
      return;

    case SCR_DEFAULTS_CONFIRM:
      switch (ev.type) {
        case EVT_ENC_SHORT: {
          const bool rebootNeeded = g_settings.serialBaud != SERIAL_BAUD ||
                                    g_settings.audioEnabled != AUDIO_ENABLED ||
                                    strcmp(g_settings.audioUrl, MILITARY_WAV_URL) != 0;
          settingsResetToUserDefaults();
          adsbSetRange(RANGES_NM[g_settings.rangeIndex]);
          wifiReconnectNow();
          applyBrightness();
          if (rebootNeeded) s_screen = SCR_REBOOT_PROMPT;
          else {
            s_screen = SCR_MENU;
            toast("DEFAULTS RESTORED; WIFI AND HOME CLEARED", now, 4000);
          }
          break;
        }
        case EVT_BACK:
        case EVT_HOME:
        case EVT_MENU:
        case EVT_ENC_LONG:
          s_screen = SCR_MENU;
          toast("RESET CANCELLED", now);
          break;
        default: break;
      }
      return;

    case SCR_UPDATE_PROMPT: {
      const FwUpdateState st = updateState();
      switch (ev.type) {
        case EVT_ENC_SHORT:
          if (st == FW_UPDATE_AVAILABLE) updateBeginInstall();
          else if (st == FW_UPDATE_ERROR) { updateDismiss(); closeToRadar(); }
          // DOWNLOADING: ignored - no cancelling mid-flash.
          break;
        case EVT_BACK:
        case EVT_HOME:
        case EVT_MENU:
        case EVT_ENC_LONG:
          if (st != FW_UPDATE_DOWNLOADING) { updateDismiss(); closeToRadar(); }
          break;
        default: break;
      }
      return;
    }

    case SCR_MILITARY_ALERT:
      // Any button skips the rest of the announcement early; it ends on its
      // own anyway once the clip finishes (see uiUpdate).
      switch (ev.type) {
        case EVT_ENC_SHORT:
        case EVT_ENC_LONG:
        case EVT_BACK:
        case EVT_HOME:
        case EVT_MENU:
          closeToRadar();
          break;
        default: break;
      }
      return;
  }
}

void uiUpdate(uint32_t now) {
  settingsService(now);

  static uint32_t lastSlow = 0;
  if (now - lastSlow > 1000) { lastSlow = now; applyBrightness(); }

  // Keep aircraft data fresh (merge / interpolate) about 25 times a second.
  if (now - s_lastAcUpdate >= 40) {
    s_lastAcUpdate = now;
    // Always use the range the user has actually chosen - an active 7500/
    // 7700 emergency used to force a fixed 5 nm view with no way to zoom out
    // or see what else was around; it now just takes over centring (see
    // aircraftDismissEmergencyFollow() for how the user can let go of that
    // too), range stays under normal control the whole time.
    aircraftUpdate(now, settingsRangeNm());
    if (aircraftEmergencyActive()) {
      const Aircraft* alert = aircraftSelected();
      const uint32_t key = alert ? alert->key : 0;
      const uint16_t squawk = aircraftEmergencySquawk();
      // Everything here - jumping to the radar screen, selecting/following
      // the aircraft, the tone and toast - fires exactly ONCE, the moment a
      // *new* emergency is detected (a different key, or the same aircraft
      // starting a fresh squawk). It deliberately does NOT run on every
      // frame any more: that used to force the screen back to SCR_RADAR and
      // force MODE_SELECT roughly 25 times a second for as long as the
      // emergency lasted, which made MENU/BACK/HOME and the encoder appear
      // completely dead - there was no way to even open the menu, because
      // whatever you pressed was undone a frame later. Now, once this has
      // fired, BACK/HOME (see aircraftDismissEmergencyFollow() in
      // aircraft.cpp) and the menu work normally again; the header alert and
      // 7700/7500 tone already only ever played once per emergency anyway.
      if (key && (key != s_alertedKey || squawk != s_alertedSquawk)) {
        s_alertedKey = key;
        s_alertedSquawk = squawk;
        if (squawk == 7700) audioPlayEmergency7700();
        else audioPlayEmergencyBeep();
        toast(squawk == 7500 ? "HIJACK SQUAWK 7500" : "EMERGENCY SQUAWK 7700", now, 5000);
        if (s_screen != SCR_RADAR) {
          // An active emergency squawk always wins, even over the train
          // board - it needs the radar screen specifically to show/follow
          // the aircraft, so this bypasses closeToRadar()'s train-mode
          // routing on purpose. Only done on the initial alert, not forced
          // back every frame afterwards.
          s_screen = SCR_RADAR;
          s_editing = false;
          s_dirty = true;
          s_panelDirty = true;
          s_hdrKey[0] = 0;
          trainSetActive(false);
        }
        s_mode = MODE_SELECT;
      }
    } else {
      s_alertedKey = 0;
      s_alertedSquawk = 0;
    }
    if (aircraftHasSelection()) {
      const Aircraft* a = aircraftSelected();
      if (!a || !a->visible) { aircraftClearSelection(); s_mode = MODE_ZOOM; s_panelDirty = true; }
    } else if (s_mode == MODE_SELECT) {
      s_mode = MODE_ZOOM;
    }
    updatePhotoSelection(aircraftSelected());
    updateRouteSelection(aircraftSelected());
  }
  receivePhotoResult();
  receiveRouteResult();

  // Only interrupt the idle radar screen, never menu navigation or another prompt.
  if (updateState() == FW_UPDATE_AVAILABLE && s_screen == SCR_RADAR) {
    s_screen = SCR_UPDATE_PROMPT;
    s_dirty = true;
  }

  // Military alert: stay up for exactly as long as the clip is actually playing
  // (with a safety cap in case the audio task never reports done). A short grace
  // period avoids racing the audio task, which hasn't necessarily set its "busy"
  // flag in the instant this screen first appears.
  if (s_screen == SCR_MILITARY_ALERT) {
    const uint32_t elapsed = (uint32_t)(now - s_milAlertStart);
    if ((elapsed > 300 && !audioClipBusy()) || elapsed > MIL_ALERT_MAX_MS) closeToRadar();
  }

  if (!displaySpritesOk()) return;

  const bool animated = (s_screen == SCR_BOOT || s_screen == SCR_RADAR);
  const uint32_t frameMs = 1000UL / g_settings.radarMaxFps;
  if (animated) {
    if (now - s_lastFrame < frameMs) return;
  } else if (!s_dirty && now - s_lastFrame < 500) {
    return;
  }
  s_lastFrame = now;
  s_dirty = false;

  if (++s_fpsCount >= 25) {
    s_fps = (uint8_t)(s_fpsCount * 1000UL / (now - s_fpsT0 + 1));
    s_fpsCount = 0;
    s_fpsT0 = now;
  }

  switch (s_screen) {
    case SCR_BOOT:      bootStep(now); if (s_screen == SCR_BOOT) drawBoot(now); break;
    case SCR_RADAR:     drawRadarScreen(now); break;
    case SCR_MENU:      drawMenu(); break;
    case SCR_HOME_EDIT: drawHomeEdit(); break;
    case SCR_WIFI:      drawWifiScreen(); break;
    case SCR_ABOUT:     drawAboutScreen(now); break;
    case SCR_REBOOT_PROMPT: drawRebootPrompt(); break;
    case SCR_DEFAULTS_CONFIRM: drawDefaultsConfirm(); break;
    case SCR_UPDATE_PROMPT: drawUpdatePrompt(); break;
    case SCR_MILITARY_ALERT: drawMilitaryAlert(); break;
    case SCR_TRAIN:          drawTrainScreen(); break;
    case SCR_TRAIN_STATION_EDIT: drawTrainEdit(); break;
    case SCR_AIRCRAFT_SEARCH:    drawAircraftSearchEdit(); break;
  }
}
