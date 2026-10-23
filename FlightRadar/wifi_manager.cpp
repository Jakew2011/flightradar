#include "wifi_manager.h"
#include "config.h"
#include "settings.h"
#include "display.h"
#include "adsb_api.h"
#include "audio.h"
#include "train.h"
#include "util.h"
#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>

static WifiState  s_state = WIFI_ST_IDLE;
static uint32_t   s_attemptStart = 0;
static uint32_t   s_nextRetry = 0;
static uint16_t   s_fails = 0;

static bool       s_portal = false;
static bool       s_portalManual = false;
static bool       s_portalBootForced = false;   // BACK held at power-on: stay in setup mode even if saved WiFi connects
static uint32_t   s_portalStart = 0;
static char       s_activePortalSsid[33] = "";
static char       s_activePortalPass[65] = "";
static WebServer* s_server = nullptr;
static DNSServer* s_dns = nullptr;
static bool s_invalidSettingsInput = false;

// ---------------------------------------------------------------------------
//  Clock
// ---------------------------------------------------------------------------
bool clockGet(struct tm* out) { return getLocalTime(out, 0); }

void clockFormat(char* out, size_t n, time_t t) {
  struct tm tmv;
  if (t < 1600000000 || !localtime_r(&t, &tmv)) { fr_copy(out, n, "--:--:--"); return; }
  snprintf(out, n, "%02d:%02d:%02d", tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
}

// ---------------------------------------------------------------------------
//  Portal (web page for entering WiFi + home position)
// ---------------------------------------------------------------------------
static void htmlEscape(String& out, const char* s) {
  for (; *s; ++s) {
    switch (*s) {
      case '&': out += F("&amp;"); break;
      case '<': out += F("&lt;"); break;
      case '>': out += F("&gt;"); break;
      case '"': out += F("&quot;"); break;
      case '\'': out += F("&#39;"); break;
      default: out += *s;
    }
  }
}

static void sendPage(const String& body) {
  String h;
  h.reserve(body.length() + 600);
  h += F("<!doctype html><html><head><meta charset='utf-8'>"
         "<meta name='viewport' content='width=device-width,initial-scale=1'>"
         "<title>Flight Radar setup</title><style>"
         "body{font-family:sans-serif;max-width:420px;margin:1em auto;padding:0 1em}"
         "input{width:100%;padding:.6em;margin:.3em 0 1em;box-sizing:border-box;font-size:1em}"
         "button{padding:.8em 1.4em;font-size:1em}small{color:#666}</style></head><body>");
  h += body;
  h += F("</body></html>");
  s_server->send(200, "text/html", h);
}

static void handleRoot() {
  String b;
  b.reserve(2000);
  b += F("<h2>Flight Radar setup</h2><form method='POST' action='/save'>"
         "<label>WiFi network (SSID, 2.4 GHz)</label><input name='ssid' list='nets' maxlength='32' value='");
  htmlEscape(b, g_settings.wifiSsid);
  b += F("'><datalist id='nets'>");
  int n = WiFi.scanComplete();
  for (int i = 0; i < n && i < 20; i++) {
    b += F("<option value='");
    htmlEscape(b, WiFi.SSID(i).c_str());
    b += F("'>");
  }
  b += F("</datalist><label>WiFi password</label>"
         "<input name='pass' type='password' maxlength='64' placeholder='leave empty to keep the saved password'>"
         "<label>Your latitude (decimal degrees, north +)</label><input id='lat' name='lat' inputmode='decimal' value='");
  if (g_settings.homeConfigured) b += String(g_settings.homeLat, 5);
  b += F("'><label>Your longitude (decimal degrees, east +)</label><input id='lon' name='lon' inputmode='decimal' value='");
  if (g_settings.homeConfigured) b += String(g_settings.homeLon, 5);
  b += F("'><button type='button' onclick='frUseLoc()'>Use my current location</button> "
         "<small id='frLocMsg'></small>"
         "<label><input type='checkbox' name='open' value='1' style='width:auto'> "
         "My WiFi network has no password</label><br><br>"
         "<button type='submit'>Save and connect</button></form>"
         "<p><a href='/settings'>Advanced settings</a></p>"
         "<script>function frUseLoc(){"
         "var m=document.getElementById('frLocMsg');"
         "if(!navigator.geolocation){m.textContent=' Geolocation is not available in this browser.';return;}"
         "m.textContent=' Locating...';"
         "navigator.geolocation.getCurrentPosition(function(p){"
         "document.getElementById('lat').value=p.coords.latitude.toFixed(5);"
         "document.getElementById('lon').value=p.coords.longitude.toFixed(5);"
         "m.textContent=' Filled in (accuracy about '+Math.round(p.coords.accuracy)+' m). Check it looks right, then Save.';"
         "},function(e){"
         "m.textContent=' Could not get your location: '+e.message+"
         "' - note some browsers block this on a page like this one that isn\\'t HTTPS; type coordinates in manually if so.';"
         "},{enableHighAccuracy:true,timeout:10000});"
         "}</script>");
  sendPage(b);
}

static void handleSave() {
  String ssid = s_server->arg("ssid");
  String pass = s_server->arg("pass");
  String lat = s_server->arg("lat");
  String lon = s_server->arg("lon");
  String msg;
  bool wifiChanged = false;

  ssid.trim();
  const bool open = s_server->arg("open") == "1";
  if (ssid.length() > 0 && ssid.length() <= 32 &&
      (open || pass.length() == 0 || (pass.length() >= 8 && pass.length() <= 63))) {
    const bool sameSsid = strcmp(ssid.c_str(), g_settings.wifiSsid) == 0;
    if (open || pass.length() > 0) {
      const char* newPass = open ? "" : pass.c_str();
      settingsSetWifi(ssid.c_str(), newPass);
      wifiChanged = true;
      msg += F("<p>WiFi settings saved.</p>");
    } else if (sameSsid) {
      msg += F("<p>Saved WiFi password kept unchanged.</p>");
    } else {
      msg += F("<p><b>Enter the password for a new network, or select the open-network option.</b></p>");
    }
  } else if (ssid.length() > 32 || pass.length() > 63 ||
             (pass.length() > 0 && pass.length() < 8) || !ssid.length()) {
    msg += F("<p><b>WiFi network name/password length is invalid; credentials were not changed.</b></p>");
  }
  if (lat.length() && lon.length()) {
    char* latEnd = nullptr;
    char* lonEnd = nullptr;
    const double la = strtod(lat.c_str(), &latEnd);
    const double lo = strtod(lon.c_str(), &lonEnd);
    if (latEnd && !*latEnd && lonEnd && !*lonEnd && settingsHomeValid(la, lo)) {
      if (fabs(la - g_settings.homeLat) > 1e-7 || fabs(lo - g_settings.homeLon) > 1e-7) {
        settingsSetHome(la, lo);
        msg += F("<p>Home position saved.</p>");
      }
    } else {
      msg += F("<p><b>Latitude/longitude not valid - position not changed.</b></p>");
    }
  } else if (!lat.length() && !lon.length() && g_settings.homeConfigured) {
    settingsClearHome();
    msg += F("<p>Home coordinates cleared.</p>");
  } else if (lat.length() != lon.length()) {
    msg += F("<p><b>Enter both coordinates, or clear both.</b></p>");
  }
  if (!msg.length()) msg = F("<p>Nothing changed.</p>");
  sendPage(String(F("<h2>Flight Radar setup</h2>")) + msg +
           F("<p>The radar is now connecting. You can close this page and reconnect your "
             "phone or computer to your normal WiFi.</p><p><a href='/'>Back</a></p>"));
  if (wifiChanged) wifiReconnectNow();
}

    static void addTextInput(String& body, const char* label, const char* name,
                             const char* value, size_t maxLength, bool password = false) {
      body += F("<label>");
      body += label;
      body += F("</label><input name='");
      body += name;
      body += F("' maxlength='");
      body += String(maxLength);
      body += F("' value='");
      if (!password) htmlEscape(body, value);
      body += F("'");
      if (password) body += F(" type='password' placeholder='leave blank to keep saved value'");
      body += F(">");
    }

    static void addNumberInput(String& body, const char* label, const char* name, uint32_t value) {
      body += F("<label>");
      body += label;
      body += F("</label><input type='number' name='");
      body += name;
      body += F("' value='");
      body += String(value);
      body += F("'>");
    }

    static bool validAdsbUrlFormat(const char* format) {
      if (!format || (strncmp(format, "http://", 7) && strncmp(format, "https://", 8))) return false;
      unsigned floats = 0, radii = 0;
      for (const char* p = format; *p; ++p) {
        if (*p != '%') continue;
        if (p[1] == '%') { ++p; continue; }
        if (p[1] == '.' && p[2] == '4' && p[3] == 'f') { ++floats; p += 3; continue; }
        if (p[1] == 'u') { ++radii; ++p; continue; }
        return false;
      }
      return floats == 2 && radii == 1;
    }

    static bool readUnsigned(const char* name, uint32_t& value, uint32_t minValue, uint32_t maxValue) {
      if (!s_server->hasArg(name)) return false;
      const String raw = s_server->arg(name);
      if (!raw.length()) { s_invalidSettingsInput = true; return false; }
      char* end = nullptr;
      const unsigned long parsed = strtoul(raw.c_str(), &end, 10);
      if (!end || *end || parsed < minValue || parsed > maxValue) {
        s_invalidSettingsInput = true;
        return false;
      }
      value = (uint32_t)parsed;
      return true;
    }

    static void handleSettingsPage() {
      String b;
      b.reserve(9000);
      b += F("<h2>Flight Radar settings</h2><form method='POST' action='/settings/save'>"
             "<h3>Network and portal</h3>");
      addTextInput(b, "WiFi hostname", "host", g_settings.wifiHostname, 32);
      addTextInput(b, "Setup network name", "apsid", g_settings.portalSsid, 32);
      addTextInput(b, "Setup network password (blank keeps current)", "appass", "", 63, true);
      b += F("<label><input type='checkbox' name='clearappass' value='1' style='width:auto'> Clear setup-network password</label><br>");
      b += F("<label><input type='checkbox' name='portal' value='1' style='width:auto'");
      if (g_settings.portalEnabled) b += F(" checked");
      b += F("> Enable setup portal and automatic fallback</label>");
      addNumberInput(b, "Failed WiFi attempts before portal", "apfails", g_settings.portalAfterFails);
      addNumberInput(b, "WiFi attempt timeout (ms)", "wictmo", g_settings.wifiConnectTimeoutMs);
      addNumberInput(b, "Retry minimum (ms)", "wimin", g_settings.wifiRetryMinMs);
      addNumberInput(b, "Retry maximum (ms)", "wimax", g_settings.wifiRetryMaxMs);
      addNumberInput(b, "Portal timeout (ms)", "aptmo", g_settings.portalTimeoutMs);
      addTextInput(b, "Time zone (POSIX TZ string)", "tz", g_settings.timezone, 47);
      b += F("<h3>Aircraft data</h3><label>ADS-B source mode</label><select name='adsmode'>");
      const char* modes[] = {"Both free sources", "adsb.lol only", "adsb.fi only", "Custom URL", "airplanes.live only"};
      for (uint8_t i = 0; i < 5; ++i) {
        b += F("<option value='"); b += String(i); b += F("'");
        if (g_settings.adsbMode == i) b += F(" selected");
        b += F(">"); b += modes[i]; b += F("</option>");
      }
      b += F("</select>");
      addTextInput(b, "Custom API URL format (use %.4f/%.4f/%u placeholders)", "adscus",
                   g_settings.adsbCustomUrl, 191);
      addNumberInput(b, "Update interval (ms)", "adsi", g_settings.adsbUpdateIntervalMs);
      addNumberInput(b, "Minimum request gap (ms)", "adsgap", g_settings.adsbMinRequestGapMs);
      addNumberInput(b, "API connect timeout (ms)", "adsc", g_settings.adsbConnectTimeoutMs);
      addNumberInput(b, "API response timeout (ms)", "adsh", g_settings.adsbHttpTimeoutMs);
      addNumberInput(b, "Maximum aircraft data age (ms)", "acage", g_settings.aircraftMaxAgeMs);
      addNumberInput(b, "Maximum extrapolation (ms)", "acextra", g_settings.aircraftMaxExtrapolationMs);
      addNumberInput(b, "Aircraft position blend (ms)", "acblend", g_settings.aircraftBlendMs);
      b += F("<label><input type='checkbox' name='ground' value='1' style='width:auto'");
      if (g_settings.showGroundAircraft) b += F(" checked");
      b += F("> Show ground aircraft</label><br>");
      b += F("<label><input type='checkbox' name='adstls' value='1' style='width:auto'");
      if (g_settings.adsbValidateTls) b += F(" checked");
      b += F("> Validate ADS-B TLS certificates (requires configured root CA)</label>");
      b += F("<br><label><input type='checkbox' name='debugac' value='1' style='width:auto'");
      if (g_settings.debugAircraftTypes) b += F(" checked");
      b += F("> Log decoded aircraft type details to Serial</label>");
      b += F("<h3>Audio announcements</h3><label><input type='checkbox' name='audio' value='1' style='width:auto'");
      if (g_settings.audioEnabled) b += F(" checked");
      b += F("> Enable audio announcements</label>");
      addTextInput(b, "Audio WAV URL", "wavurl", g_settings.audioUrl, 255);
      addNumberInput(b, "Maximum WAV size (bytes)", "wavmax", g_settings.audioMaxBytes);
      addNumberInput(b, "WAV download timeout (ms)", "wavtmo", g_settings.audioFetchTimeoutMs);
      addNumberInput(b, "WiFi wait for audio (ms)", "wavwait", g_settings.audioWifiWaitMs);
      addNumberInput(b, "Announcement cooldown (ms)", "cooldown", g_settings.announceCooldownMs);
      b += F("<h3>Train departures</h3>");
      addTextInput(b, "Station (3-letter CRS code, e.g. LIT)", "traincrs", g_settings.trainStationCrs, 3);
      addTextInput(b, "Rail Data Marketplace API key (Consumer key)", "trainkey", g_settings.trainApiKey, 95);
      b += F("<p><small>Shown via the \"Switch mode\" button/menu item on the device. "
             "Get a free key by registering at raildata.org.uk, subscribing to the "
             "\"Live Departure Board\" product, then copying the \"Consumer key\" from its "
             "Specification tab - see the README for the full steps. Paste it above and save; "
             "no reflashing needed.</small></p>");
      b += F("<h3>Display settings</h3><label>Aircraft range</label><select name='range'>");
      for (uint8_t i = 0; i < RANGE_COUNT; ++i) {
        b += F("<option value='"); b += String(i); b += F("'");
        if (g_settings.rangeIndex == i) b += F(" selected");
        b += F(">"); b += String(RANGES_NM[i]); b += F(" NM</option>");
      }
      b += F("</select><label>Theme</label><select name='theme'>");
      const char* themes[] = {"Green", "Amber", "White"};
      for (uint8_t i = 0; i < 3; ++i) {
        b += F("<option value='"); b += String(i); b += F("'");
        if (g_settings.theme == i) b += F(" selected");
        b += F(">"); b += themes[i]; b += F("</option>");
      }
      b += F("</select><label>Brightness (%)</label>");
      addNumberInput(b, "", "bright", g_settings.brightness);
      b += F("<label>Radar speed</label><select name='sweep'>");
      const char* speeds[] = {"Slow", "Normal", "Fast", "Off"};
      for (uint8_t i = 0; i < 4; ++i) {
        b += F("<option value='"); b += String(i); b += F("'");
        if (g_settings.sweepSpeed == i) b += F(" selected");
        b += F(">"); b += speeds[i]; b += F("</option>");
      }
      b += F("</select><label>Distance units</label><select name='dunit'>");
      b += F("<option value='0'"); if (!g_settings.distUnit) b += F(" selected"); b += F(">Nautical miles</option>");
      b += F("<option value='1'"); if (g_settings.distUnit) b += F(" selected"); b += F(">Kilometres</option></select>");
      b += F("<label>Altitude units</label><select name='aunit'>");
      b += F("<option value='0'"); if (!g_settings.altUnit) b += F(" selected"); b += F(">Feet</option>");
      b += F("<option value='1'"); if (g_settings.altUnit) b += F(" selected"); b += F(">Metres</option></select>");
      b += F("<label>Speed units</label><select name='sunit'>");
      b += F("<option value='0'"); if (!g_settings.speedUnit) b += F(" selected"); b += F(">Knots</option>");
      b += F("<option value='1'"); if (g_settings.speedUnit) b += F(" selected"); b += F(">km/h</option></select>");
      b += F("<h3>Radar behavior</h3>");
      b += F("<label><input type='checkbox' name='night' value='1' style='width:auto'");
      if (g_settings.nightModeEnabled) b += F(" checked");
      b += F("> Enable automatic night dimming</label>");
      addNumberInput(b, "Night start hour (0-23)", "nightst", g_settings.nightStartHour);
      addNumberInput(b, "Night end hour (0-23)", "nightend", g_settings.nightEndHour);
      addNumberInput(b, "Night brightness (5-100%)", "nightbr", g_settings.nightBrightness);
      addNumberInput(b, "Radar sweep period (ms)", "swperiod", g_settings.radarSweepPeriodMs);
      addNumberInput(b, "Maximum radar FPS", "fps", g_settings.radarMaxFps);
      addNumberInput(b, "Sweep trail segments", "trailn", g_settings.sweepTrailSegments);
      addNumberInput(b, "Sweep trail angle (degrees)", "traildeg", g_settings.sweepTrailDegrees);
      addNumberInput(b, "Maximum label count", "labelmax", g_settings.labelMax);
      addNumberInput(b, "Aircraft icon scale (percentage)", "iconscale100", (uint32_t)(g_settings.aircraftIconScale * 100.0f + 0.5f));
      b += F("<label><input type='checkbox' name='labels' value='1' style='width:auto'");
      if (g_settings.labelsEnabled) b += F(" checked");
      b += F("> Show aircraft labels</label>");
      b += F("<h3>Controls and advanced</h3><label><input type='checkbox' name='encinv' value='1' style='width:auto'");
      if (g_settings.encoderReverse) b += F(" checked");
      b += F("> Reverse rotary encoder direction</label>");
      addNumberInput(b, "Encoder steps per detent (1, 2 or 4)", "encstep", g_settings.encoderStepsPerDetent);
      addNumberInput(b, "Button debounce (ms)", "debounce", g_settings.buttonDebounceMs);
      addNumberInput(b, "Long press threshold (ms)", "longms", g_settings.longPressMs);
      addNumberInput(b, "Serial baud rate (reboot required)", "baud", g_settings.serialBaud);
      b += F("<p><small>Hardware pins, display wiring, firmware version, and memory-layout limits remain build-time settings.</small></p>"
             "<button type='submit'>Save settings</button></form>"
             "<form method='POST' action='/speaker-test'>"
             "<button type='submit'>Test speaker</button>"
             "<p><small>Plays a test tone immediately, then the configured WAV. Does not change the Audio setting.</small></p></form>"
             "<form method='POST' action='/defaults' onsubmit=\"return confirm('Restore firmware defaults? WiFi credentials and coordinates will be cleared.')\">"
             "<button type='submit'>Restore defaults</button></form>"
             "<p><a href='/'>WiFi and location setup</a></p>");
      sendPage(b);
    }

    static void handleSettingsSave() {
      const Settings old = g_settings;
          s_invalidSettingsInput = false;
          bool customUrlInvalid = false;
          uint32_t value;
    #define READ_SETTING(key, field, minv, maxv) \
      if (readUnsigned(key, value, minv, maxv)) g_settings.field = value
      READ_SETTING("apfails", portalAfterFails, 1, 20);
      READ_SETTING("wictmo", wifiConnectTimeoutMs, 3000, 60000);
      READ_SETTING("wimin", wifiRetryMinMs, 500, 60000);
      READ_SETTING("wimax", wifiRetryMaxMs, 500, 300000);
      READ_SETTING("aptmo", portalTimeoutMs, 30000, 86400000);
      READ_SETTING("adsi", adsbUpdateIntervalMs, 2000, 60000);
      READ_SETTING("adsgap", adsbMinRequestGapMs, 1000, 60000);
      READ_SETTING("adsc", adsbConnectTimeoutMs, 1000, 60000);
      READ_SETTING("adsh", adsbHttpTimeoutMs, 1000, 60000);
      READ_SETTING("acage", aircraftMaxAgeMs, 5000, 300000);
      READ_SETTING("acextra", aircraftMaxExtrapolationMs, 0, 300000);
      READ_SETTING("acblend", aircraftBlendMs, 0, 10000);
      READ_SETTING("wavmax", audioMaxBytes, 4096, 8388608);
      READ_SETTING("wavtmo", audioFetchTimeoutMs, 1000, 60000);
      READ_SETTING("wavwait", audioWifiWaitMs, 0, 300000);
      READ_SETTING("cooldown", announceCooldownMs, 10000, 3600000);
      READ_SETTING("swperiod", radarSweepPeriodMs, 500, 60000);
    #undef READ_SETTING
    #define READ_SMALL(key, field, minv, maxv) \
      if (readUnsigned(key, value, minv, maxv)) g_settings.field = (decltype(g_settings.field))value
      READ_SMALL("adsmode", adsbMode, 0, 4);
      READ_SMALL("nightst", nightStartHour, 0, 23);
      READ_SMALL("nightend", nightEndHour, 0, 23);
      READ_SMALL("nightbr", nightBrightness, 5, 100);
      READ_SMALL("encstep", encoderStepsPerDetent, 1, 4);
      READ_SMALL("debounce", buttonDebounceMs, 5, 250);
      READ_SMALL("longms", longPressMs, 250, 5000);
      READ_SMALL("fps", radarMaxFps, 5, 60);
      READ_SMALL("trailn", sweepTrailSegments, 1, SWEEP_TRAIL_SEGMENTS);
      READ_SMALL("traildeg", sweepTrailDegrees, 1, 180);
      READ_SMALL("labelmax", labelMax, 1, LABEL_MAX);
      READ_SMALL("range", rangeIndex, 0, RANGE_COUNT - 1);
      READ_SMALL("theme", theme, 0, 2);
      READ_SMALL("bright", brightness, 5, 100);
      READ_SMALL("sweep", sweepSpeed, 0, 3);
      READ_SMALL("dunit", distUnit, 0, 1);
      READ_SMALL("aunit", altUnit, 0, 1);
      READ_SMALL("sunit", speedUnit, 0, 1);
      if (readUnsigned("baud", value, 1200, 2000000)) g_settings.serialBaud = value;
      if (readUnsigned("iconscale100", value, 50, 300)) g_settings.aircraftIconScale = value / 100.0f;
    #undef READ_SMALL
      g_settings.portalEnabled = s_server->hasArg("portal") ? 1 : 0;
      g_settings.audioEnabled = s_server->hasArg("audio") ? 1 : 0;
      g_settings.showGroundAircraft = s_server->hasArg("ground") ? 1 : 0;
      g_settings.adsbValidateTls = s_server->hasArg("adstls") ? 1 : 0;
      g_settings.debugAircraftTypes = s_server->hasArg("debugac") ? 1 : 0;
      g_settings.nightModeEnabled = s_server->hasArg("night") ? 1 : 0;
      g_settings.labelsEnabled = s_server->hasArg("labels") ? 1 : 0;
      g_settings.encoderReverse = s_server->hasArg("encinv") ? 1 : 0;

      const char* stringNames[] = {"host", "apsid", "tz", "wavurl", "adscus", "trainkey"};
      char* stringValues[] = {g_settings.wifiHostname, g_settings.portalSsid, g_settings.timezone,
                              g_settings.audioUrl, g_settings.adsbCustomUrl, g_settings.trainApiKey};
      const size_t stringSizes[] = {sizeof(g_settings.wifiHostname), sizeof(g_settings.portalSsid),
                                    sizeof(g_settings.timezone), sizeof(g_settings.audioUrl),
                                    sizeof(g_settings.adsbCustomUrl), sizeof(g_settings.trainApiKey)};
      bool trainKeyChanged = false;
      for (size_t i = 0; i < sizeof(stringNames) / sizeof(stringNames[0]); ++i) {
        if (!s_server->hasArg(stringNames[i])) continue;
        String text = s_server->arg(stringNames[i]);
        text.trim();
        // host/apsid/tz (i<3) must not be blank; wavurl/adscus/trainkey may
        // be left blank to clear them (an empty trainkey just disables the
        // train board, same as never having set one).
        if (text.length() >= stringSizes[i] || ((i < 3) && !text.length())) {
          s_invalidSettingsInput = true;
          continue;
        }
        if (i == 4 && text.length() && !validAdsbUrlFormat(text.c_str())) {
          s_invalidSettingsInput = true;
          customUrlInvalid = true;
          continue;
        }
        if (i == 5 && strcmp(stringValues[5], text.c_str()) != 0) trainKeyChanged = true;
        fr_copy(stringValues[i], stringSizes[i], text.c_str());
      }
      if (s_server->hasArg("appass") && s_server->arg("appass").length() > 0) {
        String pass = s_server->arg("appass");
        if (pass.length() >= 8 && pass.length() <= 63) fr_copy(g_settings.portalPass, sizeof(g_settings.portalPass), pass.c_str());
        else s_invalidSettingsInput = true;
      }
      if (s_server->hasArg("clearappass")) g_settings.portalPass[0] = 0;
      if (s_server->hasArg("traincrs")) {
        String crs = s_server->arg("traincrs");
        crs.trim();
        if (crs.length() == 3) { settingsSetTrainStation(crs.c_str()); trainRequestNow(); }
        else if (crs.length()) s_invalidSettingsInput = true;   // settingsSetTrainStation() sanitizes to 3 letters or falls back - a non-3-length entry just didn't get applied
      }
      if (trainKeyChanged) trainRequestNow();   // new/changed/cleared key: retry the board right away rather than waiting out the normal poll interval
      if (g_settings.wifiRetryMaxMs < g_settings.wifiRetryMinMs)
        { g_settings.wifiRetryMaxMs = g_settings.wifiRetryMinMs; s_invalidSettingsInput = true; }
      if (g_settings.adsbMinRequestGapMs > g_settings.adsbUpdateIntervalMs) {
        g_settings.adsbMinRequestGapMs = g_settings.adsbUpdateIntervalMs;
        s_invalidSettingsInput = true;
      }
      if (g_settings.aircraftMaxExtrapolationMs > g_settings.aircraftMaxAgeMs) {
        g_settings.aircraftMaxExtrapolationMs = g_settings.aircraftMaxAgeMs;
        s_invalidSettingsInput = true;
      }
      if (g_settings.encoderStepsPerDetent != 1 && g_settings.encoderStepsPerDetent != 2 &&
          g_settings.encoderStepsPerDetent != 4) {
        g_settings.encoderStepsPerDetent = old.encoderStepsPerDetent;
        s_invalidSettingsInput = true;
      }
      if ((customUrlInvalid && g_settings.adsbMode == 3) || !g_settings.adsbCustomUrl[0] && g_settings.adsbMode == 3) {
        g_settings.adsbMode = old.adsbMode;
        s_invalidSettingsInput = true;
      }
      const bool baudChanged = old.serialBaud != g_settings.serialBaud;
      const bool audioRestartRequired = old.audioEnabled != g_settings.audioEnabled ||
                                        strcmp(old.audioUrl, g_settings.audioUrl) != 0 ||
                                        old.audioMaxBytes != g_settings.audioMaxBytes ||
                                        old.audioFetchTimeoutMs != g_settings.audioFetchTimeoutMs ||
                                        old.audioWifiWaitMs != g_settings.audioWifiWaitMs;
      const bool hostnameChanged = strcmp(old.wifiHostname, g_settings.wifiHostname) != 0;
      settingsSave();
      adsbSetRange(RANGES_NM[g_settings.rangeIndex]);
      adsbRequestNow();
      displaySetBrightness(g_settings.brightness);
      if (hostnameChanged) {
        WiFi.setHostname(g_settings.wifiHostname);
        wifiReconnectNow();
      }
      if (strcmp(old.timezone, g_settings.timezone) != 0 && wifiIsConnected())
        configTzTime(g_settings.timezone, "pool.ntp.org", "time.google.com");
      String msg = F("<h2>Settings saved</h2><p>Changes are saved to the device.</p>");
      if (strcmp(old.portalSsid, g_settings.portalSsid) != 0 ||
          strcmp(old.portalPass, g_settings.portalPass) != 0)
        msg += F("<p>The setup network will restart with its new name/password.</p>");
      if (s_invalidSettingsInput)
        msg += F("<p><b>Some invalid fields were left unchanged. Correct their values and save again.</b></p>");
      if (baudChanged || audioRestartRequired) {
        msg += F("<p>");
        if (baudChanged) msg += F("Serial baud rate");
        if (baudChanged && audioRestartRequired) msg += F(" and ");
        if (audioRestartRequired) msg += F("audio");
        msg += F(" changes require a reboot. Reboot now?</p>"
                 "<form method='POST' action='/reboot'><button>Reboot now</button></form>"
                 "<p><b>If you choose later, these settings will not fully take effect until the device is rebooted.</b></p>");
      }
      msg += F("<p><a href='/settings'>Back to settings</a></p>");
      sendPage(msg);
    }

    static void handleRestoreDefaults() {
      settingsResetToUserDefaults();
      sendPage(F("<h2>Defaults restored</h2><p>Firmware defaults restored. WiFi credentials and coordinates are blank.</p>"
                 "<p><a href='/'>Configure WiFi</a> / <a href='/settings'>Settings</a></p>"));
      wifiReconnectNow();
    }

    static void handleSpeakerTest() {
      const bool played = audioTestSpeaker();
      sendPage(played
        ? F("<h2>Speaker test started</h2><p>The military announcement clip is playing.</p><p><a href='/settings'>Back to settings</a></p>")
        : F("<h2>Speaker test unavailable</h2><p>The audio system is not ready yet. Check the serial log and I2S wiring.</p><p><a href='/settings'>Back to settings</a></p>"));
    }

    static void handleReboot() {
      sendPage(F("<h2>Restarting</h2><p>The device is rebooting.</p>"));
      delay(250);
      ESP.restart();
    }

// The web server (routes + listener) is independent of the setup AP - it's
// started once, here, and left running for the rest of the session. That
// way the settings page ("by always on wifi portal I mean when it connects
// to wifi it should still be broadcasting its portal but just on wifi, not
// creating its own network") stays reachable at the device's normal IP on
// your home network even after the one-time setup AP has been shut down -
// see wifiStopPortal() below, which only tears down the AP/DNS, not this.
static void startWebServer() {
  if (s_server) return;
  s_server = new WebServer(80);
  s_server->on("/", HTTP_GET, handleRoot);
  s_server->on("/save", HTTP_POST, handleSave);
  s_server->on("/settings", HTTP_GET, handleSettingsPage);
  s_server->on("/settings/save", HTTP_POST, handleSettingsSave);
  s_server->on("/speaker-test", HTTP_POST, handleSpeakerTest);
  s_server->on("/defaults", HTTP_POST, handleRestoreDefaults);
  s_server->on("/reboot", HTTP_POST, handleReboot);
  s_server->onNotFound(handleRoot);          // captive-portal style: any URL shows the form while the AP is up
  s_server->begin();
}

void wifiStartPortal(bool manual) {
  if (!manual && !g_settings.portalEnabled) return;
  if (s_portal) { s_portalManual = s_portalManual || manual; return; }
  WiFi.mode(WIFI_AP_STA);
  WiFi.setHostname(g_settings.wifiHostname);
  const bool ok = (strlen(g_settings.portalPass) >= 8) ? WiFi.softAP(g_settings.portalSsid, g_settings.portalPass)
                                                       : WiFi.softAP(g_settings.portalSsid);
  if (!ok) { Serial.println("[wifi] could not start setup portal"); return; }
  fr_copy(s_activePortalSsid, sizeof(s_activePortalSsid), g_settings.portalSsid);
  fr_copy(s_activePortalPass, sizeof(s_activePortalPass), g_settings.portalPass);
  s_dns = new DNSServer();
  s_dns->start(53, "*", WiFi.softAPIP());
  startWebServer();                          // no-op if it's already running (it usually is, see wifiBegin())
  WiFi.scanNetworks(true);                   // asynchronous scan for the drop-down list
  s_portal = true;
  s_portalManual = manual;
  s_portalStart = millis();
  Serial.printf("[wifi] setup portal: join '%s' and open http://%s/\n", g_settings.portalSsid,
                WiFi.softAPIP().toString().c_str());
}

void wifiStopPortal() {
  if (!s_portal) return;
  // Deliberately does NOT touch s_server - the settings page keeps running
  // and stays reachable at the device's normal (STA) IP address; this only
  // stops the device from also broadcasting its own separate "setup"
  // WiFi network (named from WIFI_PORTAL_SSID in config.h).
  if (s_dns) { s_dns->stop(); delete s_dns; s_dns = nullptr; }
  WiFi.softAPdisconnect(true);
  WiFi.scanDelete();
  WiFi.mode(WIFI_STA);
  s_portal = false;
  s_activePortalSsid[0] = 0;
  s_activePortalPass[0] = 0;
  s_portalManual = false;
  s_portalBootForced = false;
  Serial.println("[wifi] setup AP stopped; settings page still reachable on your WiFi network");
}

bool wifiPortalActive() { return s_portal; }
const char* wifiPortalSsid() { return g_settings.portalSsid; }
void wifiPortalIpString(char* out, size_t n) {
  fr_copy(out, n, s_portal ? WiFi.softAPIP().toString().c_str() : "-");
}

// ---------------------------------------------------------------------------
//  Connection state machine
// ---------------------------------------------------------------------------
static uint32_t backoffMs() {
  uint32_t f = s_fails > 4 ? 4 : s_fails;
  uint32_t ms = g_settings.wifiRetryMinMs << (f > 0 ? f - 1 : 0);
  return ms > g_settings.wifiRetryMaxMs ? g_settings.wifiRetryMaxMs : ms;
}

static void beginConnect() {
  if (g_settings.wifiSsid[0] == 0) {           // nothing configured
    s_state = WIFI_ST_DISCONNECTED;
    s_nextRetry = millis() + 5000;
    if (!s_portal && g_settings.portalEnabled) wifiStartPortal(false);
    return;
  }
  WiFi.mode(s_portal ? WIFI_AP_STA : WIFI_STA);
  WiFi.setHostname(g_settings.wifiHostname);
  WiFi.disconnect(false);
  WiFi.begin(g_settings.wifiSsid, g_settings.wifiPass);
  s_state = WIFI_ST_CONNECTING;
  s_attemptStart = millis();
  Serial.printf("[wifi] connecting to '%s'...\n", g_settings.wifiSsid);
}

void wifiBegin(bool forcePortal) {
  WiFi.persistent(false);                      // we keep credentials ourselves
  WiFi.setAutoReconnect(false);
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(g_settings.wifiHostname);
  WiFi.setSleep(false);
  s_state = WIFI_ST_IDLE;
  startWebServer();                            // runs for the whole session, on AND off the setup AP - see its comment
  if (forcePortal) { s_portalBootForced = true; wifiStartPortal(true); }
}

void wifiReconnectNow() {
  s_fails = 0;
  s_state = WIFI_ST_IDLE;                      // wifiUpdate() will start a fresh attempt
}

void wifiUpdate() {
  const uint32_t now = millis();
  switch (s_state) {
    case WIFI_ST_IDLE:
      beginConnect();
      break;
    case WIFI_ST_CONNECTING:
      if (WiFi.status() == WL_CONNECTED) {
        s_state = WIFI_ST_CONNECTED;
        s_fails = 0;
        configTzTime(g_settings.timezone, "pool.ntp.org", "time.google.com");
        Serial.printf("[wifi] connected, IP %s\n", WiFi.localIP().toString().c_str());
        // Stop broadcasting the device's own separate setup AP on a
        // successful connection, even if it was opened manually ("Open web
        // settings") - previously it stayed broadcasting for the full portal
        // timeout (10 minutes by default) even once already connected to
        // home WiFi, which read as "always on". The settings page itself
        // doesn't go away: wifiStopPortal() only drops the AP, the web
        // server keeps running and stays reachable at the IP shown on the
        // WiFi status screen, now just over your home network instead of the
        // device's own one. The one exception is holding BACK at power-on to
        // force setup mode deliberately - the AP stays up even if the saved
        // network connects in the background, since you asked for setup mode
        // specifically.
        if (s_portal && !s_portalBootForced) wifiStopPortal();
      } else if (now - s_attemptStart > g_settings.wifiConnectTimeoutMs) {
        WiFi.disconnect(false);
        s_fails++;
        s_state = WIFI_ST_DISCONNECTED;
        s_nextRetry = now + backoffMs();
        Serial.printf("[wifi] attempt %u failed, retry in %lu ms\n", (unsigned)s_fails, (unsigned long)backoffMs());
        if (!s_portal && g_settings.portalEnabled &&
            s_fails >= g_settings.portalAfterFails) wifiStartPortal(false);
      }
      break;
    case WIFI_ST_CONNECTED:
      if (WiFi.status() != WL_CONNECTED) {
        s_state = WIFI_ST_DISCONNECTED;
        s_nextRetry = now + g_settings.wifiRetryMinMs;
        Serial.println("[wifi] connection lost");
      }
      break;
    case WIFI_ST_DISCONNECTED:
      if ((int32_t)(now - s_nextRetry) >= 0) beginConnect();
      break;
  }

  // The settings web server runs for the whole session (see startWebServer()
  // / its comment above wifiStartPortal()), not just while the setup AP is
  // broadcasting, so this is unconditional.
  if (s_server) s_server->handleClient();

  if (s_portal) {
    if (s_dns) s_dns->processNextRequest();
    if (s_portal && !g_settings.portalEnabled && !s_portalManual) wifiStopPortal();
    else if (s_portal &&
             (strcmp(s_activePortalSsid, g_settings.portalSsid) != 0 ||
              strcmp(s_activePortalPass, g_settings.portalPass) != 0)) {
      const bool manual = s_portalManual;
      wifiStopPortal();
      wifiStartPortal(manual);
    } else if (s_portal && now - s_portalStart > g_settings.portalTimeoutMs) {
      wifiStopPortal();
    }
  }
}

WifiState wifiState() { return s_state; }
bool wifiIsConnected() { return s_state == WIFI_ST_CONNECTED && WiFi.status() == WL_CONNECTED; }
uint16_t wifiFailCount() { return s_fails; }
int wifiRssi() { return wifiIsConnected() ? WiFi.RSSI() : 0; }

const char* wifiStateName() {
  switch (s_state) {
    case WIFI_ST_CONNECTING:   return "CONNECTING";
    case WIFI_ST_CONNECTED:    return "CONNECTED";
    case WIFI_ST_DISCONNECTED: return "DISCONNECTED";
    default:                   return "STARTING";
  }
}

void wifiIpString(char* out, size_t n) {
  fr_copy(out, n, wifiIsConnected() ? WiFi.localIP().toString().c_str() : "-");
}
