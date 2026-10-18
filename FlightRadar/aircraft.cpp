#include "aircraft.h"
#include "settings.h"
#include "geo.h"
#include "util.h"
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <math.h>
#include <string.h>
#include <ctype.h>

// ---------------------------------------------------------------------------
//  Storage: three arrays. s_list/s_scratch belong to the main loop only;
//  s_pending is the hand-over buffer written by the network task.
// ---------------------------------------------------------------------------
static Aircraft* s_list = nullptr;
static Aircraft* s_scratch = nullptr;
static Aircraft* s_pending = nullptr;
static uint16_t  s_count = 0;
static uint16_t  s_visible = 0;
static uint16_t  s_visibleMilitary = 0;
static uint16_t  s_pendingCount = 0;
static uint32_t  s_pendingHomeRev = 0;
static volatile bool s_pendingReady = false;
static SemaphoreHandle_t s_mutex = nullptr;
static uint32_t  s_selKey = 0;
static bool      s_hasSel = false;
static uint32_t  s_lastHomeRev = 0;
static uint32_t  s_emergencyKey = 0;
static uint16_t  s_emergencySquawk = 0;
static double    s_mapCenterLat = 0.0, s_mapCenterLon = 0.0;
static uint32_t  s_mapCenterKey = 0;

void* aircraftAllocLarge(size_t bytes) {
  void* p = nullptr;
  if (psramFound()) p = ps_malloc(bytes);
  if (!p) p = malloc(bytes);
  if (p) memset(p, 0, bytes);
  return p;
}

void aircraftInit() {
  const size_t bytes = sizeof(Aircraft) * MAX_AIRCRAFT;
  s_list = (Aircraft*)aircraftAllocLarge(bytes);
  s_scratch = (Aircraft*)aircraftAllocLarge(bytes);
  s_pending = (Aircraft*)aircraftAllocLarge(bytes);
  s_mutex = xSemaphoreCreateMutex();
  s_lastHomeRev = settingsHomeRev();
  if (!s_list || !s_scratch || !s_pending || !s_mutex) {
    Serial.println("[aircraft] out of memory - aircraft list disabled");
    s_list = s_scratch = s_pending = nullptr;
  }
}

Aircraft* aircraftList() { return s_list; }
uint16_t aircraftCount() { return s_list ? s_count : 0; }
uint16_t aircraftVisibleCount() { return s_visible; }
uint16_t aircraftVisibleMilitaryCount() { return s_visibleMilitary; }

void aircraftSubmit(const Aircraft* list, uint16_t count, uint32_t homeRev) {
  if (!s_pending || !s_mutex) return;
  if (count > MAX_AIRCRAFT) count = MAX_AIRCRAFT;
  if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(2000)) != pdTRUE) return;
  if (count) memcpy(s_pending, list, sizeof(Aircraft) * count);
  s_pendingCount = count;
  s_pendingHomeRev = homeRev;
  s_pendingReady = true;
  xSemaphoreGive(s_mutex);
}

// ---------------------------------------------------------------------------
//  Motion model
// ---------------------------------------------------------------------------
static inline float extrapSeconds(uint32_t now, uint32_t reportMs) {
  int32_t age = (int32_t)(now - reportMs);
  if (age < 0) age = 0;
  if (age > (int32_t)g_settings.aircraftMaxExtrapolationMs) age = g_settings.aircraftMaxExtrapolationMs;
  return age / 1000.0f;
}

static void displayPos(const Aircraft& a, uint32_t now, float& x, float& y) {
  const float dt = extrapSeconds(now, a.reportMs);
  x = a.x0 + a.vx * dt;
  y = a.y0 + a.vy * dt;
  int32_t since = (int32_t)(now - a.commitMs);
  if (since < 0) since = 0;
  if (g_settings.aircraftBlendMs && since < (int32_t)g_settings.aircraftBlendMs) {
    const float k = 1.0f - (float)since / (float)g_settings.aircraftBlendMs;
    x += a.offX * k;
    y += a.offY * k;
  }
}

static int findByKey(const Aircraft* list, uint16_t n, uint32_t key) {
  for (uint16_t i = 0; i < n; i++) if (list[i].key == key) return i;
  return -1;
}

static bool mergePending(uint32_t now) {
  if (!s_pendingReady) return false;
  if (xSemaphoreTake(s_mutex, 0) != pdTRUE) return false;   // network task is copying; try next frame
  s_pendingReady = false;
  const bool sameHome = (s_pendingHomeRev == settingsHomeRev());
  if (sameHome) {
    uint16_t n = 0;
    for (uint16_t i = 0; i < s_pendingCount && n < MAX_AIRCRAFT; i++) {
      const Aircraft& p = s_pending[i];
      Aircraft& d = s_scratch[n++];
      d = p;
      const int oi = findByKey(s_list, s_count, p.key);
      if (oi >= 0) {
        float ox, oy;
        displayPos(s_list[oi], now, ox, oy);
        const float dt = extrapSeconds(now, p.reportMs);
        const float nx = p.x0 + p.vx * dt, ny = p.y0 + p.vy * dt;
        d.offX = ox - nx;
        d.offY = oy - ny;
        if (sqrtf(d.offX * d.offX + d.offY * d.offY) > 3.0f) d.offX = d.offY = 0;   // big jump: snap
        d.commitMs = now;
      } else {
        d.offX = d.offY = 0;
        d.commitMs = now - g_settings.aircraftBlendMs;
      }
    }
    Aircraft* t = s_list; s_list = s_scratch; s_scratch = t;
    s_count = n;
  }
  xSemaphoreGive(s_mutex);
  return sameHome;
}

// ---------------------------------------------------------------------------
//  Military announcement (one-shot per aircraft, with a cooldown so the same
//  airframe re-detected a few minutes later - e.g. after a signal dropout -
//  doesn't spam the speaker on every pass of a slow orbit).
// ---------------------------------------------------------------------------
struct AnnounceEntry { uint32_t key; uint32_t untilMs; };
static AnnounceEntry s_announceHist[16];
static uint8_t       s_announcePending = 0;

static bool announceOnCooldown(uint32_t key, uint32_t now) {
  for (auto& e : s_announceHist) if (e.key == key) return (int32_t)(e.untilMs - now) > 0;
  return false;
}
static void announceRemember(uint32_t key, uint32_t now) {
  uint8_t oldest = 0;
  for (uint8_t i = 0; i < 16; i++) {
    if (s_announceHist[i].key == key || s_announceHist[i].key == 0) { oldest = i; break; }
    if (s_announceHist[i].untilMs < s_announceHist[oldest].untilMs) oldest = i;
  }
  s_announceHist[oldest] = { key, now + g_settings.announceCooldownMs };
}

bool aircraftPollAnnouncement() {
  if (s_announcePending == 0) return false;
  s_announcePending--;
  return true;
}

bool aircraftEmergencyActive() { return s_emergencyKey != 0; }
uint16_t aircraftEmergencySquawk() { return s_emergencySquawk; }

void aircraftUpdate(uint32_t now, float rangeNm) {
  if (!s_list) return;

  if (s_lastHomeRev != settingsHomeRev()) {       // home moved: old data is meaningless
    s_lastHomeRev = settingsHomeRev();
    s_count = 0;
    s_hasSel = false;
  }
  const bool listChanged = mergePending(now);

  const Aircraft* emergency = nullptr;
  for (uint16_t i = 0; i < s_count; ++i) {
    const Aircraft& a = s_list[i];
    if ((uint32_t)(now - a.reportMs) > g_settings.aircraftMaxAgeMs) continue;
    if (a.squawk != 7500 && a.squawk != 7700) continue;
    if (!emergency || a.squawk == 7700) emergency = &a;
    if (emergency->squawk == 7700) break;
  }

  const uint32_t nextEmergencyKey = emergency ? emergency->key : 0;
  const uint16_t nextEmergencySquawk = emergency ? emergency->squawk : 0;
  const double nextCenterLat = emergency ? emergency->latitude : g_settings.homeLat;
  const double nextCenterLon = emergency ? emergency->longitude : g_settings.homeLon;
  const bool centerChanged = nextEmergencyKey != s_mapCenterKey ||
    fabs(nextCenterLat - s_mapCenterLat) > 0.000001 ||
    fabs(nextCenterLon - s_mapCenterLon) > 0.000001;
  if (centerChanged || listChanged) {
    for (uint16_t i = 0; i < s_count; ++i) {
      geoToEastNorth(nextCenterLat, nextCenterLon, s_list[i].latitude, s_list[i].longitude,
                     &s_list[i].x0, &s_list[i].y0);
      s_list[i].x = s_list[i].x0;
      s_list[i].y = s_list[i].y0;
    }
  }
  if (s_emergencyKey && !nextEmergencyKey && s_hasSel && s_selKey == s_emergencyKey)
    s_hasSel = false;
  s_emergencyKey = nextEmergencyKey;
  s_emergencySquawk = nextEmergencySquawk;
  s_mapCenterKey = nextEmergencyKey;
  s_mapCenterLat = nextCenterLat;
  s_mapCenterLon = nextCenterLon;
  if (emergency) {
    s_selKey = emergency->key;
    s_hasSel = true;
  }

  uint16_t w = 0, vis = 0, visMil = 0;
  bool selFound = false;
  for (uint16_t i = 0; i < s_count; i++) {
    Aircraft a = s_list[i];
    const int32_t age = (int32_t)(now - a.reportMs);
    if (age > (int32_t)g_settings.aircraftMaxAgeMs) continue;          // stale: drop
    displayPos(a, now, a.x, a.y);
    a.distance = sqrtf(a.x * a.x + a.y * a.y);
    if (a.distance > 0.0001f) {
      float b = atan2f(a.x, a.y) * 57.29577951f;
      if (b < 0) b += 360.0f;
      a.bearing = b;
    } else {
      a.bearing = 0;                                        // exactly at centre: no division
    }
    const bool emergency = a.squawk == 7500 || a.squawk == 7700;
    a.visible = (a.distance <= rangeNm || emergency) &&
                (g_settings.showGroundAircraft || !a.onGround || emergency);
    a.selected = s_hasSel && a.key == s_selKey;
    if (a.selected) selFound = true;
    if (a.visible) vis++;
    if (a.visible && a.military) visMil++;
    if (a.visible && a.military && !announceOnCooldown(a.key, now)) {
      announceRemember(a.key, now);
      if (s_announcePending < 255) s_announcePending++;
    }
    s_list[w++] = a;
  }
  s_count = w;
  s_visible = vis;
  s_visibleMilitary = visMil;
  if (s_hasSel && !selFound) s_hasSel = false;              // selected aircraft vanished
}

// ---------------------------------------------------------------------------
//  Selection
// ---------------------------------------------------------------------------
bool aircraftHasSelection() { return s_hasSel; }
void aircraftClearSelection() { s_hasSel = false; }

const Aircraft* aircraftSelected() {
  if (!s_hasSel || !s_list) return nullptr;
  for (uint16_t i = 0; i < s_count; i++) if (s_list[i].key == s_selKey) return &s_list[i];
  return nullptr;
}

bool aircraftSelectNearest() {
  int best = -1;
  float bd = 1e9f;
  for (uint16_t i = 0; s_list && i < s_count; i++) {
    if (s_list[i].visible && s_list[i].distance < bd) { bd = s_list[i].distance; best = i; }
  }
  if (best < 0) return false;
  s_selKey = s_list[best].key;
  s_hasSel = true;
  return true;
}

bool aircraftCycle(int dir) {
  if (!s_list) return false;
  static uint16_t order[MAX_AIRCRAFT];
  uint16_t n = 0;
  for (uint16_t i = 0; i < s_count; i++) {
    if (!s_list[i].visible) continue;
    uint16_t j = n++;
    while (j > 0 && s_list[order[j - 1]].distance > s_list[i].distance) { order[j] = order[j - 1]; j--; }
    order[j] = i;                                            // insertion sort by distance
  }
  if (n == 0) return false;
  int pos = -1;
  for (uint16_t k = 0; k < n; k++) if (s_hasSel && s_list[order[k]].key == s_selKey) { pos = k; break; }
  int next;
  if (pos < 0) next = 0;
  else next = (pos + (dir >= 0 ? 1 : -1) + (int)n) % (int)n;
  s_selKey = s_list[order[next]].key;
  s_hasSel = true;
  return true;
}

// ---------------------------------------------------------------------------
//  Classification
// ---------------------------------------------------------------------------
static bool inList(const char* s, const char* const* list, size_t n) {
  for (size_t i = 0; i < n; i++) if (!strcmp(s, list[i])) return true;
  return false;
}
static bool prefixIn(const char* s, const char* const* list, size_t n) {
  for (size_t i = 0; i < n; i++) if (!strncmp(s, list[i], strlen(list[i]))) return true;
  return false;
}
#define N(a) (sizeof(a) / sizeof((a)[0]))

static const char* const HELI_TYPES[] = {
  "R22","R44","R66","EC20","EC30","EC35","EC45","EC55","EC75","AS32","AS50","AS55","AS65",
  "B06","B06T","B407","B412","B429","B430","B47G","S76","S92","S61","S64","A109","A119",
  "A139","A169","A189","H60","H47","H64","H500","MI8","MI24","KA32","CH47","UH1","NH90",
  "LYNX","AH64","EH10","MD52","MD60","EN28","H160","H135","H145","GAZL","B105"
};
static const char* const FAST_JET_TYPES[] = {
  "F16","F15","F18","F35","F22","EUFI","TORN","A10","HAWK","T38","FA18","M346","GR4","F5","F14","MIRA","RFAL","GRIF","SB39"
};
// Ex-military "warbirds" that are now civil-registered and flown on the
// airshow/flypast circuit - the RAF's Battle of Britain Memorial Flight
// (Spitfire, Hurricane, Lancaster, Dakota) and common privately-owned
// ex-military types. Best-effort ICAO type codes: if a real flyover doesn't
// trigger the announcement, set DEBUG_PRINT_AIRCRAFT_TYPES to 1 in config.h,
// watch the Serial Monitor for the exact type code your feed reports, and add
// it here.
static const char* const HISTORIC_MIL_TYPES[] = {
  "SPIT","HURI","LANC","DC3","C47","VULC","MOSQ","P51","P47","P40","B17","B25","T6",
  "YK52","YAK52","L39","CORS","SEAF","HAVI","VAMP","MEET","JPRO","CATA","AT6",
};
static const char* const AIRLINER_PREFIX[] = {
  "A2","A3","B73","B74","B75","B76","B77","B78","B37","B38","B39","E17","E19","E29","E75","CRJ","AT7","AT4","DH8","BCS","A1","MD8","MD9","F70","F100","SF34","BE20"
};
static const char* const LIGHT_PREFIX[] = {
  "C1","C2","PA","DA4","DA2","DR4","SR2","P28","AA5","GLID","G1","TB","RV","ULAC","BE3","BE5","M20","PC12","TOBA","SF2","EV97","AC","J3","C77"
};

static bool airlinePattern(const char* cs) {
  return cs && isalpha((uint8_t)cs[0]) && isalpha((uint8_t)cs[1]) && isalpha((uint8_t)cs[2]) && isdigit((uint8_t)cs[3]);
}

struct OpEntry { const char* code; const char* name; bool mil; };
static const OpEntry OPERATORS[] = {
  {"BAW","British Airways",false},{"SHT","BA Shuttle",false},{"EZY","easyJet",false},{"EZS","easyJet Switzerland",false},
  {"EJU","easyJet Europe",false},{"RYR","Ryanair",false},{"RUK","Ryanair UK",false},{"VIR","Virgin Atlantic",false},
  {"LOG","Loganair",false},{"EXS","Jet2",false},{"TOM","TUI Airways",false},{"TUI","TUI",false},{"DLH","Lufthansa",false},
  {"AFR","Air France",false},{"KLM","KLM",false},{"IBE","Iberia",false},{"SAS","SAS",false},{"UAE","Emirates",false},
  {"QTR","Qatar Airways",false},{"ETD","Etihad",false},{"SIA","Singapore Airlines",false},{"CPA","Cathay Pacific",false},
  {"ANA","All Nippon",false},{"JAL","Japan Airlines",false},{"AAL","American Airlines",false},{"DAL","Delta",false},
  {"UAL","United",false},{"ACA","Air Canada",false},{"WZZ","Wizz Air",false},{"VLG","Vueling",false},{"NAX","Norwegian",false},
  {"FIN","Finnair",false},{"SWR","Swiss",false},{"AUA","Austrian",false},{"BEL","Brussels Airlines",false},{"TAP","TAP Portugal",false},
  {"LOT","LOT Polish",false},{"THY","Turkish Airlines",false},{"ELY","El Al",false},{"ETH","Ethiopian",false},{"SAA","South African",false},
  {"QFA","Qantas",false},{"ICE","Icelandair",false},{"EIN","Aer Lingus",false},{"CFE","BA CityFlyer",false},{"BCS","European Air Charter",false},
  {"FDX","FedEx",false},{"UPS","UPS",false},{"GEC","Lufthansa Cargo",false},{"CLX","Cargolux",false},
  {"ASCOT","RAF",true},{"RRR","RAF",true},{"RFR","French Air Force",true},{"GAF","German Air Force",true},
  {"NOH","US Navy",true},{"RCH","US Air Mobility",true},{"NAF","US Navy",true},{"CFC","Canadian Armed Forces",true},
};

static const OpEntry* operatorLookup(const char* cs) {
  if (!cs || strlen(cs) < 3) return nullptr;
  for (size_t i = 0; i < N(OPERATORS); i++) {
    size_t l = strlen(OPERATORS[i].code);
    if (!strncmp(cs, OPERATORS[i].code, l) && (l == 3 ? isdigit((uint8_t)cs[3]) : true)) return &OPERATORS[i];
  }
  return nullptr;
}

AircraftClass aircraftClassify(const char* category, const char* type, const char* callsign,
                               bool dbFlagMilitary, bool* outMilitary) {
  if (!category) category = "";
  if (!type) type = "";
  const OpEntry* op = operatorLookup(callsign);
  const bool mil = dbFlagMilitary || !strcmp(category, "A6") ||
                  inList(type, FAST_JET_TYPES, N(FAST_JET_TYPES)) ||
                  inList(type, HISTORIC_MIL_TYPES, N(HISTORIC_MIL_TYPES)) ||
                  (op && op->mil);
  if (outMilitary) *outMilitary = mil;
  if (mil) return AC_MILITARY;                    // one bucket for any current or ex-military airframe

  const bool heli = !strcmp(category, "A7") || inList(type, HELI_TYPES, N(HELI_TYPES));
  if (heli) return AC_HELICOPTER;
  if (!strcmp(category, "A3") || !strcmp(category, "A4") || !strcmp(category, "A5")) return AC_AIRLINER;
  if (!strcmp(category, "A2")) return airlinePattern(callsign) ? AC_AIRLINER : AC_LIGHT;
  if (!strcmp(category, "A1") || !strcmp(category, "B1") || !strcmp(category, "B2") ||
      !strcmp(category, "B4") || !strcmp(category, "B6")) return AC_LIGHT;
  if (type[0]) {
    if (prefixIn(type, AIRLINER_PREFIX, N(AIRLINER_PREFIX)) && airlinePattern(callsign)) return AC_AIRLINER;
    if (prefixIn(type, LIGHT_PREFIX, N(LIGHT_PREFIX))) return AC_LIGHT;
    if (prefixIn(type, AIRLINER_PREFIX, N(AIRLINER_PREFIX))) return AC_AIRLINER;
  }
  if (airlinePattern(callsign)) return AC_AIRLINER;
  return AC_UNKNOWN;
}

const char* aircraftOperatorFromCallsign(const char* cs) {
  const OpEntry* op = operatorLookup(cs);
  return op ? op->name : nullptr;
}

const char* aircraftDisplayName(const Aircraft& a) {
  if (a.callsign[0]) return a.callsign;
  if (a.registration[0]) return a.registration;
  return a.icao;
}
