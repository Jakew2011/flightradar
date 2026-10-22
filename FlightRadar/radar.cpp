#include "radar.h"
#include "config.h"
#include "display.h"
#include "aircraft.h"
#include "icons.h"
#include "settings.h"
#include "geo.h"
#include <Arduino.h>
#include <math.h>
#include <string.h>

static const uint16_t EMERGENCY_RED = RGB565(255, 0, 0);

// Ring radii (nm) for each zoom level. The last ring always equals the range.
static const float RING_NM[RANGE_COUNT][3] = {
  {1, 2.5f, 5}, {2, 5, 10}, {5, 10, 20}, {10, 20, 30}, {10, 25, 50},
  {25, 50, 75}, {25, 50, 100}, {50, 100, 150}, {50, 100, 200},
};

static float    s_sweepDeg = 0;
static uint32_t s_lastMs = 0;

uint16_t radarSweepPeriodMs() {
  switch (g_settings.sweepSpeed) {
    case 0: return 6000;
    case 1: return g_settings.radarSweepPeriodMs;
    case 2: return 2500;
    default: return 0;
  }
}

static inline void polar(float deg, float r, int& x, int& y) {
  const float a = deg * 0.017453292f;
  x = RADAR_CX + (int)lroundf(r * sinf(a));
  y = RADAR_CY - (int)lroundf(r * cosf(a));
}

static void advanceSweep(uint32_t now) {
  uint32_t dt = now - s_lastMs;
  s_lastMs = now;
  if (dt > 500) dt = 500;
  const uint16_t period = radarSweepPeriodMs();
  if (!period) return;
  s_sweepDeg += 360.0f * (float)dt / (float)period;
  if (s_sweepDeg >= 360.0f) s_sweepDeg -= 360.0f;
}

static void drawSweep(const Theme& th) {
  if (!radarSweepPeriodMs()) return;
  TFT_eSprite& s = spRadar;
  const int N = g_settings.sweepTrailSegments;
  const float step = (float)g_settings.sweepTrailDegrees / N;
  for (int k = N - 1; k >= 0; --k) {                    // oldest (dimmest) first
    const float a1 = s_sweepDeg - k * step;
    const float a0 = a1 - step - 0.8f;                  // small overlap avoids seams
    const float fresh = 1.0f - (float)k / N;            // 1 = newest
    const float f = 0.05f + 0.33f * fresh * fresh;
    const uint16_t col = RGB565((int)(th.sweepR * f), (int)(th.sweepG * f), (int)(th.sweepB * f));
    int x0, y0, x1, y1;
    polar(a0, RADAR_R, x0, y0);
    polar(a1, RADAR_R, x1, y1);
    s.fillTriangle(RADAR_CX, RADAR_CY, x0, y0, x1, y1, col);
  }
  int lx, ly;
  polar(s_sweepDeg, RADAR_R, lx, ly);
  const uint16_t lead = RGB565(th.sweepR * 3 / 4, th.sweepG * 3 / 4, th.sweepB * 3 / 4);
  s.drawLine(RADAR_CX, RADAR_CY, lx, ly, lead);
}

static void drawGrid(const Theme& th, float rangeNm) {
  TFT_eSprite& s = spRadar;
  uint8_t ri = settingsRangeIndexFor((uint16_t)lroundf(rangeNm));
  s.drawFastHLine(RADAR_CX - RADAR_R, RADAR_CY, 2 * RADAR_R + 1, th.gridDim);
  s.drawFastVLine(RADAR_CX, RADAR_CY - RADAR_R, 2 * RADAR_R + 1, th.gridDim);

  s.setTextDatum(BL_DATUM);
  for (int i = 0; i < 3; i++) {
    const float nm = RING_NM[ri][i];
    const int rpx = (int)lroundf(nm / rangeNm * RADAR_R);
    if (rpx < 3) continue;
    const bool outer = (i == 2);
    s.drawCircle(RADAR_CX, RADAR_CY, rpx, outer ? th.ringOuter : th.grid);
    if (outer) s.drawCircle(RADAR_CX, RADAR_CY, rpx - 1, th.ringOuter);
    char buf[12];
    fmtDistance(buf, sizeof(buf), nm, false);
    const int lx = RADAR_CX + (int)(rpx * 0.7071f) + 2;
    const int ly = RADAR_CY - (int)(rpx * 0.7071f) - 1;
    s.setTextColor(th.textDim);
    s.drawString(buf, lx, ly, 1);
  }
  // bearing ticks every 10 degrees, longer every 30
  for (int d = 0; d < 360; d += 10) {
    const int len = (d % 30 == 0) ? 8 : 4;
    int xa, ya, xb, yb;
    polar((float)d, RADAR_R - len, xa, ya);
    polar((float)d, RADAR_R, xb, yb);
    s.drawLine(xa, ya, xb, yb, th.ringOuter);
  }
  // home marker
  s.drawCircle(RADAR_CX, RADAR_CY, 3, th.text);
  s.drawPixel(RADAR_CX, RADAR_CY, th.text);
}

static void drawCompass(const Theme& th) {
  TFT_eSprite& s = spRadar;
  s.setTextDatum(MC_DATUM);
  const int r = RADAR_R + 11;
  s.setTextColor(th.text);
  s.drawString("N", RADAR_CX, RADAR_CY - r, 2);
  s.setTextColor(th.textDim);
  s.drawString("E", RADAR_CX + r, RADAR_CY, 2);
  s.drawString("S", RADAR_CX, RADAR_CY + r, 2);
  s.drawString("W", RADAR_CX - r, RADAR_CY, 2);
}

void radarDrawBase(uint32_t nowMs, float rangeNm, bool compass) {
  if (!displaySpritesOk()) return;
  const Theme& th = themeCurrent();
  advanceSweep(nowMs);
  spRadar.fillSprite(th.bg);
  drawSweep(th);
  drawGrid(th, rangeNm);
  if (compass) drawCompass(th);
}

// ---------------------------------------------------------------------------
//  Aircraft + labels
// ---------------------------------------------------------------------------
struct LRect { int16_t x0, y0, x1, y1; };
static LRect   s_lab[LABEL_MAX + 1];
static uint8_t s_nlab;
static int16_t s_px[MAX_AIRCRAFT], s_py[MAX_AIRCRAFT];
static uint8_t s_done[(MAX_AIRCRAFT + 7) / 8];

static inline bool hit(const LRect& a, const LRect& b) {
  return !(a.x1 < b.x0 || a.x0 > b.x1 || a.y1 < b.y0 || a.y0 > b.y1);
}

static bool placeLabel(const Aircraft* list, uint16_t n, uint16_t self, const char* text,
                       uint16_t colour, bool force) {
  if (s_nlab >= g_settings.labelMax && !force) return false;
  const int w = (int)strlen(text) * 6, h = 8;
  const int x = s_px[self], y = s_py[self];
  const int cx[4] = { x + 12, x - 12 - w, x - w / 2, x - w / 2 };
  const int cy[4] = { y - 4,  y - 4,      y - 22,    y + 12 };
  for (int c = 0; c < 4; c++) {
    LRect r = { (int16_t)cx[c], (int16_t)cy[c], (int16_t)(cx[c] + w), (int16_t)(cy[c] + h) };
    if (r.x0 < 0 || r.y0 < 0 || r.x1 >= SCREEN_W || r.y1 >= RADAR_H) continue;
    bool bad = false;
    for (uint8_t i = 0; i < s_nlab && !bad; i++) bad = hit(r, s_lab[i]);
    for (uint16_t i = 0; i < n && !bad; i++) {
      if (i == self || !list[i].visible) continue;
      if (s_px[i] >= r.x0 - 8 && s_px[i] <= r.x1 + 8 && s_py[i] >= r.y0 - 8 && s_py[i] <= r.y1 + 8) bad = true;
    }
    if (bad && !force) continue;
    spRadar.setTextDatum(TL_DATUM);
    spRadar.setTextColor(colour);
    spRadar.drawString(text, r.x0, r.y0, 1);
    if (s_nlab < LABEL_MAX) s_lab[s_nlab++] = r;
    return true;
  }
  return false;
}

void radarDrawAircraft(float rangeNm) {
  if (!displaySpritesOk()) return;
  const Theme& th = themeCurrent();
  Aircraft* list = aircraftList();
  const uint16_t n = aircraftCount();
  if (!list || n == 0) return;
  const float ppn = (float)RADAR_R / rangeNm;

  int sel = -1;
  for (uint16_t i = 0; i < n; i++) {
    if (!list[i].visible) continue;
    s_px[i] = RADAR_CX + (int16_t)lroundf(list[i].x * ppn);
    s_py[i] = RADAR_CY - (int16_t)lroundf(list[i].y * ppn);
    if (list[i].selected) { sel = i; continue; }
    iconDraw(spRadar, list[i].cls, s_px[i], s_py[i], list[i].hasHeading ? list[i].heading : 0.0f,
             (list[i].squawk == 7500 || list[i].squawk == 7700) ? EMERGENCY_RED :
               list[i].military ? th.military : th.aircraft, g_settings.aircraftIconScale);
  }
  if (sel >= 0) {
    const Aircraft& a = list[sel];
    iconDraw(spRadar, a.cls, s_px[sel], s_py[sel], a.hasHeading ? a.heading : 0.0f, th.select,
             g_settings.aircraftIconScale * 1.3f);
    spRadar.drawCircle(s_px[sel], s_py[sel], 14, th.select);
    spRadar.drawCircle(s_px[sel], s_py[sel], 15, th.select);
    if (a.squawk == 7500 || a.squawk == 7700) {
      const uint16_t alertColor = ((millis() / 350) & 1) ? EMERGENCY_RED : RGB565(255, 96, 96);
      spRadar.drawCircle(s_px[sel], s_py[sel], 18, alertColor);
      spRadar.drawCircle(s_px[sel], s_py[sel], 20, alertColor);
    }
  }

  if (g_settings.labelsEnabled) {
    s_nlab = 0;
    memset(s_done, 0, sizeof(s_done));
    if (sel >= 0) {
      placeLabel(list, n, sel, aircraftDisplayName(list[sel]), th.select, true);
      s_done[sel >> 3] |= (1 << (sel & 7));
    }
    for (int tries = 0; tries < g_settings.labelMax + 8; tries++) {       // nearest first
      int best = -1;
      float bd = 1e9f;
      for (uint16_t i = 0; i < n; i++) {
        if (!list[i].visible || (s_done[i >> 3] & (1 << (i & 7)))) continue;
        if (list[i].distance < bd) { bd = list[i].distance; best = i; }
      }
      if (best < 0) break;
      s_done[best >> 3] |= (1 << (best & 7));
      placeLabel(list, n, best, aircraftDisplayName(list[best]), list[best].military ? th.military : th.text, false);
    }
  }

  if (aircraftEmergencyActive()) {
    const bool bright = ((millis() / 350) & 1) == 0;
    spRadar.fillRoundRect(54, 4, 212, 20, 4, bright ? EMERGENCY_RED : RGB565(110, 0, 0));
    spRadar.setTextDatum(MC_DATUM);
    spRadar.setTextColor(TFT_WHITE);
    spRadar.drawString(aircraftEmergencySquawk() == 7500 ? "HIJACK SQUAWK 7500" :
                       "EMERGENCY SQUAWK 7700", RADAR_CX, 14, 1);
  }
}

// ---------------------------------------------------------------------------
//  Airports/airfields - simple point markers, not real coastline/lake
//  outlines (not practical to embed real vector map data on this hardware -
//  this is the scoped-down version of that request). Best-effort, UK-centric
//  list of major commercial airports plus a couple of general-aviation
//  fields, written without being able to test against real hardware. Add
//  more yourself in the same {code, lat, lon} format if you want finer
//  coverage near you.
// ---------------------------------------------------------------------------
struct AirportEntry { const char* code; double lat, lon; };
static const AirportEntry AIRPORTS[] = {
  {"LHR", 51.4700, -0.4543}, {"LGW", 51.1481, -0.1903}, {"STN", 51.8860,  0.2389},
  {"LTN", 51.8747, -0.3683}, {"LCY", 51.5053,  0.0553}, {"MAN", 53.3537, -2.2750},
  {"BHX", 52.4539, -1.7480}, {"EDI", 55.9500, -3.3725}, {"GLA", 55.8719, -4.4331},
  {"BRS", 51.3827, -2.7191}, {"LBA", 53.8659, -1.6606}, {"LPL", 53.3336, -2.8497},
  {"NCL", 55.0375, -1.6917}, {"EMA", 52.8311, -1.3281}, {"SOU", 50.9503, -1.3568},
  {"NWI", 52.6758,  1.2828}, {"EXT", 50.7346, -3.4139}, {"CWL", 51.3967, -3.3433},
  {"BFS", 54.6575, -6.2158}, {"ABZ", 57.2019, -2.1978}, {"ESH", 50.8356, -0.2972},  // Shoreham
  {"BQH", 51.3308,  0.0325},  // Biggin Hill
  {"FAB", 51.2758, -0.7764},  // Farnborough
  {"BOH", 50.7800, -1.8425},  // Bournemouth
};
#define N_AIRPORTS (sizeof(AIRPORTS) / sizeof(AIRPORTS[0]))

void radarDrawAirports(float rangeNm) {
  if (!displaySpritesOk() || !SHOW_AIRPORTS) return;
  const Theme& th = themeCurrent();
  double centerLat, centerLon;
  aircraftMapCenter(centerLat, centerLon);
  const float ppn = (float)RADAR_R / rangeNm;
  TFT_eSprite& s = spRadar;
  s.setTextDatum(MC_DATUM);
  for (size_t i = 0; i < N_AIRPORTS; i++) {
    float eastNm, northNm;
    geoToEastNorth(centerLat, centerLon, AIRPORTS[i].lat, AIRPORTS[i].lon, &eastNm, &northNm);
    if (sqrtf(eastNm * eastNm + northNm * northNm) > rangeNm) continue;
    const int px = RADAR_CX + (int)lroundf(eastNm * ppn);
    const int py = RADAR_CY - (int)lroundf(northNm * ppn);
    s.drawRect(px - 3, py - 3, 7, 7, th.gridDim);
    s.setTextColor(th.textDim);
    s.drawString(AIRPORTS[i].code, px, py - 11, 1);
  }
}

void radarPush() {
  if (displaySpritesOk()) spRadar.pushSprite(0, RADAR_Y);
}

void radarRender(uint32_t nowMs, float rangeNm) {
  radarDrawBase(nowMs, rangeNm, true);
  radarDrawAirports(rangeNm);
  radarDrawAircraft(rangeNm);
  radarPush();
}
