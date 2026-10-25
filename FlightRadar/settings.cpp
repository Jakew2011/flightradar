#include "settings.h"
#include "util.h"
#include "wifi_manager.h"
#include "adsb_api.h"
#include "train.h"
#include <Arduino.h>
#include <Preferences.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

const uint16_t RANGES_NM[RANGE_COUNT] = {5, 10, 20, 30, 50, 75, 100, 150, 200};

Settings g_settings;

static const char* NVS_NS = "fradar";
static uint32_t s_homeRev = 1;
static bool     s_dirty = false;
static uint32_t s_dirtyAt = 0;
static void applyDefaults(Settings& s);

uint32_t settingsHomeRev() { return s_homeRev; }

bool settingsHomeValid(double lat, double lon) {
  return isfinite(lat) && isfinite(lon) && lat >= -90.0 && lat <= 90.0 && lon >= -180.0 && lon <= 180.0;
}

uint8_t settingsRangeIndexFor(uint16_t nm) {
  uint8_t best = 2;
  uint16_t bestDiff = 0xFFFF;
  for (uint8_t i = 0; i < RANGE_COUNT; i++) {
    uint16_t d = nm > RANGES_NM[i] ? nm - RANGES_NM[i] : RANGES_NM[i] - nm;
    if (d < bestDiff) { bestDiff = d; best = i; }
  }
  return best;
}

float settingsRangeNm() {
  uint8_t i = g_settings.rangeIndex;
  if (i >= RANGE_COUNT) i = 2;
  return (float)RANGES_NM[i];
}

bool settingsNightModeActive() {
  if (!g_settings.nightModeEnabled) return false;
  struct tm t;
  if (!clockGet(&t)) return false;          // time not synced yet: don't guess
  const int h = t.tm_hour;
  return (g_settings.nightStartHour > g_settings.nightEndHour)
           ? (h >= g_settings.nightStartHour || h < g_settings.nightEndHour)
           : (h >= g_settings.nightStartHour && h < g_settings.nightEndHour);
}

// Only reset saved settings when the Settings struct's *shape* changes, not
// when config.h's *values* change. This used to hash the fully-applied
// defaults (every WiFi SSID/password, URL, timeout, etc. from config.h), so
// editing config.h for anything - a new OTA URL, a tweaked timeout, flipping
// ADSB_MULTI_SOURCE_ENABLED - silently wiped the saved WiFi credentials and
// home location on the next boot. Now it's just the struct size plus a
// manually-maintained schema version (bumped in config.h only when a field
// is actually added/removed/retyped), so ordinary firmware updates keep
// everything the user configured on the device.
static uint32_t cfgSignature() {
  uint32_t h = 2166136261u;
  h ^= (uint32_t)sizeof(Settings);      h *= 16777619u;
  h ^= (uint32_t)SETTINGS_SCHEMA_VERSION; h *= 16777619u;
  return h;
}

static void applyDefaults(Settings& s) {
  memset(&s, 0, sizeof(s));
  fr_copy(s.wifiSsid, sizeof(s.wifiSsid), WIFI_SSID);
  fr_copy(s.wifiPass, sizeof(s.wifiPass), WIFI_PASSWORD);
  fr_copy(s.wifiHostname, sizeof(s.wifiHostname), WIFI_HOSTNAME);
  fr_copy(s.portalSsid, sizeof(s.portalSsid), WIFI_PORTAL_SSID);
  fr_copy(s.portalPass, sizeof(s.portalPass), WIFI_PORTAL_PASSWORD);
  fr_copy(s.timezone, sizeof(s.timezone), TIMEZONE_POSIX);
  fr_copy(s.audioUrl, sizeof(s.audioUrl), MILITARY_WAV_URL);
  fr_copy(s.adsbCustomUrl, sizeof(s.adsbCustomUrl), ADSB_CUSTOM_URL_FORMAT);
  fr_copy(s.trainStationCrs, sizeof(s.trainStationCrs), TRAIN_DEFAULT_STATION_CRS);
  fr_copy(s.trainApiKey, sizeof(s.trainApiKey), OPENLDBWS_API_KEY);
  s.homeLat = HOME_LAT;
  s.homeLon = HOME_LON;
  s.homeConfigured = settingsHomeValid(HOME_LAT, HOME_LON) &&
                     (HOME_LAT != 0.0 || HOME_LON != 0.0);
  s.serialBaud = SERIAL_BAUD;
  s.wifiConnectTimeoutMs = WIFI_CONNECT_TIMEOUT_MS;
  s.wifiRetryMinMs = WIFI_RETRY_MIN_MS;
  s.wifiRetryMaxMs = WIFI_RETRY_MAX_MS;
  s.portalTimeoutMs = WIFI_PORTAL_TIMEOUT_MS;
  s.audioMaxBytes = MILITARY_WAV_MAX_BYTES;
  s.audioFetchTimeoutMs = MILITARY_WAV_FETCH_TIMEOUT_MS;
  s.audioWifiWaitMs = MILITARY_WAV_WIFI_WAIT_MS;
  s.announceCooldownMs = MILITARY_ANNOUNCE_COOLDOWN_MS;
  s.adsbUpdateIntervalMs = AIRCRAFT_UPDATE_INTERVAL;
  s.adsbMinRequestGapMs = AIRCRAFT_MIN_REQUEST_GAP;
  s.adsbConnectTimeoutMs = ADSB_CONNECT_TIMEOUT_MS;
  s.adsbHttpTimeoutMs = ADSB_HTTP_TIMEOUT_MS;
  s.aircraftMaxAgeMs = AIRCRAFT_MAX_AGE;
  s.aircraftMaxExtrapolationMs = AIRCRAFT_MAX_EXTRAPOLATION;
  s.aircraftBlendMs = AIRCRAFT_BLEND_MS;
  s.radarSweepPeriodMs = RADAR_SWEEP_PERIOD;
  s.portalEnabled = 1;
  s.portalAfterFails = WIFI_PORTAL_AFTER_FAILS;
  s.adsbMode = ADSB_MULTI_SOURCE_ENABLED ? 0 :
               ADSB_SOURCE == ADSB_SOURCE_ADSB_LOL ? 1 :
               ADSB_SOURCE == ADSB_SOURCE_ADSB_FI ? 2 :
               ADSB_SOURCE == ADSB_SOURCE_CUSTOM ? 3 : 4;
  s.showGroundAircraft = SHOW_GROUND_AIRCRAFT;
  s.nightModeEnabled = NIGHT_MODE_ENABLED;
  s.nightStartHour = NIGHT_MODE_START_HOUR;
  s.nightEndHour = NIGHT_MODE_END_HOUR;
  s.nightBrightness = NIGHT_MODE_BRIGHTNESS;
  s.encoderReverse = ENCODER_REVERSE;
  s.encoderStepsPerDetent = ENCODER_STEPS_PER_DETENT;
  s.buttonDebounceMs = BUTTON_DEBOUNCE_MS;
  s.longPressMs = LONG_PRESS_MS;
  s.audioEnabled = AUDIO_ENABLED;
  s.debugAircraftTypes = DEBUG_PRINT_AIRCRAFT_TYPES;
  s.adsbValidateTls = ADSB_VALIDATE_TLS;
  s.radarMaxFps = RADAR_MAX_FPS;
  s.sweepTrailSegments = SWEEP_TRAIL_SEGMENTS;
  s.sweepTrailDegrees = (uint16_t)SWEEP_TRAIL_DEGREES;
  s.labelsEnabled = LABELS_ENABLED;
  s.labelMax = LABEL_MAX;
  s.aircraftIconScale = AIRCRAFT_ICON_SCALE;
  s.rangeIndex = settingsRangeIndexFor(DEFAULT_RANGE);
  s.theme = DEFAULT_THEME;
  s.brightness = DISPLAY_BRIGHTNESS;
  s.sweepSpeed = 1;
  s.distUnit = DEFAULT_DISTANCE_UNIT;
  s.altUnit = DEFAULT_ALTITUDE_UNIT;
  s.speedUnit = DEFAULT_SPEED_UNIT;
}

static void sanitize(Settings& s) {
  s.wifiSsid[sizeof(s.wifiSsid) - 1] = 0;
  s.wifiPass[sizeof(s.wifiPass) - 1] = 0;
  s.wifiHostname[sizeof(s.wifiHostname) - 1] = 0;
  s.portalSsid[sizeof(s.portalSsid) - 1] = 0;
  s.portalPass[sizeof(s.portalPass) - 1] = 0;
  s.timezone[sizeof(s.timezone) - 1] = 0;
  s.audioUrl[sizeof(s.audioUrl) - 1] = 0;
  s.adsbCustomUrl[sizeof(s.adsbCustomUrl) - 1] = 0;
  s.trainStationCrs[sizeof(s.trainStationCrs) - 1] = 0;
  s.trainApiKey[sizeof(s.trainApiKey) - 1] = 0;
  {
    // CRS codes are always exactly 3 letters - sanitize anything stored or
    // typed into the portal down to that shape, upper-cased.
    char clean[4] = {0};
    uint8_t n = 0;
    for (const char* p = s.trainStationCrs; *p && n < 3; ++p) {
      char c = *p;
      if (c >= 'a' && c <= 'z') c -= 32;
      if (c >= 'A' && c <= 'Z') clean[n++] = c;
    }
    clean[n] = 0;
    if (n == 3) fr_copy(s.trainStationCrs, sizeof(s.trainStationCrs), clean);
    else fr_copy(s.trainStationCrs, sizeof(s.trainStationCrs), TRAIN_DEFAULT_STATION_CRS);
  }
  if (!settingsHomeValid(s.homeLat, s.homeLon)) {
    s.homeLat = (HOME_LAT >= -90.0 && HOME_LAT <= 90.0) ? HOME_LAT : 0.0;
    s.homeLon = (HOME_LON >= -180.0 && HOME_LON <= 180.0) ? HOME_LON : 0.0;
  }
  s.homeConfigured = s.homeConfigured ? 1 : 0;
  if (s.rangeIndex >= RANGE_COUNT) s.rangeIndex = settingsRangeIndexFor(DEFAULT_RANGE);
  if (s.theme > 2) s.theme = 0;
  s.brightness = fr_clamp<uint8_t>(s.brightness, 5, 100);
  if (s.sweepSpeed > 3) s.sweepSpeed = 1;
  if (s.distUnit > 1) s.distUnit = 0;
  if (s.altUnit > 1) s.altUnit = 0;
  if (s.speedUnit > 1) s.speedUnit = 0;
  if (s.serialBaud < 1200 || s.serialBaud > 2000000) s.serialBaud = SERIAL_BAUD;
  s.wifiConnectTimeoutMs = fr_clamp<uint32_t>(s.wifiConnectTimeoutMs, 3000, 60000);
  s.wifiRetryMinMs = fr_clamp<uint32_t>(s.wifiRetryMinMs, 500, 60000);
  s.wifiRetryMaxMs = fr_clamp<uint32_t>(s.wifiRetryMaxMs, s.wifiRetryMinMs, 300000);
  s.portalTimeoutMs = fr_clamp<uint32_t>(s.portalTimeoutMs, 30000, 86400000);
  s.audioMaxBytes = fr_clamp<uint32_t>(s.audioMaxBytes, 4096, 8UL * 1024UL * 1024UL);
  s.audioFetchTimeoutMs = fr_clamp<uint32_t>(s.audioFetchTimeoutMs, 1000, 60000);
  s.audioWifiWaitMs = fr_clamp<uint32_t>(s.audioWifiWaitMs, 0, 300000);
  s.announceCooldownMs = fr_clamp<uint32_t>(s.announceCooldownMs, 10000, 3600000);
  s.adsbUpdateIntervalMs = fr_clamp<uint32_t>(s.adsbUpdateIntervalMs, 2000, 60000);
  s.adsbMinRequestGapMs = fr_clamp<uint32_t>(s.adsbMinRequestGapMs, 1000, 60000);
  if (s.adsbMinRequestGapMs > s.adsbUpdateIntervalMs)
    s.adsbMinRequestGapMs = s.adsbUpdateIntervalMs;
  s.adsbConnectTimeoutMs = fr_clamp<uint32_t>(s.adsbConnectTimeoutMs, 1000, 60000);
  s.adsbHttpTimeoutMs = fr_clamp<uint32_t>(s.adsbHttpTimeoutMs, 1000, 60000);
  s.aircraftMaxAgeMs = fr_clamp<uint32_t>(s.aircraftMaxAgeMs, 5000, 300000);
  s.aircraftMaxExtrapolationMs = fr_clamp<uint32_t>(s.aircraftMaxExtrapolationMs, 0, s.aircraftMaxAgeMs);
  s.aircraftBlendMs = fr_clamp<uint32_t>(s.aircraftBlendMs, 0, 10000);
  s.radarSweepPeriodMs = fr_clamp<uint32_t>(s.radarSweepPeriodMs, 500, 60000);
  s.portalEnabled = s.portalEnabled ? 1 : 0;
  s.portalAfterFails = fr_clamp<uint8_t>(s.portalAfterFails, 1, 20);
  if (s.adsbMode > 4) s.adsbMode = 0;
  s.showGroundAircraft = s.showGroundAircraft ? 1 : 0;
  s.nightModeEnabled = s.nightModeEnabled ? 1 : 0;
  if (s.nightStartHour > 23) s.nightStartHour = 22;
  if (s.nightEndHour > 23) s.nightEndHour = 7;
  s.nightBrightness = fr_clamp<uint8_t>(s.nightBrightness, 5, 100);
  s.encoderReverse = s.encoderReverse ? 1 : 0;
  if (s.encoderStepsPerDetent != 1 && s.encoderStepsPerDetent != 2 &&
      s.encoderStepsPerDetent != 4) s.encoderStepsPerDetent = 4;
  s.buttonDebounceMs = fr_clamp<uint16_t>(s.buttonDebounceMs, 5, 250);
  s.longPressMs = fr_clamp<uint16_t>(s.longPressMs, 250, 5000);
  s.audioEnabled = s.audioEnabled ? 1 : 0;
  s.debugAircraftTypes = s.debugAircraftTypes ? 1 : 0;
  s.adsbValidateTls = s.adsbValidateTls ? 1 : 0;
  s.radarMaxFps = fr_clamp<uint8_t>(s.radarMaxFps, 5, 60);
  s.sweepTrailSegments = fr_clamp<uint8_t>(s.sweepTrailSegments, 1, SWEEP_TRAIL_SEGMENTS);
  s.sweepTrailDegrees = fr_clamp<uint16_t>(s.sweepTrailDegrees, 1, 180);
  s.labelsEnabled = s.labelsEnabled ? 1 : 0;
  s.labelMax = fr_clamp<uint8_t>(s.labelMax, 1, LABEL_MAX);
  if (!isfinite(s.aircraftIconScale)) s.aircraftIconScale = AIRCRAFT_ICON_SCALE;
  s.aircraftIconScale = fr_clamp<float>(s.aircraftIconScale, 0.5f, 3.0f);
}

void settingsSave() {
  Preferences p;
  if (!p.begin(NVS_NS, false)) {
    Serial.println("[settings] could not open flash storage for writing");
    return;
  }
  p.putString("ssid", g_settings.wifiSsid);
  p.putString("pass", g_settings.wifiPass);
  p.putString("host", g_settings.wifiHostname);
  p.putString("apsid", g_settings.portalSsid);
  p.putString("appass", g_settings.portalPass);
  p.putString("tz", g_settings.timezone);
  p.putString("wavurl", g_settings.audioUrl);
  p.putString("adscus", g_settings.adsbCustomUrl);
  p.putString("traincrs", g_settings.trainStationCrs);
  p.putString("trainkey", g_settings.trainApiKey);
  p.putDouble("lat", g_settings.homeLat);
  p.putDouble("lon", g_settings.homeLon);
  p.putUChar("homeok", g_settings.homeConfigured);
  p.putUInt("baud", g_settings.serialBaud);
  p.putUInt("wictmo", g_settings.wifiConnectTimeoutMs);
  p.putUInt("wimin", g_settings.wifiRetryMinMs);
  p.putUInt("wimax", g_settings.wifiRetryMaxMs);
  p.putUInt("aptmo", g_settings.portalTimeoutMs);
  p.putUInt("wavmax", g_settings.audioMaxBytes);
  p.putUInt("wavtmo", g_settings.audioFetchTimeoutMs);
  p.putUInt("wavwait", g_settings.audioWifiWaitMs);
  p.putUInt("cooldown", g_settings.announceCooldownMs);
  p.putUInt("adsi", g_settings.adsbUpdateIntervalMs);
  p.putUInt("adsgap", g_settings.adsbMinRequestGapMs);
  p.putUInt("adsc", g_settings.adsbConnectTimeoutMs);
  p.putUInt("adsh", g_settings.adsbHttpTimeoutMs);
  p.putUInt("acage", g_settings.aircraftMaxAgeMs);
  p.putUInt("acextra", g_settings.aircraftMaxExtrapolationMs);
  p.putUInt("acblend", g_settings.aircraftBlendMs);
  p.putUInt("swperiod", g_settings.radarSweepPeriodMs);
  p.putUChar("apen", g_settings.portalEnabled);
  p.putUChar("apfails", g_settings.portalAfterFails);
  p.putUChar("adsmode", g_settings.adsbMode);
  p.putUChar("ground", g_settings.showGroundAircraft);
  p.putUChar("night", g_settings.nightModeEnabled);
  p.putUChar("nightst", g_settings.nightStartHour);
  p.putUChar("nightend", g_settings.nightEndHour);
  p.putUChar("nightbr", g_settings.nightBrightness);
  p.putUChar("encinv", g_settings.encoderReverse);
  p.putUChar("encstep", g_settings.encoderStepsPerDetent);
  p.putUShort("debounce", g_settings.buttonDebounceMs);
  p.putUShort("longms", g_settings.longPressMs);
  p.putUChar("audio", g_settings.audioEnabled);
  p.putUChar("debugac", g_settings.debugAircraftTypes);
  p.putUChar("adstls", g_settings.adsbValidateTls);
  p.putUChar("fps", g_settings.radarMaxFps);
  p.putUChar("trailn", g_settings.sweepTrailSegments);
  p.putUShort("traildeg", g_settings.sweepTrailDegrees);
  p.putUChar("labels", g_settings.labelsEnabled);
  p.putUChar("labelmax", g_settings.labelMax);
  p.putFloat("iconscale", g_settings.aircraftIconScale);
  p.putUChar("range", g_settings.rangeIndex);
  p.putUChar("theme", g_settings.theme);
  p.putUChar("bright", g_settings.brightness);
  p.putUChar("sweep", g_settings.sweepSpeed);
  p.putUChar("dunit", g_settings.distUnit);
  p.putUChar("aunit", g_settings.altUnit);
  p.putUChar("sunit", g_settings.speedUnit);
  p.putUInt("cfgsig", cfgSignature());
  p.end();
  s_dirty = false;
}

void settingsBegin() {
  applyDefaults(g_settings);
  Preferences p;
  if (!p.begin(NVS_NS, false)) {
    Serial.println("[settings] flash storage unavailable, using defaults from config.h");
    return;
  }
  const uint32_t sig = cfgSignature();
  const bool sameCfg = (p.getUInt("cfgsig", 0) == sig);
  if (sameCfg) {
    // config.h unchanged since last save: keep values changed on the device.
    if (p.isKey("ssid")) p.getString("ssid", g_settings.wifiSsid, sizeof(g_settings.wifiSsid));
    if (p.isKey("pass")) p.getString("pass", g_settings.wifiPass, sizeof(g_settings.wifiPass));
    g_settings.homeLat = p.getDouble("lat", g_settings.homeLat);
    g_settings.homeLon = p.getDouble("lon", g_settings.homeLon);
      g_settings.homeConfigured = p.getUChar("homeok", g_settings.homeConfigured);
  }
  if (sameCfg) {
  p.getString("host", g_settings.wifiHostname, sizeof(g_settings.wifiHostname));
  p.getString("apsid", g_settings.portalSsid, sizeof(g_settings.portalSsid));
  p.getString("appass", g_settings.portalPass, sizeof(g_settings.portalPass));
  p.getString("tz", g_settings.timezone, sizeof(g_settings.timezone));
  p.getString("wavurl", g_settings.audioUrl, sizeof(g_settings.audioUrl));
  p.getString("adscus", g_settings.adsbCustomUrl, sizeof(g_settings.adsbCustomUrl));
  p.getString("traincrs", g_settings.trainStationCrs, sizeof(g_settings.trainStationCrs));
  p.getString("trainkey", g_settings.trainApiKey, sizeof(g_settings.trainApiKey));
  g_settings.serialBaud = p.getUInt("baud", g_settings.serialBaud);
  g_settings.wifiConnectTimeoutMs = p.getUInt("wictmo", g_settings.wifiConnectTimeoutMs);
  g_settings.wifiRetryMinMs = p.getUInt("wimin", g_settings.wifiRetryMinMs);
  g_settings.wifiRetryMaxMs = p.getUInt("wimax", g_settings.wifiRetryMaxMs);
  g_settings.portalTimeoutMs = p.getUInt("aptmo", g_settings.portalTimeoutMs);
  g_settings.audioMaxBytes = p.getUInt("wavmax", g_settings.audioMaxBytes);
  g_settings.audioFetchTimeoutMs = p.getUInt("wavtmo", g_settings.audioFetchTimeoutMs);
  g_settings.audioWifiWaitMs = p.getUInt("wavwait", g_settings.audioWifiWaitMs);
  g_settings.announceCooldownMs = p.getUInt("cooldown", g_settings.announceCooldownMs);
  g_settings.adsbUpdateIntervalMs = p.getUInt("adsi", g_settings.adsbUpdateIntervalMs);
  g_settings.adsbMinRequestGapMs = p.getUInt("adsgap", g_settings.adsbMinRequestGapMs);
  g_settings.adsbConnectTimeoutMs = p.getUInt("adsc", g_settings.adsbConnectTimeoutMs);
  g_settings.adsbHttpTimeoutMs = p.getUInt("adsh", g_settings.adsbHttpTimeoutMs);
  g_settings.aircraftMaxAgeMs = p.getUInt("acage", g_settings.aircraftMaxAgeMs);
  g_settings.aircraftMaxExtrapolationMs = p.getUInt("acextra", g_settings.aircraftMaxExtrapolationMs);
  g_settings.aircraftBlendMs = p.getUInt("acblend", g_settings.aircraftBlendMs);
  g_settings.radarSweepPeriodMs = p.getUInt("swperiod", g_settings.radarSweepPeriodMs);
  g_settings.portalEnabled = p.getUChar("apen", g_settings.portalEnabled);
  g_settings.portalAfterFails = p.getUChar("apfails", g_settings.portalAfterFails);
  g_settings.adsbMode = p.getUChar("adsmode", g_settings.adsbMode);
  g_settings.showGroundAircraft = p.getUChar("ground", g_settings.showGroundAircraft);
  g_settings.nightModeEnabled = p.getUChar("night", g_settings.nightModeEnabled);
  g_settings.nightStartHour = p.getUChar("nightst", g_settings.nightStartHour);
  g_settings.nightEndHour = p.getUChar("nightend", g_settings.nightEndHour);
  g_settings.nightBrightness = p.getUChar("nightbr", g_settings.nightBrightness);
  g_settings.encoderReverse = p.getUChar("encinv", g_settings.encoderReverse);
  g_settings.encoderStepsPerDetent = p.getUChar("encstep", g_settings.encoderStepsPerDetent);
  g_settings.buttonDebounceMs = p.getUShort("debounce", g_settings.buttonDebounceMs);
  g_settings.longPressMs = p.getUShort("longms", g_settings.longPressMs);
  g_settings.audioEnabled = p.getUChar("audio", g_settings.audioEnabled);
  g_settings.debugAircraftTypes = p.getUChar("debugac", g_settings.debugAircraftTypes);
  g_settings.adsbValidateTls = p.getUChar("adstls", g_settings.adsbValidateTls);
  g_settings.radarMaxFps = p.getUChar("fps", g_settings.radarMaxFps);
  g_settings.sweepTrailSegments = p.getUChar("trailn", g_settings.sweepTrailSegments);
  g_settings.sweepTrailDegrees = p.getUShort("traildeg", g_settings.sweepTrailDegrees);
  g_settings.labelsEnabled = p.getUChar("labels", g_settings.labelsEnabled);
  g_settings.labelMax = p.getUChar("labelmax", g_settings.labelMax);
  g_settings.aircraftIconScale = p.getFloat("iconscale", g_settings.aircraftIconScale);
  g_settings.rangeIndex = p.getUChar("range", g_settings.rangeIndex);
  g_settings.theme      = p.getUChar("theme", g_settings.theme);
  g_settings.brightness = p.getUChar("bright", g_settings.brightness);
  g_settings.sweepSpeed = p.getUChar("sweep", g_settings.sweepSpeed);
  g_settings.distUnit   = p.getUChar("dunit", g_settings.distUnit);
  g_settings.altUnit    = p.getUChar("aunit", g_settings.altUnit);
  g_settings.speedUnit  = p.getUChar("sunit", g_settings.speedUnit);
  }
  p.end();
  sanitize(g_settings);
  if (!sameCfg) settingsSave();   // remember the new config.h signature
}

void settingsFactoryReset() {
  settingsResetToUserDefaults();
}

void settingsResetToUserDefaults() {
  applyDefaults(g_settings);
  g_settings.wifiSsid[0] = 0;
  g_settings.wifiPass[0] = 0;
  g_settings.homeLat = 0.0;
  g_settings.homeLon = 0.0;
  g_settings.homeConfigured = 0;
  sanitize(g_settings);
  settingsSave();
  s_homeRev++;
}

void settingsSetWifi(const char* ssid, const char* pass) {
  fr_copy(g_settings.wifiSsid, sizeof(g_settings.wifiSsid), ssid);
  fr_copy(g_settings.wifiPass, sizeof(g_settings.wifiPass), pass);
  settingsSave();
}

void settingsSetTrainStation(const char* crs) {
  fr_copy(g_settings.trainStationCrs, sizeof(g_settings.trainStationCrs), crs);
  sanitize(g_settings);   // upper-cases + validates down to 3 letters, or falls back to the default
  settingsSave();
}

void settingsSetTrainApiKey(const char* key) {
  fr_copy(g_settings.trainApiKey, sizeof(g_settings.trainApiKey), key);
  sanitize(g_settings);
  settingsSave();
}

bool settingsSetHome(double lat, double lon) {
  if (!settingsHomeValid(lat, lon)) return false;
  g_settings.homeLat = lat;
  g_settings.homeLon = lon;
  g_settings.homeConfigured = 1;
  s_homeRev++;
  settingsSave();
  return true;
}

void settingsClearHome() {
  g_settings.homeLat = 0.0;
  g_settings.homeLon = 0.0;
  g_settings.homeConfigured = 0;
  s_homeRev++;
  settingsSave();
}

void settingsMarkDirty(uint32_t nowMs) { s_dirty = true; s_dirtyAt = nowMs; }

void settingsService(uint32_t nowMs) {
  if (s_dirty && (int32_t)(nowMs - s_dirtyAt) > 3000) settingsSave();
}

// ---------------------------------------------------------------------------
//  Formatting
// ---------------------------------------------------------------------------
const char* unitDistanceName() { return g_settings.distUnit ? "KM" : "NM"; }

void fmtDistance(char* out, size_t n, float nm, bool withUnit) {
  float v = g_settings.distUnit ? nm * 1.852f : nm;
  const char* u = withUnit ? (g_settings.distUnit ? " KM" : " NM") : "";
  if (fabsf(v - roundf(v)) < 0.05f || v >= 100.0f) snprintf(out, n, "%.0f%s", v, u);
  else snprintf(out, n, "%.1f%s", v, u);
}

static void fmtThousands(char* out, size_t n, long v) {
  char tmp[16];
  snprintf(tmp, sizeof(tmp), "%ld", v < 0 ? -v : v);
  size_t len = strlen(tmp), o = 0;
  char res[24];
  if (v < 0) res[o++] = '-';
  for (size_t i = 0; i < len; i++) {
    res[o++] = tmp[i];
    size_t rem = len - 1 - i;
    if (rem > 0 && rem % 3 == 0) res[o++] = ',';
  }
  res[o] = 0;
  fr_copy(out, n, res);
}

void fmtAltitude(char* out, size_t n, int32_t ft, bool has, bool onGround) {
  if (onGround) { fr_copy(out, n, "GROUND"); return; }
  if (!has) { fr_copy(out, n, "N/A"); return; }
  long v = g_settings.altUnit ? lroundf(ft * 0.3048f) : (long)ft;
  char num[20];
  fmtThousands(num, sizeof(num), v);
  snprintf(out, n, "%s %s", num, g_settings.altUnit ? "M" : "FT");
}

void fmtSpeed(char* out, size_t n, float kt, bool has) {
  if (!has) { fr_copy(out, n, "N/A"); return; }
  if (g_settings.speedUnit) snprintf(out, n, "%ld KM/H", lroundf(kt * 1.852f));
  else snprintf(out, n, "%ld KT", lroundf(kt));
}

// ---------------------------------------------------------------------------
//  Serial console (no phone / PC needed after set-up, but handy)
// ---------------------------------------------------------------------------
static void consoleHelp() {
  Serial.println(F("Commands (type in the Serial Monitor, then press Enter):"));
  Serial.println(F("  STATUS            show WiFi / data status"));
  Serial.println(F("  SSID=<name>       set WiFi network name"));
  Serial.println(F("  PASS=<password>   set WiFi password"));
  Serial.println(F("  LAT=<degrees>     set home latitude  (e.g. LAT=51.47)"));
  Serial.println(F("  LON=<degrees>     set home longitude (e.g. LON=-0.4543)"));
  Serial.println(F("  TRAINKEY=<key>    set the Rail Data Marketplace API key (blank to clear)"));
  Serial.println(F("  PORTAL            start the WiFi setup portal"));
  Serial.println(F("  RESTART           reboot"));
  Serial.println(F("  FACTORY           erase saved settings"));
}

static void consoleStatus() {
  char ip[20];
  wifiIpString(ip, sizeof(ip));
  AdsbStatus st;
  adsbGetStatus(st);
  Serial.printf("WiFi: %s  SSID: '%s'  IP: %s  RSSI: %d\n", wifiStateName(), g_settings.wifiSsid, ip, wifiRssi());
  Serial.printf("Home: %.5f, %.5f  Range: %u nm\n", g_settings.homeLat, g_settings.homeLon, (unsigned)RANGES_NM[g_settings.rangeIndex]);
  Serial.printf("Data: %s  aircraft=%u  http=%d  error='%s'\n", st.lastOk ? "OK" : "NO DATA", st.lastCount, st.lastHttpCode, st.lastError);
  Serial.printf("Heap free: %u  PSRAM free: %u\n", (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getFreePsram());
}

static void consoleLine(char* line) {
  while (*line == ' ' || *line == '\t') line++;
  size_t len = strlen(line);
  while (len && (line[len - 1] == ' ' || line[len - 1] == '\t')) line[--len] = 0;
  if (!len) return;
  char* eq = strchr(line, '=');
  char* val = nullptr;
  if (eq) { *eq = 0; val = eq + 1; }
  if (!strcasecmp(line, "HELP") || !strcmp(line, "?")) consoleHelp();
  else if (!strcasecmp(line, "STATUS")) consoleStatus();
  else if (!strcasecmp(line, "SSID") && val) {
    settingsSetWifi(val, g_settings.wifiPass);
    Serial.printf("SSID set to '%s'\n", g_settings.wifiSsid);
    wifiReconnectNow();
  } else if (!strcasecmp(line, "PASS") && val) {
    settingsSetWifi(g_settings.wifiSsid, val);
    Serial.println(F("Password saved."));
    wifiReconnectNow();
  } else if (!strcasecmp(line, "LAT") && val) {
    if (settingsSetHome(atof(val), g_settings.homeLon)) Serial.printf("Home latitude = %.5f\n", g_settings.homeLat);
    else Serial.println(F("Invalid latitude (-90..90)."));
  } else if (!strcasecmp(line, "LON") && val) {
    if (settingsSetHome(g_settings.homeLat, atof(val))) Serial.printf("Home longitude = %.5f\n", g_settings.homeLon);
    else Serial.println(F("Invalid longitude (-180..180)."));
  } else if (!strcasecmp(line, "TRAINKEY") && val) {
    settingsSetTrainApiKey(val);   // TRAINKEY= (nothing after the =) clears it
    trainRequestNow();
    Serial.println(g_settings.trainApiKey[0] ? F("Rail Data Marketplace API key saved.") : F("Rail Data Marketplace API key cleared."));
  } else if (!strcasecmp(line, "PORTAL")) {
    wifiStartPortal(true);
    Serial.println(F("Setup portal started."));
  } else if (!strcasecmp(line, "RESTART")) {
    Serial.println(F("Restarting..."));
    delay(200);
    ESP.restart();
  } else if (!strcasecmp(line, "FACTORY")) {
    settingsFactoryReset();
    Serial.println(F("Settings erased. Restarting..."));
    delay(200);
    ESP.restart();
  } else {
    Serial.println(F("Unknown command. Type HELP."));
  }
}

void settingsConsole() {
  static char line[160];
  static uint8_t len = 0;
  while (Serial.available() > 0) {
    int c = Serial.read();
    if (c < 0) break;
    if (c == '\r' || c == '\n') {
      if (len) { line[len] = 0; consoleLine(line); len = 0; }
    } else if (len < sizeof(line) - 1) {
      line[len++] = (char)c;
    }
  }
}
