#include "train.h"
#include "config.h"
#include "settings.h"
#include "wifi_manager.h"
#include "util.h"
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <string.h>
#include <ctype.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// ---------------------------------------------------------------------------
//  Shared state (written only by trainTask, read by the UI)
// ---------------------------------------------------------------------------
static portMUX_TYPE   s_mux = portMUX_INITIALIZER_UNLOCKED;
static TrainStatus     s_status;
static TrainService    s_services[TRAIN_MAX_SERVICES];
static uint16_t         s_count = 0;
static volatile bool    s_active = false;   // only true while the train screen is on-screen
static volatile bool    s_force = false;

bool trainIsConfigured() { return OPENLDBWS_API_TOKEN[0] != 0; }

void trainSetActive(bool active) {
  s_active = active;
  if (active) s_force = true;    // refresh immediately on entering the screen
}
void trainRequestNow() { s_force = true; }

void trainGetStatus(TrainStatus& out) {
  portENTER_CRITICAL(&s_mux);
  out = s_status;
  portEXIT_CRITICAL(&s_mux);
}

uint16_t trainGetServices(TrainService* out, uint16_t maxOut) {
  portENTER_CRITICAL(&s_mux);
  uint16_t n = s_count < maxOut ? s_count : maxOut;
  for (uint16_t i = 0; i < n; ++i) out[i] = s_services[i];
  portEXIT_CRITICAL(&s_mux);
  return n;
}

// ---------------------------------------------------------------------------
//  Tiny, pragmatic XML reader - this is not a general-purpose XML parser,
//  just enough to pull named elements out of an OpenLDBWS response. It
//  ignores the namespace prefix on each tag (lt2/lt4/lt5/lt7/lt8... differ
//  across WSDL versions and aren't worth hard-coding) and matches on the
//  local element name only, scoped to whatever substring you hand it.
// ---------------------------------------------------------------------------
// Finds the first <PREFIX:localName>...</PREFIX:localName> at or after
// "from" within xml[0..xmlLen). On success returns true and sets
// contentStart/contentEnd (the inner text span) and afterEnd (index just
// past the closing tag, handy as the next search's "from").
static bool xmlFind(const String& xml, int from, const char* localName,
                    int& contentStart, int& contentEnd, int& afterEnd) {
  if (from < 0) return false;
  String openTail = ":";
  openTail += localName;
  openTail += ">";
  int p = xml.indexOf(openTail, from);
  while (p >= 0) {
    int lt = xml.lastIndexOf('<', p);
    if (lt < 0 || lt < from - 1) return false;
    // reject a false match where ":localName>" is actually the tail of a
    // longer, different element name (e.g. "std" matching inside "etd")
    bool prefixOk = true;
    for (int i = lt + 1; i < p; ++i) {
      char c = xml[i];
      if (!isalnum((unsigned char)c)) { prefixOk = false; break; }
    }
    if (prefixOk) {
      String prefix = xml.substring(lt + 1, p);
      contentStart = p + openTail.length();
      String closeTag = "</";
      closeTag += prefix;
      closeTag += ":";
      closeTag += localName;
      closeTag += ">";
      int endP = xml.indexOf(closeTag, contentStart);
      if (endP >= 0) {
        contentEnd = endP;
        afterEnd = endP + closeTag.length();
        return true;
      }
    }
    p = xml.indexOf(openTail, p + 1);
  }
  return false;
}

// Decodes the handful of XML entities OpenLDBWS actually uses in station/
// destination names (&amp; &apos; &quot; &lt; &gt;) into "out".
static void xmlUnescapeInto(char* out, size_t n, const String& in) {
  size_t o = 0;
  for (int i = 0; i < (int)in.length() && o + 1 < n; ) {
    if (in[i] == '&') {
      struct { const char* ent; char ch; } ents[] = {
        {"&amp;", '&'}, {"&apos;", '\''}, {"&quot;", '"'}, {"&lt;", '<'}, {"&gt;", '>'}
      };
      bool matched = false;
      for (auto& e : ents) {
        size_t elen = strlen(e.ent);
        if ((size_t)(in.length() - i) >= elen && in.substring(i, i + elen) == e.ent) {
          out[o++] = e.ch;
          i += (int)elen;
          matched = true;
          break;
        }
      }
      if (!matched) { out[o++] = in[i++]; }
    } else {
      out[o++] = in[i++];
    }
  }
  out[o] = 0;
}

static void setError(const char* msg, int http) {
  portENTER_CRITICAL(&s_mux);
  s_status.lastOk = false;
  s_status.lastHttpCode = http;
  fr_copy(s_status.lastError, sizeof(s_status.lastError), msg);
  portEXIT_CRITICAL(&s_mux);
}

// One fetch + parse cycle. Returns true on success (s_services/s_count/s_status updated).
static bool fetchBoard(const char* crs) {
  if (!trainIsConfigured()) { setError("NO API TOKEN CONFIGURED", 0); return false; }
  if (!wifiIsConnected())   { setError("NO WIFI", 0); return false; }

  char body[900];
  snprintf(body, sizeof(body),
    "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
    "<soap:Envelope xmlns:soap=\"http://schemas.xmlsoap.org/soap/envelope/\" "
    "xmlns:typ=\"http://thalesgroup.com/RTTI/2013-11-28/Token/types\" "
    "xmlns:ldb=\"http://thalesgroup.com/RTTI/2017-10-01/ldb/\">"
    "<soap:Header><typ:AccessToken><typ:TokenValue>%s</typ:TokenValue></typ:AccessToken></soap:Header>"
    "<soap:Body><ldb:GetDepartureBoardRequest><ldb:numRows>%u</ldb:numRows>"
    "<ldb:crs>%s</ldb:crs></ldb:GetDepartureBoardRequest></soap:Body></soap:Envelope>",
    OPENLDBWS_API_TOKEN, (unsigned)TRAIN_MAX_ROWS, crs);

  WiFiClientSecure secure;
  secure.setInsecure();     // encrypted but the server cert isn't pinned/verified, same approach as the ADS-B feeds
  secure.setHandshakeTimeout(10);

  HTTPClient http;
  http.setReuse(false);
  http.setConnectTimeout(TRAIN_CONNECT_TIMEOUT_MS);
  http.setTimeout(TRAIN_HTTP_TIMEOUT_MS);
  if (!http.begin(secure, OPENLDBWS_ENDPOINT)) { setError("BAD URL", 0); return false; }
  http.addHeader("Content-Type", "text/xml; charset=utf-8");
  http.addHeader("SOAPAction", "\"http://thalesgroup.com/RTTI/2017-10-01/ldb/GetDepartureBoard\"");
  http.addHeader("User-Agent", "FlightRadar-ESP32/" FW_VERSION);

  int httpCode = http.POST((uint8_t*)body, strlen(body));
  if (httpCode != HTTP_CODE_OK) {
    http.end();
    if (httpCode < 0) setError("NO CONNECTION", httpCode);
    else {
      char msg[24];
      snprintf(msg, sizeof(msg), "HTTP %d", httpCode);
      setError(msg, httpCode);
    }
    return false;
  }

  String xml = http.getString();
  http.end();
  if (!xml.length()) { setError("EMPTY RESPONSE", httpCode); return false; }

  // Fault check (bad/expired token, unknown CRS, etc.)
  int fs, fe, fa;
  if (xmlFind(xml, 0, "faultstring", fs, fe, fa)) {
    char msg[40];
    xmlUnescapeInto(msg, sizeof(msg), xml.substring(fs, fe));
    setError(msg, httpCode);
    return false;
  }

  TrainStatus newStatus;
  memset(&newStatus, 0, sizeof(newStatus));
  newStatus.lastHttpCode = httpCode;

  int cs, ce, ca;
  if (xmlFind(xml, 0, "locationName", cs, ce, ca))
    xmlUnescapeInto(newStatus.locationName, sizeof(newStatus.locationName), xml.substring(cs, ce));
  else
    fr_copy(newStatus.locationName, sizeof(newStatus.locationName), crs);

  if (xmlFind(xml, 0, "nrccMessages", cs, ce, ca)) {
    // Each message is wrapped in its own <...:message>...</...:message>
    // inside nrccMessages; just surface the first one.
    int ms, me, ma;
    if (xmlFind(xml, cs, "message", ms, me, ma) && ma <= ce)
      xmlUnescapeInto(newStatus.nrccMessage, sizeof(newStatus.nrccMessage), xml.substring(ms, me));
  }

  TrainService staged[TRAIN_MAX_SERVICES];
  uint16_t n = 0;

  int svcStart, svcEnd, svcAfter;
  int searchFrom = 0;
  while (n < TRAIN_MAX_SERVICES && xmlFind(xml, searchFrom, "service", svcStart, svcEnd, svcAfter)) {
    searchFrom = svcAfter;
    TrainService& svc = staged[n];
    memset(&svc, 0, sizeof(svc));
    fr_copy(svc.platform, sizeof(svc.platform), "-");
    fr_copy(svc.etd, sizeof(svc.etd), "?");

    int fStart, fEnd, fAfter;
    if (xmlFind(xml, svcStart, "std", fStart, fEnd, fAfter) && fAfter <= svcEnd)
      xmlUnescapeInto(svc.std, sizeof(svc.std), xml.substring(fStart, fEnd));
    else
      fr_copy(svc.std, sizeof(svc.std), "--:--");

    if (xmlFind(xml, svcStart, "etd", fStart, fEnd, fAfter) && fAfter <= svcEnd)
      xmlUnescapeInto(svc.etd, sizeof(svc.etd), xml.substring(fStart, fEnd));

    if (xmlFind(xml, svcStart, "platform", fStart, fEnd, fAfter) && fAfter <= svcEnd)
      xmlUnescapeInto(svc.platform, sizeof(svc.platform), xml.substring(fStart, fEnd));

    // Destination name is nested inside <...:destination><...:location>
    // <...:locationName>...</...:locationName> - scope the search to the
    // destination block so this doesn't accidentally pick up the origin's
    // name instead.
    int dStart, dEnd, dAfter;
    if (xmlFind(xml, svcStart, "destination", dStart, dEnd, dAfter) && dEnd <= svcEnd) {
      if (xmlFind(xml, dStart, "locationName", fStart, fEnd, fAfter) && fAfter <= dEnd)
        xmlUnescapeInto(svc.destination, sizeof(svc.destination), xml.substring(fStart, fEnd));
    }
    if (!svc.destination[0]) fr_copy(svc.destination, sizeof(svc.destination), "UNKNOWN");

    ++n;
  }

  newStatus.lastOk = true;
  newStatus.count = n;
  newStatus.lastSuccessMs = millis();
  newStatus.lastError[0] = 0;

  portENTER_CRITICAL(&s_mux);
  s_status = newStatus;
  s_count = n;
  for (uint16_t i = 0; i < n; ++i) s_services[i] = staged[i];
  portEXIT_CRITICAL(&s_mux);
  return true;
}

static void trainTask(void*) {
  uint32_t nextDue = 0;
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(250));
    if (!s_active) continue;
    const uint32_t now = millis();
    const bool due = (int32_t)(now - nextDue) >= 0;
    if (!(s_force || due)) continue;
    s_force = false;

    char crs[8];
    fr_copy(crs, sizeof(crs), g_settings.trainStationCrs);
    const bool ok = fetchBoard(crs);
    nextDue = millis() + (ok ? TRAIN_UPDATE_INTERVAL_MS : 15000UL);
    if (!ok) {
      TrainStatus st;
      trainGetStatus(st);
      Serial.printf("[train] fetch failed: %s (http %d)\n", st.lastError, st.lastHttpCode);
    } else {
      Serial.printf("[train] %s: %u services\n", g_settings.trainStationCrs, (unsigned)s_count);
    }
  }
}

void trainInit() {
  memset(&s_status, 0, sizeof(s_status));
  fr_copy(s_status.lastError, sizeof(s_status.lastError),
          trainIsConfigured() ? "NOT YET REQUESTED" : "NO API TOKEN CONFIGURED");
  xTaskCreatePinnedToCore(trainTask, "train", 8192, nullptr, 1, nullptr, 0);
}
