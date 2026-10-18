#include "aircraft_route.h"
#include "config.h"
#include "util.h"
#include "wifi_manager.h"
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <stdio.h>
#include <string.h>

static constexpr uint32_t ROUTE_DEBOUNCE_MS = 350;
static constexpr uint32_t ROUTE_MIN_REQUEST_GAP_MS = 1000;
static constexpr uint32_t ROUTE_TIMEOUT_MS = 8000;
static constexpr uint32_t ROUTE_CONNECT_TIMEOUT_MS = 6000;

static portMUX_TYPE s_routeMux = portMUX_INITIALIZER_UNLOCKED;
static char s_requestedIcao[8] = "";
static char s_requestedCallsign[9] = "";
static uint32_t s_requestGeneration = 0;
static AircraftRouteResult s_pending = {};
static bool s_pendingReady = false;
static AircraftRouteState s_state = AIRCRAFT_ROUTE_IDLE;
static bool s_taskStarted = false;

static bool currentRequest(uint32_t generation, char* icao, size_t icaoSize,
                           char* callsign, size_t callsignSize) {
  bool current;
  portENTER_CRITICAL(&s_routeMux);
  current = generation == s_requestGeneration && s_requestedIcao[0] != 0;
  if (current) {
    fr_copy(icao, icaoSize, s_requestedIcao);
    fr_copy(callsign, callsignSize, s_requestedCallsign);
  }
  portEXIT_CRITICAL(&s_routeMux);
  return current;
}

static void publish(uint32_t generation, const AircraftRouteResult& result) {
  portENTER_CRITICAL(&s_routeMux);
  if (generation == s_requestGeneration && !strcmp(result.icao, s_requestedIcao)) {
    s_pending = result;
    s_pendingReady = true;
    s_state = result.state;
  }
  portEXIT_CRITICAL(&s_routeMux);
}

static AircraftRouteResult lookupRoute(const char* icao, const char* callsign) {
  AircraftRouteResult result = {};
  fr_copy(result.icao, sizeof(result.icao), icao);
  fr_copy(result.callsign, sizeof(result.callsign), callsign);
  result.state = AIRCRAFT_ROUTE_FAILED;
  if (!callsign[0]) {
    result.state = AIRCRAFT_ROUTE_NOT_FOUND;
    return result;
  }

  char url[96];
  int pos = snprintf(url, sizeof(url), "https://api.adsbdb.com/v0/callsign/");
  for (const char* p = callsign; *p; ++p) {
    char c = *p;
    if (c >= 'a' && c <= 'z') c -= 32;
    if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-')) {
      Serial.printf("[route] callsign contains unsupported character: %s\n", callsign);
      return result;
    }
    if (pos < 0 || (size_t)pos + 1 >= sizeof(url)) return result;
    url[pos++] = c;
  }
  url[pos] = 0;

  WiFiClientSecure client;
  client.setInsecure();
  client.setHandshakeTimeout(10);
  HTTPClient http;
  http.setReuse(false);
  http.setConnectTimeout(ROUTE_CONNECT_TIMEOUT_MS);
  http.setTimeout(ROUTE_TIMEOUT_MS);
  if (!http.begin(client, url)) {
    Serial.println("[route] could not open ADSBdb lookup URL");
    return result;
  }
  http.addHeader("Accept", "application/json");
  http.addHeader("User-Agent", "FlightRadar-ESP32/" FW_VERSION);
  const int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("[route] callsign lookup failed: HTTP %d\n", code);
    http.end();
    if (code == HTTP_CODE_NOT_FOUND) result.state = AIRCRAFT_ROUTE_NOT_FOUND;
    return result;
  }

  JsonDocument doc;
  const DeserializationError error = deserializeJson(doc, http.getStream());
  http.end();
  if (error) {
    Serial.printf("[route] invalid lookup JSON: %s\n", error.c_str());
    return result;
  }
  JsonObjectConst route = doc["response"]["flightroute"].as<JsonObjectConst>();
  if (route.isNull()) {
    result.state = AIRCRAFT_ROUTE_NOT_FOUND;
    return result;
  }

  const char* departure = route["origin"]["iata_code"] | "";
  const char* arrival = route["destination"]["iata_code"] | "";
  if (!*departure) departure = route["origin"]["icao_code"] | "";
  if (!*arrival) arrival = route["destination"]["icao_code"] | "";
  if (!*departure || !*arrival) {
    result.state = AIRCRAFT_ROUTE_NOT_FOUND;
    return result;
  }
  fr_copy(result.departure, sizeof(result.departure), departure);
  fr_copy(result.arrival, sizeof(result.arrival), arrival);
  result.state = AIRCRAFT_ROUTE_READY;
  return result;
}

static void routeTask(void*) {
  uint32_t handledGeneration = 0;
  uint32_t lastRequestMs = 0;
  for (;;) {
    char icao[8] = "";
    char callsign[9] = "";
    uint32_t generation;
    portENTER_CRITICAL(&s_routeMux);
    generation = s_requestGeneration;
    fr_copy(icao, sizeof(icao), s_requestedIcao);
    fr_copy(callsign, sizeof(callsign), s_requestedCallsign);
    portEXIT_CRITICAL(&s_routeMux);

    if (!icao[0] || generation == handledGeneration) {
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }

    vTaskDelay(pdMS_TO_TICKS(ROUTE_DEBOUNCE_MS));
    if (!currentRequest(generation, icao, sizeof(icao), callsign, sizeof(callsign))) continue;
    if (!wifiIsConnected()) {
      vTaskDelay(pdMS_TO_TICKS(250));
      continue;
    }
    const uint32_t sinceLast = millis() - lastRequestMs;
    if (lastRequestMs && sinceLast < ROUTE_MIN_REQUEST_GAP_MS) {
      vTaskDelay(pdMS_TO_TICKS(ROUTE_MIN_REQUEST_GAP_MS - sinceLast));
      continue;
    }

    handledGeneration = generation;
    lastRequestMs = millis();
    const AircraftRouteResult result = lookupRoute(icao, callsign);
    publish(generation, result);
    if (result.state == AIRCRAFT_ROUTE_READY) {
      Serial.printf("[route] %s: %s -> %s\n", callsign, result.departure, result.arrival);
    }
  }
}

void aircraftRouteInit() {
  const BaseType_t result = xTaskCreatePinnedToCore(routeTask, "aircraft_route", 12288,
                                                    nullptr, 1, nullptr, 0);
  portENTER_CRITICAL(&s_routeMux);
  s_taskStarted = result == pdPASS;
  if (!s_taskStarted) s_state = AIRCRAFT_ROUTE_FAILED;
  portEXIT_CRITICAL(&s_routeMux);
  if (result != pdPASS) Serial.println("[route] could not start route task");
}

void aircraftRouteRequest(const char* icao, const char* callsign) {
  char normalizedIcao[8], normalizedCallsign[9];
  fr_copy(normalizedIcao, sizeof(normalizedIcao), icao);
  fr_copy(normalizedCallsign, sizeof(normalizedCallsign), callsign);
  for (char* p = normalizedCallsign; *p; ++p) {
    if (*p >= 'a' && *p <= 'z') *p -= 32;
  }

  portENTER_CRITICAL(&s_routeMux);
  if (strcmp(normalizedIcao, s_requestedIcao) ||
      strcmp(normalizedCallsign, s_requestedCallsign)) {
    fr_copy(s_requestedIcao, sizeof(s_requestedIcao), normalizedIcao);
    fr_copy(s_requestedCallsign, sizeof(s_requestedCallsign), normalizedCallsign);
    ++s_requestGeneration;
    s_state = normalizedIcao[0] ? (s_taskStarted ? AIRCRAFT_ROUTE_LOADING : AIRCRAFT_ROUTE_FAILED)
                                : AIRCRAFT_ROUTE_IDLE;
  }
  portEXIT_CRITICAL(&s_routeMux);
}

bool aircraftRoutePoll(AircraftRouteResult& result) {
  bool ready;
  portENTER_CRITICAL(&s_routeMux);
  ready = s_pendingReady;
  if (ready) {
    result = s_pending;
    memset(&s_pending, 0, sizeof(s_pending));
    s_pendingReady = false;
  }
  portEXIT_CRITICAL(&s_routeMux);
  return ready;
}

AircraftRouteState aircraftRouteState() {
  portENTER_CRITICAL(&s_routeMux);
  const AircraftRouteState state = s_state;
  portEXIT_CRITICAL(&s_routeMux);
  return state;
}
