#include "adsb_api.h"
#include "aircraft.h"
#include "settings.h"
#include "wifi_manager.h"
#include "geo.h"
#include "util.h"
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <math.h>
#include <string.h>
#include <strings.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#if !defined(ARDUINOJSON_VERSION_MAJOR) || ARDUINOJSON_VERSION_MAJOR < 7
  #error "ArduinoJson version 7 is required. Install it with Tools > Manage Libraries > ArduinoJson (by Benoit Blanchon)."
#endif

static portMUX_TYPE s_statMux = portMUX_INITIALIZER_UNLOCKED;
static AdsbStatus   s_status;
static volatile uint16_t s_rangeNm = DEFAULT_RANGE;
static volatile bool     s_force = false;

static void setStatus(bool ok, int http, uint16_t count, const char* err) {
  portENTER_CRITICAL(&s_statMux);
  s_status.lastOk = ok;
  s_status.lastHttpCode = http;
  if (ok) {
    s_status.everOk = true;
    s_status.lastCount = count;
    s_status.lastSuccessMs = millis();
    time_t t = time(nullptr);
    s_status.lastSuccessEpoch = (t > 1600000000) ? t : 0;
    s_status.failures = 0;
    s_status.lastError[0] = 0;
  } else {
    s_status.failures++;
    fr_copy(s_status.lastError, sizeof(s_status.lastError), err);
  }
  portEXIT_CRITICAL(&s_statMux);
}

void adsbGetStatus(AdsbStatus& out) {
  portENTER_CRITICAL(&s_statMux);
  out = s_status;
  portEXIT_CRITICAL(&s_statMux);
}

void adsbSetRange(uint16_t rangeNm) { s_rangeNm = rangeNm; }
void adsbRequestNow() { s_force = true; }

// ---------------------------------------------------------------------------
//  BodyStream: buffered reader over the HTTP body that also undoes HTTP
//  "chunked" encoding if the server uses it. Reading through a buffer is much
//  faster than reading single bytes from a TLS socket.
// ---------------------------------------------------------------------------
class BodyStream : public Stream {
 public:
  BodyStream(WiFiClient& c, bool chunked, uint32_t timeoutMs)
      : _c(c), _chunked(chunked), _timeout(timeoutMs) { setTimeout(timeoutMs); }
  int available() override { return (_len > _pos) ? (int)(_len - _pos) : 0; }
  int read() override {
    if (_peek != -2) { int c = _peek; _peek = -2; return c; }
    return next();
  }
  int peek() override {
    if (_peek == -2) _peek = next();
    return _peek;
  }
  size_t write(uint8_t) override { return 0; }

 private:
  WiFiClient& _c;
  bool _chunked;
  uint32_t _timeout;
  uint8_t _buf[512];
  size_t _pos = 0, _len = 0;
  int _peek = -2;
  bool _eof = false;
  uint32_t _chunkLeft = 0;
  bool _needCrlf = false;

  bool fill() {
    const uint32_t t0 = millis();
    for (;;) {
      int avail = _c.available();
      if (avail > 0) {
        int n = _c.read(_buf, avail < (int)sizeof(_buf) ? avail : (int)sizeof(_buf));
        if (n > 0) { _pos = 0; _len = (size_t)n; return true; }
      } else if (!_c.connected()) {
        return false;
      }
      if (millis() - t0 > _timeout) return false;
      delay(2);
    }
  }
  int rawRead() {
    if (_pos >= _len && !fill()) return -1;
    return _buf[_pos++];
  }
  int next() {
    if (_eof) return -1;
    if (!_chunked) { int c = rawRead(); if (c < 0) _eof = true; return c; }
    if (_chunkLeft == 0) {
      if (_needCrlf) { rawRead(); rawRead(); _needCrlf = false; }
      char line[20];
      uint8_t n = 0;
      for (;;) {
        int c = rawRead();
        if (c < 0) { _eof = true; return -1; }
        if (c == '\n') break;
        if (c != '\r' && n < sizeof(line) - 1) line[n++] = (char)c;
      }
      line[n] = 0;
      _chunkLeft = strtoul(line, nullptr, 16);
      if (_chunkLeft == 0) { _eof = true; return -1; }
    }
    int c = rawRead();
    if (c < 0) { _eof = true; return -1; }
    if (--_chunkLeft == 0) _needCrlf = true;
    return c;
  }
};

// Next byte that is not whitespace (consumed). -1 on timeout/end.
static int nextNonWs(Stream& s) {
  for (;;) {
    int c = s.read();
    if (c < 0) return -1;
    if (c != ' ' && c != '\n' && c != '\r' && c != '\t') return c;
  }
}
// Same but does not consume the non-whitespace byte.
static int peekNonWs(Stream& s) {
  for (;;) {
    int c = s.peek();
    if (c < 0) return -1;
    if (c != ' ' && c != '\n' && c != '\r' && c != '\t') return c;
    s.read();
  }
}

// ---------------------------------------------------------------------------
//  JSON element -> Aircraft
// ---------------------------------------------------------------------------
static void copyTrim(char* dst, size_t n, const char* src, bool upper) {
  fr_copy(dst, n, src);
  size_t l = strlen(dst);
  while (l && (dst[l - 1] == ' ' || dst[l - 1] == '\t')) dst[--l] = 0;
  size_t start = 0;
  while (dst[start] == ' ') start++;
  if (start) memmove(dst, dst + start, strlen(dst + start) + 1);
  if (upper) for (char* p = dst; *p; ++p) if (*p >= 'a' && *p <= 'z') *p -= 32;
}

// Returns true if "a" was filled with a usable aircraft.
static bool adsbIngestElement(JsonDocument& doc, Aircraft& a, double homeLat, double homeLon,
                              uint32_t nowMs, uint16_t radiusNm, bool globalAlertQuery) {
  memset(&a, 0, sizeof(a));

  const char* hex = doc["hex"] | "";
  if (*hex == '~') hex++;                       // non-ICAO (TIS-B) address marker
  char* end = nullptr;
  const unsigned long key = strtoul(hex, &end, 16);
  if (!*hex || (end && *end) || key == 0) return false;
  a.key = (uint32_t)key;
  copyTrim(a.icao, sizeof(a.icao), hex, true);
  JsonVariantConst squawk = doc["squawk"];
  if (squawk.is<const char*>()) {
    a.squawk = (uint16_t)strtoul(squawk.as<const char*>(), nullptr, 10);
  } else if (squawk.is<uint16_t>()) {
    a.squawk = squawk.as<uint16_t>();
  }
  if (globalAlertQuery && !aircraftSpecialSquawk(a.squawk)) return false;

  if (!doc["lat"].is<float>() || !doc["lon"].is<float>()) return false;   // no position
  a.latitude = doc["lat"].as<double>();
  a.longitude = doc["lon"].as<double>();
  if (!settingsHomeValid(a.latitude, a.longitude)) return false;

  float seenPos = doc["seen_pos"] | 0.0f;
  if (seenPos < 0) seenPos = 0;
  if (seenPos * 1000.0f > (float)g_settings.aircraftMaxAgeMs) return false;
  a.reportMs = nowMs - (uint32_t)(seenPos * 1000.0f);

  copyTrim(a.callsign, sizeof(a.callsign), doc["flight"] | "", true);
  copyTrim(a.registration, sizeof(a.registration), doc["r"] | "", true);
  copyTrim(a.aircraftType, sizeof(a.aircraftType), doc["t"] | "", true);
  const char* op = doc["ownOp"] | "";
  if (*op) copyTrim(a.operatorName, sizeof(a.operatorName), op, false);
  else {
    const char* fromCs = aircraftOperatorFromCallsign(a.callsign);
    if (fromCs) fr_copy(a.operatorName, sizeof(a.operatorName), fromCs);
  }

  // altitude: number (feet) or the string "ground"
  JsonVariantConst ab = doc["alt_baro"];
  if (ab.is<float>()) { a.altitude = (int32_t)lroundf(ab.as<float>()); a.hasAltitude = true; }
  else if (ab.is<const char*>()) {
    if (!strcasecmp(ab.as<const char*>(), "ground")) { a.onGround = true; a.hasAltitude = true; a.altitude = 0; }
  } else if (doc["alt_geom"].is<float>()) {
    a.altitude = (int32_t)lroundf(doc["alt_geom"].as<float>()); a.hasAltitude = true;
  }

  if (doc["gs"].is<float>()) {
    float gs = doc["gs"].as<float>();
    if (gs >= 0 && gs < 1500) { a.groundSpeed = gs; a.hasSpeed = true; }
  }
  const char* hk = doc["track"].is<float>() ? "track"
                 : doc["true_heading"].is<float>() ? "true_heading"
                 : doc["mag_heading"].is<float>() ? "mag_heading" : nullptr;
  if (hk) {
    float h = doc[hk].as<float>();
    if (isfinite(h)) { h = fmodf(h, 360.0f); if (h < 0) h += 360.0f; a.heading = h; a.hasHeading = true; }
  }

  const int dbFlags = doc["dbFlags"] | 0;
  a.cls = aircraftClassify(doc["category"] | "", a.aircraftType, a.callsign, (dbFlags & 1) != 0, &a.military);
  // Military already covers RAF/police-air-support-adjacent squawks etc; only
  // check for police/coastguard/air-ambulance on everything else so a given
  // aircraft never ends up flagged as both.
  a.service = a.military ? SVC_NONE : aircraftClassifyService(a.callsign, a.registration);
  if (g_settings.debugAircraftTypes)
    Serial.printf("[adsb] %s type=%-4s cat=%-2s callsign=%-8s military=%d\n",
                  a.icao, a.aircraftType, (const char*)(doc["category"] | ""), a.callsign, a.military);

  // Position -> distance/bearing (great circle) -> local east/north.
  geoToEastNorth(homeLat, homeLon, a.latitude, a.longitude, &a.x0, &a.y0);
  a.distance = sqrtf(a.x0 * a.x0 + a.y0 * a.y0);
  if (!globalAlertQuery && a.distance > radiusNm * 1.25f + 2.0f) return false;

  if (a.hasSpeed && a.hasHeading) {
    const float rad = a.heading * 0.017453292f;
    a.vx = a.groundSpeed / 3600.0f * sinf(rad);
    a.vy = a.groundSpeed / 3600.0f * cosf(rad);
  }
  a.x = a.x0; a.y = a.y0;
  a.commitMs = nowMs;
  return true;
}

// ---------------------------------------------------------------------------
//  One download
// ---------------------------------------------------------------------------
enum FetchResult { FETCH_OK, FETCH_CONNECT, FETCH_TIMEOUT, FETCH_HTTP, FETCH_RATE, FETCH_JSON };

static void keepNearest(Aircraft* stage, uint16_t& n, const Aircraft& a) {
  if (n < MAX_AIRCRAFT) { stage[n++] = a; return; }
  uint16_t far = 0;
  for (uint16_t i = 1; i < n; i++) if (stage[i].distance > stage[far].distance) far = i;
  if (a.distance < stage[far].distance) stage[far] = a;
}

static FetchResult doFetch(Aircraft* stage, uint16_t& outCount, JsonDocument& filter, JsonDocument& doc,
                           const char* urlFormat, double lat, double lon, uint16_t radius,
                           int& httpCode, char* err, size_t errN,
                           uint16_t globalSquawk = 0, const char* sqkUrlFormat = nullptr) {
  outCount = 0;
  char url[256];
  int urlLength;
  if (globalSquawk) {
    if (!sqkUrlFormat) {
      fr_copy(err, errN, "NO SQUAWK ENDPOINT FOR SOURCE");
      return FETCH_CONNECT;
    }
    urlLength = snprintf(url, sizeof(url), sqkUrlFormat, (unsigned)globalSquawk);
  } else {
    unsigned floatArgs = 0, radiusArgs = 0;
    for (const char* p = urlFormat; *p; ++p) {
      if (*p != '%') continue;
      if (p[1] == '%') { ++p; continue; }
      if (p[1] == '.' && p[2] == '4' && p[3] == 'f') { ++floatArgs; p += 3; continue; }
      if (p[1] == 'u') { ++radiusArgs; ++p; continue; }
      fr_copy(err, errN, "BAD URL FORMAT");
      return FETCH_CONNECT;
    }
    if (floatArgs != 2 || radiusArgs != 1) {
      fr_copy(err, errN, "BAD URL FORMAT");
      return FETCH_CONNECT;
    }
    urlLength = snprintf(url, sizeof(url), urlFormat, lat, lon, (unsigned)radius);
  }
  if (urlLength < 0 || (size_t)urlLength >= sizeof(url)) {
    fr_copy(err, errN, "URL TOO LONG");
    return FETCH_CONNECT;
  }
  const bool https = strncmp(url, "https", 5) == 0;

  WiFiClientSecure secure;
  WiFiClient plain;
  WiFiClient* client = &plain;
  if (https) {
    client = &secure;
    if (g_settings.adsbValidateTls) {
#ifdef ADSB_ROOT_CA_PEM
      secure.setCACert(ADSB_ROOT_CA_PEM);
#else
      fr_copy(err, errN, "NO TLS ROOT CA");
      return FETCH_CONNECT;
#endif
    } else {
      secure.setInsecure();
    }
    secure.setHandshakeTimeout(10);
  }

  HTTPClient http;
  http.setReuse(false);
  http.setConnectTimeout(g_settings.adsbConnectTimeoutMs);
  http.setTimeout(g_settings.adsbHttpTimeoutMs);
  http.useHTTP10(true);            // no chunked replies from well-behaved servers
  if (!http.begin(*client, url)) { fr_copy(err, errN, "BAD URL"); return FETCH_CONNECT; }
  http.addHeader("Accept", "application/json");
  http.addHeader("User-Agent", "FlightRadar-ESP32/" FW_VERSION);
  const char* keys[] = {"Transfer-Encoding"};
  http.collectHeaders(keys, 1);

  httpCode = http.GET();
  if (httpCode == 429 || httpCode == 420) {
    http.end();
    fr_copy(err, errN, "RATE LIMITED");
    return FETCH_RATE;
  }
  if (httpCode != HTTP_CODE_OK) {
    http.end();
    if (httpCode == HTTPC_ERROR_READ_TIMEOUT) { fr_copy(err, errN, "TIMEOUT"); return FETCH_TIMEOUT; }
    if (httpCode < 0) { fr_copy(err, errN, "NO CONNECTION"); return FETCH_CONNECT; }
    snprintf(err, errN, "HTTP %d", httpCode);
    return FETCH_HTTP;
  }

  const String te = http.header("Transfer-Encoding");
  const bool chunked = strstr(te.c_str(), "hunked") != nullptr;
  BodyStream body(http.getStream(), chunked, g_settings.adsbHttpTimeoutMs);
  Stream& s = body;

  FetchResult result = FETCH_JSON;
  fr_copy(err, errN, "BAD JSON");

  if (s.find("\"ac\":")) {
    int c = nextNonWs(s);
    if (c == 'n') {                                  // "ac":null  -> no aircraft
      result = FETCH_OK;
    } else if (c == '[') {
      const uint32_t now = millis();
      bool ok = true;
      if (peekNonWs(s) == ']') {                     // empty list
        result = FETCH_OK;
      } else {
        for (;;) {
          DeserializationError e = deserializeJson(doc, s, DeserializationOption::Filter(filter));
          if (e) { ok = false; break; }
          Aircraft a;
          if (adsbIngestElement(doc, a, lat, lon, now, radius, globalSquawk != 0))
            keepNearest(stage, outCount, a);
          const int d = nextNonWs(s);
          if (d == ',') continue;
          if (d == ']') break;
          ok = false; break;
        }
        if (ok) result = FETCH_OK;
      }
    }
  }
  http.end();
  if (result == FETCH_OK) err[0] = 0;
  return result;
}

struct AdsbFeed {
  const char* name;
  const char* urlFormat;
  const char* sqkUrlFormat;   // global squawk 7500/7700 lookup - each provider uses a different path
};

static const AdsbFeed ADSB_FEEDS[] = {
  {"adsb.lol",        "https://api.adsb.lol/v2/point/%.4f/%.4f/%u",                  "https://api.adsb.lol/v2/squawk/%u"},
  {"adsb.fi",          "https://opendata.adsb.fi/api/v3/lat/%.4f/lon/%.4f/dist/%u",  "https://opendata.adsb.fi/api/v2/sqk/%u"},
  {"custom",           nullptr,                                                      nullptr},
  {"airplanes.live",   "https://api.airplanes.live/v2/point/%.4f/%.4f/%u",           "https://api.airplanes.live/v2/squawk/%u"}
};

static constexpr size_t ADSB_FEED_COUNT = sizeof(ADSB_FEEDS) / sizeof(ADSB_FEEDS[0]);

static void mergeFields(Aircraft& preferred, const Aircraft& other) {
  if (!preferred.squawk) preferred.squawk = other.squawk;
  if (!preferred.callsign[0]) fr_copy(preferred.callsign, sizeof(preferred.callsign), other.callsign);
  if (!preferred.registration[0]) fr_copy(preferred.registration, sizeof(preferred.registration), other.registration);
  if (!preferred.aircraftType[0]) fr_copy(preferred.aircraftType, sizeof(preferred.aircraftType), other.aircraftType);
  if (!preferred.operatorName[0]) fr_copy(preferred.operatorName, sizeof(preferred.operatorName), other.operatorName);
  if (!preferred.hasAltitude && other.hasAltitude) {
    preferred.altitude = other.altitude;
    preferred.hasAltitude = true;
    preferred.onGround = other.onGround;
  }
  if (!preferred.hasSpeed && other.hasSpeed) {
    preferred.groundSpeed = other.groundSpeed;
    preferred.hasSpeed = true;
  }
  if (!preferred.hasHeading && other.hasHeading) {
    preferred.heading = other.heading;
    preferred.hasHeading = true;
  }
  if (preferred.cls == AC_UNKNOWN && other.cls != AC_UNKNOWN) {
    preferred.cls = other.cls;
  }
  preferred.military = preferred.military || other.military;
  if (preferred.military) preferred.cls = AC_MILITARY;
  if (preferred.service == SVC_NONE) preferred.service = other.service;
  if (preferred.military) preferred.service = SVC_NONE;
}

static void mergeFeedAircraft(Aircraft* combined, uint16_t& combinedCount,
                              const Aircraft* feed, uint16_t feedCount) {
  for (uint16_t i = 0; i < feedCount; ++i) {
    const Aircraft& incoming = feed[i];
    int match = -1;
    for (uint16_t j = 0; j < combinedCount; ++j) {
      if (combined[j].key == incoming.key) {
        match = (int)j;
        break;
      }
    }
    if (match < 0) {
      keepNearest(combined, combinedCount, incoming);
      continue;
    }

    Aircraft& current = combined[match];
    if ((int32_t)(incoming.reportMs - current.reportMs) > 0) {
      Aircraft merged = incoming;
      mergeFields(merged, current);
      current = merged;
    } else {
      mergeFields(current, incoming);
    }
  }
}

static void mergeAlertAircraft(Aircraft* combined, uint16_t& combinedCount,
                               const Aircraft* alerts, uint16_t alertCount) {
  for (uint16_t i = 0; i < alertCount; ++i) {
    const Aircraft& incoming = alerts[i];
    int match = -1;
    for (uint16_t j = 0; j < combinedCount; ++j) {
      if (combined[j].key == incoming.key) { match = (int)j; break; }
    }
    if (match >= 0) {
      Aircraft& current = combined[match];
      if ((int32_t)(incoming.reportMs - current.reportMs) >= 0) {
        Aircraft merged = incoming;
        mergeFields(merged, current);
        current = merged;
      } else {
        mergeFields(current, incoming);
        current.squawk = incoming.squawk;
      }
      continue;
    }
    if (combinedCount < MAX_AIRCRAFT) {
      combined[combinedCount++] = incoming;
      continue;
    }
    uint16_t victim = MAX_AIRCRAFT;
    float farthest = -1.0f;
    for (uint16_t j = 0; j < combinedCount; ++j) {
      if (aircraftSpecialSquawk(combined[j].squawk)) continue;
      if (combined[j].distance > farthest) {
        farthest = combined[j].distance;
        victim = j;
      }
    }
    if (victim < MAX_AIRCRAFT) combined[victim] = incoming;
  }
}

// ---------------------------------------------------------------------------
//  Task
// ---------------------------------------------------------------------------
static void adsbTask(void*) {
  Aircraft* stage = (Aircraft*)aircraftAllocLarge(sizeof(Aircraft) * MAX_AIRCRAFT);
  Aircraft* feedStage = (Aircraft*)aircraftAllocLarge(sizeof(Aircraft) * MAX_AIRCRAFT);
  Aircraft* feedCache = (Aircraft*)aircraftAllocLarge(
    sizeof(Aircraft) * MAX_AIRCRAFT * ADSB_FEED_COUNT);
  Aircraft* alertCache = (Aircraft*)aircraftAllocLarge(sizeof(Aircraft) * MAX_AIRCRAFT * SPECIAL_SQUAWK_COUNT);
  Aircraft* alertMerge = (Aircraft*)aircraftAllocLarge(sizeof(Aircraft) * MAX_AIRCRAFT);
  if (!stage || !feedStage || !feedCache || !alertCache || !alertMerge) {
    setStatus(false, 0, 0, "NO MEMORY");
    Serial.println("[adsb] out of memory, task stopped");
    free(stage);
    free(feedStage);
    free(feedCache);
    free(alertCache);
    free(alertMerge);
    vTaskDelete(nullptr);
    return;
  }
  uint16_t cachedCounts[ADSB_FEED_COUNT] = {};
  uint32_t cacheUpdatedMs[ADSB_FEED_COUNT] = {};
  bool cacheValid[ADSB_FEED_COUNT] = {};
  uint16_t alertCounts[SPECIAL_SQUAWK_COUNT] = {};
  uint32_t alertUpdatedMs[SPECIAL_SQUAWK_COUNT] = {};
  bool alertValid[SPECIAL_SQUAWK_COUNT] = {};
  uint8_t nextAlertIndex = 0;
  JsonDocument filter, doc;
  const char* fields[] = {"hex","flight","r","t","ownOp","lat","lon","alt_baro","alt_geom","gs",
                          "track","true_heading","mag_heading","category","dbFlags","seen_pos","squawk"};
  for (const char* f : fields) filter[f] = true;

  uint32_t nextDue = 0, lastAttempt = 0;
  uint32_t lastRev = settingsHomeRev();
  uint16_t lastRadius = 0;
  bool first = true;

  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(100));
    if (!wifiIsConnected()) continue;

    const uint32_t now = millis();
    uint16_t radius = (uint16_t)((uint32_t)s_rangeNm * 11u / 10u + 1u);
    if (radius < 10) radius = 10;
    if (radius > 250) radius = 250;

    const bool homeMoved = (settingsHomeRev() != lastRev);
    const bool wantMore = (radius > lastRadius);
    const bool forced = s_force || homeMoved || wantMore;
    const bool due = (int32_t)(now - nextDue) >= 0;
    const bool gapOk = first || (now - lastAttempt) >= g_settings.adsbMinRequestGapMs;
    if (!((forced && gapOk) || (due && gapOk))) continue;

    s_force = false;
    first = false;
    lastAttempt = now;
    const uint32_t rev = settingsHomeRev();
    const double lat = g_settings.homeLat, lon = g_settings.homeLon;

    bool anyFeedOk = false;
    bool anyRateLimited = false;
    int lastHttp = 0;
    uint16_t combinedCount = 0;
    const size_t firstFeed = g_settings.adsbMode == 1 ? 0 : g_settings.adsbMode == 2 ? 1 :
                             g_settings.adsbMode == 3 ? 2 : g_settings.adsbMode == 4 ? 3 : 0;
    const size_t endFeed = g_settings.adsbMode == 0 ? 2 : firstFeed + 1;
    for (size_t i = firstFeed; g_settings.homeConfigured && i < endFeed; ++i) {
      uint16_t feedCount = 0;
      int http = 0;
      char err[24] = {0};
      const char* urlFormat = i == 2 ? g_settings.adsbCustomUrl : ADSB_FEEDS[i].urlFormat;
      const FetchResult result = doFetch(feedStage, feedCount, filter, doc,
        urlFormat, lat, lon, radius, http, err, sizeof(err));
      doc.clear();
      if (result == FETCH_OK) {
        Aircraft* cache = feedCache + i * MAX_AIRCRAFT;
        if (feedCount) memcpy(cache, feedStage, sizeof(Aircraft) * feedCount);
        cachedCounts[i] = feedCount;
        cacheUpdatedMs[i] = millis();
        cacheValid[i] = true;
        anyFeedOk = true;
        lastHttp = http;
        Serial.printf("[adsb] %s OK: %u aircraft\n",
                      ADSB_FEEDS[i].name, (unsigned)feedCount);
      } else {
        anyRateLimited = anyRateLimited || result == FETCH_RATE;
        Serial.printf("[adsb] %s failed: %s; retaining recent reports\n",
                      ADSB_FEEDS[i].name, err);
      }
    }

    // Query every feed that's actually active (not just one hardcoded provider) for
    // this cycle's squawk and merge whatever comes back. Querying only feed[0] meant
    // that in "both" mode, emergency detection lived or died on adsb.lol alone even
    // though adsb.fi was also configured and working - and adsb.lol is the one that's
    // been seen flaking with 429/503. Any feed with no known squawk-lookup path (the
    // "custom" source) is skipped quietly rather than spamming a failure every cycle.
    // Cycles round-robin through every code in SPECIAL_SQUAWKS (aircraft.h), not just
    // 7500/7700, so 7600 gets the same out-of-range global lookup too.
    const uint16_t alertSquawk = SPECIAL_SQUAWKS[nextAlertIndex].code;
    uint16_t alertCount = 0;
    int alertHttp = 0;
    bool alertAnyOk = false;
    bool triedAnyFeed = false;
    for (size_t i = firstFeed; i < endFeed; ++i) {
      const char* sqkUrlFormat = ADSB_FEEDS[i].sqkUrlFormat;
      if (!sqkUrlFormat) continue;
      triedAnyFeed = true;
      uint16_t thisCount = 0;
      int http = 0;
      char err[24] = {0};
      const FetchResult alertResult = doFetch(feedStage, thisCount, filter, doc, nullptr, lat, lon, 250,
        http, err, sizeof(err), alertSquawk, sqkUrlFormat);
      doc.clear();
      if (alertResult == FETCH_OK) {
        mergeFeedAircraft(alertMerge, alertCount, feedStage, thisCount);
        alertAnyOk = true;
        alertHttp = http;
        Serial.printf("[adsb] global squawk %u OK: %u aircraft (%s)\n",
                      (unsigned)alertSquawk, (unsigned)thisCount, ADSB_FEEDS[i].name);
      } else {
        anyRateLimited = anyRateLimited || alertResult == FETCH_RATE;
        Serial.printf("[adsb] global squawk %u failed: %s (%s)\n",
                      (unsigned)alertSquawk, err, ADSB_FEEDS[i].name);
      }
    }
    if (triedAnyFeed && alertAnyOk) {
      Aircraft* cache = alertCache + nextAlertIndex * MAX_AIRCRAFT;
      if (alertCount) memcpy(cache, alertMerge, sizeof(Aircraft) * alertCount);
      alertCounts[nextAlertIndex] = alertCount;
      alertUpdatedMs[nextAlertIndex] = millis();
      alertValid[nextAlertIndex] = true;
      anyFeedOk = true;
      lastHttp = alertHttp;
    } else if (triedAnyFeed) {
      Serial.printf("[adsb] global squawk %u: all feeds failed this cycle; retaining recent reports\n",
                    (unsigned)alertSquawk);
    }
    nextAlertIndex = (uint8_t)((nextAlertIndex + 1) % SPECIAL_SQUAWK_COUNT);

    const uint32_t combinedAt = millis();
    for (size_t i = firstFeed; i < endFeed; ++i) {
      if (!cacheValid[i] ||
          (uint32_t)(combinedAt - cacheUpdatedMs[i]) > g_settings.aircraftMaxAgeMs) continue;
      mergeFeedAircraft(stage, combinedCount,
                        feedCache + i * MAX_AIRCRAFT, cachedCounts[i]);
    }
    for (uint8_t i = 0; i < SPECIAL_SQUAWK_COUNT; ++i) {
      if (!alertValid[i] ||
          (uint32_t)(combinedAt - alertUpdatedMs[i]) > g_settings.aircraftMaxAgeMs) continue;
      mergeAlertAircraft(stage, combinedCount, alertCache + i * MAX_AIRCRAFT, alertCounts[i]);
    }
    if (combinedCount || anyFeedOk) aircraftSubmit(stage, combinedCount, rev);

    if (anyFeedOk) {
      setStatus(true, lastHttp, combinedCount, "");
      lastRev = rev;
      lastRadius = radius;
      nextDue = millis() + g_settings.adsbUpdateIntervalMs;
    } else {
      setStatus(false, lastHttp, combinedCount, "ALL SOURCES FAILED");
      lastRev = rev;              // retries follow the back-off below, not the zoom/home triggers
      lastRadius = radius;
      AdsbStatus st;
      adsbGetStatus(st);
      uint32_t mult = st.failures < 5 ? st.failures + 1 : 6;
      uint32_t wait = g_settings.adsbUpdateIntervalMs * mult;
      if (anyRateLimited) wait = 30000;
      nextDue = millis() + wait;
      Serial.printf("[adsb] all sources failed (retry in %lu ms)\n", (unsigned long)wait);
    }
  }
}

void adsbInit() {
  memset(&s_status, 0, sizeof(s_status));
  fr_copy(s_status.lastError, sizeof(s_status.lastError), "STARTING");
  s_rangeNm = RANGES_NM[g_settings.rangeIndex < RANGE_COUNT ? g_settings.rangeIndex : 2];
  xTaskCreatePinnedToCore(adsbTask, "adsb", 16384, nullptr, 1, nullptr, 0);
}
