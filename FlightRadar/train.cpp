#include "train.h"
#include "config.h"
#include "settings.h"
#include "wifi_manager.h"
#include "util.h"
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <string.h>
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

bool trainIsConfigured() { return g_settings.trainApiKey[0] != 0; }

static void trainTask(void*);
static bool s_taskStarted = false;

void trainSetActive(bool active) {
  s_active = active;
  if (active) {
    s_force = true;    // refresh immediately on entering the screen
    // The background task is only created the first time it's actually
    // needed, not at boot - it costs an 8 KB stack that sits unused for
    // anyone who never opens the train board, competing with everything
    // else (photo downloads/JPEG decode included) for the same RAM budget.
    if (!s_taskStarted) {
      s_taskStarted = true;
      xTaskCreatePinnedToCore(trainTask, "train", 8192, nullptr, 1, nullptr, 0);
    }
  }
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

static void setError(const char* msg, int http) {
  portENTER_CRITICAL(&s_mux);
  s_status.lastOk = false;
  s_status.lastHttpCode = http;
  fr_copy(s_status.lastError, sizeof(s_status.lastError), msg);
  portEXIT_CRITICAL(&s_mux);
}

// One fetch + parse cycle. Returns true on success (s_services/s_count/s_status updated).
//
// This is the Rail Data Marketplace's Live Departure Board API - a plain
// REST/JSON endpoint (GET .../GetDepartureBoard/{crs}, key in an
// "x-apikey" header) that replaced the old OpenLDBWS SOAP/XML service when
// its self-service registration page was retired. See the comment above
// OPENLDBWS_API_KEY in config.h for how to get a key.
static bool fetchBoard(const char* crs) {
  if (!trainIsConfigured()) { setError("NO API KEY CONFIGURED", 0); return false; }
  if (!wifiIsConnected())   { setError("NO WIFI", 0); return false; }

  char url[180];
  snprintf(url, sizeof(url), "%s/%s?numRows=%u", OPENLDBWS_ENDPOINT, crs, (unsigned)TRAIN_MAX_ROWS);

  WiFiClientSecure secure;
  secure.setInsecure();     // encrypted but the server cert isn't pinned/verified, same approach as the ADS-B feeds
  secure.setHandshakeTimeout(10);

  HTTPClient http;
  http.setReuse(false);
  http.setConnectTimeout(TRAIN_CONNECT_TIMEOUT_MS);
  http.setTimeout(TRAIN_HTTP_TIMEOUT_MS);
  if (!http.begin(secure, url)) { setError("BAD URL", 0); return false; }
  http.addHeader("x-apikey", g_settings.trainApiKey);
  http.addHeader("User-Agent", "FlightRadar-ESP32/" FW_VERSION);

  int httpCode = http.GET();
  if (httpCode != HTTP_CODE_OK) {
    http.end();
    if (httpCode < 0) {
      setError("NO CONNECTION", httpCode);
    } else if (httpCode == 401 || httpCode == 403) {
      setError("BAD/EXPIRED API KEY", httpCode);
    } else {
      char msg[24];
      snprintf(msg, sizeof(msg), "HTTP %d", httpCode);
      setError(msg, httpCode);
    }
    return false;
  }

  JsonDocument doc;
  DeserializationError jsonErr = deserializeJson(doc, http.getStream());
  http.end();
  if (jsonErr) { setError("BAD JSON RESPONSE", httpCode); return false; }

  TrainStatus newStatus;
  memset(&newStatus, 0, sizeof(newStatus));
  newStatus.lastHttpCode = httpCode;

  const char* locName = doc["locationName"] | "";
  fr_copy(newStatus.locationName, sizeof(newStatus.locationName), locName[0] ? locName : crs);

  JsonVariantConst nrcc = doc["nrccMessages"];
  if (nrcc.is<JsonArrayConst>() && nrcc.size() > 0) {
    const char* v = nrcc[0]["Value"] | "";
    fr_copy(newStatus.nrccMessage, sizeof(newStatus.nrccMessage), v);
  }

  TrainService staged[TRAIN_MAX_SERVICES];
  uint16_t n = 0;

  JsonVariantConst services = doc["trainServices"];
  if (services.is<JsonArrayConst>()) {
    for (JsonVariantConst svc : services.as<JsonArrayConst>()) {
      if (n >= TRAIN_MAX_SERVICES) break;
      TrainService& out = staged[n];
      memset(&out, 0, sizeof(out));

      const char* std_ = svc["std"] | "";
      fr_copy(out.std, sizeof(out.std), std_[0] ? std_ : "--:--");

      const bool cancelled = svc["isCancelled"] | false;
      const char* etd = svc["etd"] | "";
      if (cancelled) fr_copy(out.etd, sizeof(out.etd), "Cancelled");
      else fr_copy(out.etd, sizeof(out.etd), etd[0] ? etd : "?");

      const char* plat = svc["platform"] | "";
      fr_copy(out.platform, sizeof(out.platform), plat[0] ? plat : "-");

      // "destination" is an array (usually one entry) of {locationName, crs, ...}.
      const char* dest = "";
      JsonVariantConst destArr = svc["destination"];
      if (destArr.is<JsonArrayConst>() && destArr.size() > 0)
        dest = destArr[0]["locationName"] | "";
      fr_copy(out.destination, sizeof(out.destination), dest[0] ? dest : "UNKNOWN");

      ++n;
    }
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
          trainIsConfigured() ? "NOT YET REQUESTED" : "NO API KEY CONFIGURED");
  // The background task itself is started lazily, the first time
  // trainSetActive(true) is called - see the comment there.
}
