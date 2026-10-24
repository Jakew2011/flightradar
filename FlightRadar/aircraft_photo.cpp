#include "aircraft_photo.h"
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
#include <stdlib.h>
#include <string.h>

static constexpr size_t PHOTO_MAX_BYTES = 384 * 1024;
static constexpr uint32_t PHOTO_DEBOUNCE_MS = 900;
static constexpr uint32_t PHOTO_MIN_REQUEST_GAP_MS = 3000;
static constexpr uint32_t PHOTO_TIMEOUT_MS = 8000;
static constexpr uint32_t PHOTO_CONNECT_TIMEOUT_MS = 6000;
static constexpr size_t PHOTO_MAX_CANDIDATES = 4;

static portMUX_TYPE s_photoMux = portMUX_INITIALIZER_UNLOCKED;
static char s_requestedIcao[8] = "";
static char s_requestedRegistration[13] = "";
static uint32_t s_requestGeneration = 0;
static AircraftPhotoResult s_pending = {};
static bool s_pendingReady = false;
static AircraftPhotoState s_state = AIRCRAFT_PHOTO_IDLE;
static bool s_taskStarted = false;

static void* photoAlloc(size_t bytes) {
  void* p = psramFound() ? ps_malloc(bytes) : nullptr;
  if (!p) p = malloc(bytes);
  return p;
}

static bool currentRequest(uint32_t generation, char* icao, size_t icaoSize,
                           char* registration, size_t registrationSize) {
  bool current;
  portENTER_CRITICAL(&s_photoMux);
  current = generation == s_requestGeneration && s_requestedIcao[0] != 0;
  if (current) {
    fr_copy(icao, icaoSize, s_requestedIcao);
    fr_copy(registration, registrationSize, s_requestedRegistration);
  }
  portEXIT_CRITICAL(&s_photoMux);
  return current;
}

static void publish(uint32_t generation, const char* icao, AircraftPhotoState state,
                    uint8_t* jpeg, size_t jpegLength) {
  uint8_t* replaced = nullptr;
  bool accepted = false;
  portENTER_CRITICAL(&s_photoMux);
  if (generation == s_requestGeneration && !strcmp(icao, s_requestedIcao)) {
    replaced = s_pending.jpeg;
    fr_copy(s_pending.icao, sizeof(s_pending.icao), icao);
    s_pending.jpeg = jpeg;
    s_pending.jpegLength = jpegLength;
    s_pending.state = state;
    s_pendingReady = true;
    s_state = state;
    accepted = true;
  }
  portEXIT_CRITICAL(&s_photoMux);
  free(replaced);
  if (!accepted) free(jpeg);
}

static bool secureGet(const char* url, HTTPClient& http, WiFiClientSecure& client,
                      const char* accept, bool includeContact) {
  if (strncmp(url, "https://", 8) != 0) return false;
  client.setInsecure();
  client.setHandshakeTimeout(10);
  http.setReuse(false);
  http.setConnectTimeout(PHOTO_CONNECT_TIMEOUT_MS);
  http.setTimeout(PHOTO_TIMEOUT_MS);
  if (!http.begin(client, url)) return false;
  http.addHeader("Accept", accept);
  if (includeContact) {
    http.addHeader("User-Agent", "FlightRadar-ESP32/" FW_VERSION " (+" PLANESPOTTERS_CONTACT ")");
  } else {
    http.addHeader("User-Agent", "FlightRadar-ESP32/" FW_VERSION);
  }
  return true;
}

static bool buildConvertedImageUrl(const char* source, char* out, size_t outSize) {
  static const char* const allowedPrefixes[] = {
    "https://t.plnspttrs.net/",
    "https://cdn.jetphotos.com/"
  };
  bool allowed = false;
  for (const char* prefix : allowedPrefixes) {
    if (!strncmp(source, prefix, strlen(prefix))) {
      allowed = true;
      break;
    }
  }
  if (!allowed) return false;
  static const char endpoint[] = "https://images.weserv.nl/?url=";
  size_t used = sizeof(endpoint) - 1;
  if (used >= outSize) return false;
  memcpy(out, endpoint, used);

  static const char hex[] = "0123456789ABCDEF";
  for (const unsigned char* p = (const unsigned char*)source + 8; *p; ++p) {
    const unsigned char c = *p;
    const bool unreserved = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                            (c >= '0' && c <= '9') || c == '-' || c == '_' ||
                            c == '.' || c == '~' || c == '/';
    const size_t needed = unreserved ? 1 : 3;
    if (used + needed >= outSize) return false;
    if (unreserved) {
      out[used++] = (char)c;
    } else {
      out[used++] = '%';
      out[used++] = hex[c >> 4];
      out[used++] = hex[c & 0x0f];
    }
  }
  const int suffixLength = snprintf(out + used, outSize - used,
                                    "&w=208&h=112&fit=cover&output=jpg&q=95");
  return suffixLength > 0 && (size_t)suffixLength < outSize - used;
}

static bool isBaselineJpeg(const uint8_t* data, size_t length) {
  if (!data || length < 4 || data[0] != 0xff || data[1] != 0xd8) return false;
  size_t pos = 2;
  while (pos < length) {
    if (data[pos++] != 0xff) return false;
    while (pos < length && data[pos] == 0xff) ++pos;
    if (pos >= length) return false;
    const uint8_t marker = data[pos++];
    if (marker == 0xda || marker == 0xd9) return false;
    if (marker == 0x01 || (marker >= 0xd0 && marker <= 0xd7)) continue;
    if (pos + 2 > length) return false;
    const size_t segmentLength = ((size_t)data[pos] << 8) | data[pos + 1];
    if (segmentLength < 2 || segmentLength > length - pos) return false;
    if (marker == 0xc0) return true;
    if (marker == 0xc2) return false;
    pos += segmentLength;
  }
  return false;
}

static bool downloadJpeg(const char* url, const char* label, bool includeContact,
                         uint8_t*& jpeg, size_t& jpegLength) {
  jpeg = nullptr;
  jpegLength = 0;
  WiFiClientSecure client;
  HTTPClient http;
  if (!secureGet(url, http, client, "image/jpeg", includeContact)) {
    Serial.printf("[photo] could not open %s URL\n", label);
    return false;
  }
  const int code = http.GET();
  if (code != HTTP_CODE_OK) {
    char response[121];
    size_t responseLength = 0;
    WiFiClient& stream = http.getStream();
    while (responseLength < sizeof(response) - 1 && stream.available() > 0) {
      const int c = stream.read();
      if (c < 0) break;
      response[responseLength++] = (char)c;
    }
    response[responseLength] = 0;
    Serial.printf("[photo] %s download failed: HTTP %d%s%s\n", label, code,
                  responseLength ? " - " : "", response);
    http.end();
    return false;
  }

  const int imageSize = http.getSize();
  if (imageSize == 0 || imageSize > (int)PHOTO_MAX_BYTES) {
    Serial.printf("[photo] %s size is invalid or over %u bytes (%d)\n",
                  label, (unsigned)PHOTO_MAX_BYTES, imageSize);
    http.end();
    return false;
  }

  const size_t capacity = imageSize > 0 ? (size_t)imageSize : PHOTO_MAX_BYTES;
  uint8_t* buffer = (uint8_t*)photoAlloc(capacity);
  if (!buffer) {
    Serial.printf("[photo] not enough memory for %s buffer (%u bytes)\n",
                  label, (unsigned)capacity);
    http.end();
    return false;
  }

  WiFiClient& stream = http.getStream();
  const uint32_t deadline = millis() + PHOTO_TIMEOUT_MS * 4;
  while (jpegLength < capacity) {
    const int available = stream.available();
    if (available > 0) {
      const size_t want = (size_t)available < capacity - jpegLength
                            ? (size_t)available : capacity - jpegLength;
      jpegLength += stream.readBytes(buffer + jpegLength, want);
      if (imageSize < 0 && jpegLength >= 2 &&
          buffer[jpegLength - 2] == 0xff && buffer[jpegLength - 1] == 0xd9) break;
    } else if (!http.connected()) {
      break;
    }
    if ((int32_t)(millis() - deadline) > 0) break;
    delay(2);
  }
  const bool overflow = imageSize < 0 && jpegLength == capacity &&
                       (stream.available() > 0 || http.connected());
  http.end();

  if (overflow || (imageSize > 0 && jpegLength != capacity) || jpegLength < 4 ||
      buffer[0] != 0xff || buffer[1] != 0xd8 ||
      buffer[jpegLength - 2] != 0xff || buffer[jpegLength - 1] != 0xd9) {
    Serial.printf("[photo] invalid or incomplete %s JPEG (%u bytes)\n",
                  label, (unsigned)jpegLength);
    free(buffer);
    return false;
  }
  if (!isBaselineJpeg(buffer, jpegLength)) {
    Serial.printf("[photo] %s is not a baseline JPEG; refusing low-resolution progressive preview\n",
                  label);
    free(buffer);
    return false;
  }
  jpeg = buffer;
  return true;
}

static bool downloadPhotoCandidate(const char* imageUrl, bool includeContact,
                                   uint8_t*& jpeg, size_t& jpegLength) {
  char convertedUrl[1280];
  if (buildConvertedImageUrl(imageUrl, convertedUrl, sizeof(convertedUrl)) &&
      downloadJpeg(convertedUrl, "converted photo", false, jpeg, jpegLength)) {
    return true;
  }
  return downloadJpeg(imageUrl, "original photo", includeContact, jpeg, jpegLength);
}

static AircraftPhotoState lookupPlanespotters(const char* icao, uint8_t*& jpeg,
                                               size_t& jpegLength) {
  jpeg = nullptr;
  jpegLength = 0;
  char url[96];
  snprintf(url, sizeof(url), "https://api.planespotters.net/pub/photos/hex/%s", icao);

  WiFiClientSecure apiClient;
  HTTPClient api;
  if (!secureGet(url, api, apiClient, "application/json", true)) {
    Serial.println("[photo] could not open Planespotters lookup URL");
    return AIRCRAFT_PHOTO_FAILED;
  }
  const int apiCode = api.GET();
  if (apiCode != HTTP_CODE_OK) {
    char response[161];
    size_t responseLength = 0;
    WiFiClient& stream = api.getStream();
    while (responseLength < sizeof(response) - 1 && stream.available() > 0) {
      const int c = stream.read();
      if (c < 0) break;
      response[responseLength++] = (char)c;
    }
    response[responseLength] = 0;
    Serial.printf("[photo] lookup failed: HTTP %d%s%s\n", apiCode,
                  responseLength ? " - " : "", response);
    api.end();
    return apiCode == HTTP_CODE_NOT_FOUND ? AIRCRAFT_PHOTO_NOT_FOUND : AIRCRAFT_PHOTO_FAILED;
  }
  const int jsonLength = api.getSize();
  if (jsonLength > 64 * 1024) {
    Serial.printf("[photo] lookup response is too large (%d bytes)\n", jsonLength);
    api.end();
    return AIRCRAFT_PHOTO_FAILED;
  }

  String payload = api.getString();
  api.end();
  if (payload.length() > 64 * 1024) {
    Serial.printf("[photo] lookup response is too large (%u bytes)\n", (unsigned)payload.length());
    return AIRCRAFT_PHOTO_FAILED;
  }
  JsonDocument doc;
  const DeserializationError jsonError = deserializeJson(doc, payload);
  if (jsonError) {
    Serial.printf("[photo] invalid lookup JSON: %s\n", jsonError.c_str());
    return AIRCRAFT_PHOTO_FAILED;
  }
  JsonArrayConst photos = doc["photos"].as<JsonArrayConst>();
  if (photos.isNull() || photos.size() == 0) return AIRCRAFT_PHOTO_NOT_FOUND;

  size_t candidatesTried = 0;
  for (JsonObjectConst photo : photos) {
    const char* sources[] = {
      photo["thumbnail_large"]["src"] | "",
      photo["thumbnail"]["src"] | ""
    };
    char previousUrl[384] = "";
    for (const char* source : sources) {
      if (!source[0] || strncmp(source, "https://", 8) != 0 ||
          !strcmp(source, previousUrl)) continue;
      if (candidatesTried >= PHOTO_MAX_CANDIDATES) break;
      char imageUrl[384];
      const int urlLength = snprintf(imageUrl, sizeof(imageUrl), "%s", source);
      if (urlLength < 0 || (size_t)urlLength >= sizeof(imageUrl)) {
        Serial.println("[photo] thumbnail URL is too long");
        continue;
      }
      fr_copy(previousUrl, sizeof(previousUrl), imageUrl);
      ++candidatesTried;
      if (downloadPhotoCandidate(imageUrl, true, jpeg, jpegLength)) {
        return AIRCRAFT_PHOTO_READY;
      }
    }
    if (candidatesTried >= PHOTO_MAX_CANDIDATES) break;
  }
  if (!candidatesTried) {
    Serial.println("[photo] lookup contained no usable HTTPS thumbnails");
  } else {
    Serial.printf("[photo] no usable image after trying %u thumbnails\n",
                  (unsigned)candidatesTried);
  }
  return AIRCRAFT_PHOTO_FAILED;
}

static AircraftPhotoState lookupJetApi(const char* registration, uint8_t*& jpeg,
                                       size_t& jpegLength) {
  jpeg = nullptr;
  jpegLength = 0;
  if (!registration || !registration[0]) return AIRCRAFT_PHOTO_NOT_FOUND;
  for (const unsigned char* p = (const unsigned char*)registration; *p; ++p) {
    if (!((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
          (*p >= '0' && *p <= '9') || *p == '-')) {
      Serial.println("[photo] JetAPI skipped: invalid aircraft registration");
      return AIRCRAFT_PHOTO_FAILED;
    }
  }

  char url[192];
  const int urlLength = snprintf(url, sizeof(url),
                                JETAPI_BASE_URL "/api?reg=%s&photos=3&flights=0&only_jp=true",
                                registration);
  if (urlLength < 0 || (size_t)urlLength >= sizeof(url)) {
    Serial.println("[photo] JetAPI lookup URL is too long");
    return AIRCRAFT_PHOTO_FAILED;
  }

  WiFiClientSecure apiClient;
  HTTPClient api;
  if (!secureGet(url, api, apiClient, "application/json", false)) {
    Serial.println("[photo] could not open JetAPI lookup URL");
    return AIRCRAFT_PHOTO_FAILED;
  }
  const int apiCode = api.GET();
  if (apiCode != HTTP_CODE_OK) {
    Serial.printf("[photo] JetAPI lookup failed: HTTP %d\n", apiCode);
    api.end();
    return AIRCRAFT_PHOTO_FAILED;
  }
  const int jsonLength = api.getSize();
  if (jsonLength > 64 * 1024) {
    Serial.printf("[photo] JetAPI response is too large (%d bytes)\n", jsonLength);
    api.end();
    return AIRCRAFT_PHOTO_FAILED;
  }
  String payload = api.getString();
  api.end();
  if (payload.length() > 64 * 1024) {
    Serial.printf("[photo] JetAPI response is too large (%u bytes)\n",
                  (unsigned)payload.length());
    return AIRCRAFT_PHOTO_FAILED;
  }

  JsonDocument doc;
  const DeserializationError jsonError = deserializeJson(doc, payload);
  if (jsonError) {
    Serial.printf("[photo] invalid JetAPI JSON: %s\n", jsonError.c_str());
    return AIRCRAFT_PHOTO_FAILED;
  }
  JsonArrayConst images = doc["Images"].as<JsonArrayConst>();
  if (images.isNull() || images.size() == 0) return AIRCRAFT_PHOTO_NOT_FOUND;

  size_t candidatesTried = 0;
  for (JsonObjectConst image : images) {
    const char* sources[] = {image["Image"] | "", image["Thumbnail"] | ""};
    for (const char* source : sources) {
      if (!source[0] || strncmp(source, "https://cdn.jetphotos.com/", 26) != 0) continue;
      if (candidatesTried >= PHOTO_MAX_CANDIDATES) break;
      ++candidatesTried;
      if (downloadPhotoCandidate(source, false, jpeg, jpegLength)) {
        Serial.printf("[photo] loaded JetAPI photo for %s\n", registration);
        return AIRCRAFT_PHOTO_READY;
      }
    }
    if (candidatesTried >= PHOTO_MAX_CANDIDATES) break;
  }
  Serial.printf("[photo] no usable JetAPI image after trying %u thumbnails\n",
                (unsigned)candidatesTried);
  return candidatesTried ? AIRCRAFT_PHOTO_FAILED : AIRCRAFT_PHOTO_NOT_FOUND;
}

static AircraftPhotoState lookupPhoto(const char* icao, const char* registration,
                                      uint8_t*& jpeg, size_t& jpegLength) {
  const AircraftPhotoState jetApi = lookupJetApi(registration, jpeg, jpegLength);
  if (jetApi == AIRCRAFT_PHOTO_READY) return jetApi;

  if (!registration || !registration[0]) {
    Serial.println("[photo] JetAPI skipped: aircraft registration unavailable");
  }

  Serial.println("[photo] trying Planespotters fallback");
  const AircraftPhotoState planespotters =
    lookupPlanespotters(icao, jpeg, jpegLength);
  if (planespotters == AIRCRAFT_PHOTO_READY) return planespotters;
  if (jetApi == AIRCRAFT_PHOTO_NOT_FOUND &&
      planespotters == AIRCRAFT_PHOTO_NOT_FOUND) {
    return AIRCRAFT_PHOTO_NOT_FOUND;
  }
  return AIRCRAFT_PHOTO_FAILED;
}

static void photoTask(void*) {
  uint32_t handledGeneration = 0;
  uint32_t lastRequestMs = 0;
  for (;;) {
    char icao[8] = "";
    char registration[13] = "";
    uint32_t generation;
    portENTER_CRITICAL(&s_photoMux);
    generation = s_requestGeneration;
    fr_copy(icao, sizeof(icao), s_requestedIcao);
    fr_copy(registration, sizeof(registration), s_requestedRegistration);
    portEXIT_CRITICAL(&s_photoMux);

    if (!icao[0] || generation == handledGeneration) {
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }

    vTaskDelay(pdMS_TO_TICKS(PHOTO_DEBOUNCE_MS));
    char latestIcao[8] = "";
    char latestRegistration[13] = "";
    if (!currentRequest(generation, latestIcao, sizeof(latestIcao),
                        latestRegistration, sizeof(latestRegistration))) continue;
    fr_copy(icao, sizeof(icao), latestIcao);
    fr_copy(registration, sizeof(registration), latestRegistration);
    if (!wifiIsConnected()) {
      vTaskDelay(pdMS_TO_TICKS(250));
      continue;
    }
    const uint32_t sinceLast = millis() - lastRequestMs;
    if (lastRequestMs && sinceLast < PHOTO_MIN_REQUEST_GAP_MS) {
      vTaskDelay(pdMS_TO_TICKS(PHOTO_MIN_REQUEST_GAP_MS - sinceLast));
      continue;
    }

    handledGeneration = generation;
    lastRequestMs = millis();
    uint8_t* jpeg = nullptr;
    size_t jpegLength = 0;
    AircraftPhotoState result = lookupPhoto(icao, registration, jpeg, jpegLength);
    if (result == AIRCRAFT_PHOTO_FAILED) {
      vTaskDelay(pdMS_TO_TICKS(5000));
      char retryIcao[8] = "";
      char retryRegistration[13] = "";
      if (currentRequest(generation, retryIcao, sizeof(retryIcao),
                         retryRegistration, sizeof(retryRegistration)) &&
          !strcmp(retryIcao, icao) && wifiIsConnected()) {
        Serial.printf("[photo] retrying lookup for %s after a transient failure\n", icao);
        result = lookupPhoto(icao, retryRegistration, jpeg, jpegLength);
      }
    }
    publish(generation, icao, result, jpeg, jpegLength);
    if (result == AIRCRAFT_PHOTO_READY) {
      Serial.printf("[photo] loaded photo for %s (%u bytes)\n", icao, (unsigned)jpegLength);
    }
  }
}

void aircraftPhotoInit() {
  // 16 KB here used to be enough for a single lookup, but lookupPhoto() can
  // nest two full HTTPS round trips deep at once (e.g. lookupJetApi()'s own
  // WiFiClientSecure/HTTPClient/JsonDocument are all still live on the stack
  // while it calls downloadPhotoCandidate() -> downloadJpeg(), which opens a
  // SECOND WiFiClientSecure/HTTPClient of its own, plus a 1280-byte URL
  // buffer, all at the same time) - exactly the kind of deep-nested-buffers
  // stack overflow already diagnosed for the main loop task (see the
  // getArduinoLoopTaskStackSize() comment in the .ino), just in this task's
  // own stack instead. Scrolling quickly through many different aircraft
  // drives a steady stream of these lookups back-to-back, each one a chance
  // to hit it - which is consistent with crashes turning up specifically
  // while scrolling through the aircraft list. Sized generously here rather
  // than trimmed to a guess, since corrupting memory silently is far worse
  // than a few KB of unused stack on a board with plenty of RAM.
  const BaseType_t result = xTaskCreatePinnedToCore(photoTask, "aircraft_photo", 32768,
                                                    nullptr, 1, nullptr, 0);
  portENTER_CRITICAL(&s_photoMux);
  s_taskStarted = result == pdPASS;
  if (!s_taskStarted) s_state = AIRCRAFT_PHOTO_FAILED;
  portEXIT_CRITICAL(&s_photoMux);
  if (result != pdPASS) Serial.println("[photo] could not start photo task");
}

void aircraftPhotoRequest(const char* icao, const char* registration) {
  char normalized[8];
  char normalizedRegistration[13];
  uint8_t* discardedJpeg = nullptr;
  fr_copy(normalized, sizeof(normalized), icao);
  fr_copy(normalizedRegistration, sizeof(normalizedRegistration), registration);
  for (char* p = normalized; *p; ++p) {
    if (*p >= 'a' && *p <= 'z') *p -= 32;
  }
  for (char* p = normalizedRegistration; *p; ++p) {
    if (*p >= 'a' && *p <= 'z') *p -= 32;
  }
  portENTER_CRITICAL(&s_photoMux);
  if (strcmp(normalized, s_requestedIcao) ||
      strcmp(normalizedRegistration, s_requestedRegistration)) {
    discardedJpeg = s_pending.jpeg;
    memset(&s_pending, 0, sizeof(s_pending));
    s_pendingReady = false;
    fr_copy(s_requestedIcao, sizeof(s_requestedIcao), normalized);
    fr_copy(s_requestedRegistration, sizeof(s_requestedRegistration),
            normalizedRegistration);
    ++s_requestGeneration;
    s_state = normalized[0] ? (s_taskStarted ? AIRCRAFT_PHOTO_LOADING : AIRCRAFT_PHOTO_FAILED)
                            : AIRCRAFT_PHOTO_IDLE;
  }
  portEXIT_CRITICAL(&s_photoMux);
  free(discardedJpeg);
}

bool aircraftPhotoPoll(AircraftPhotoResult& result) {
  bool ready;
  portENTER_CRITICAL(&s_photoMux);
  ready = s_pendingReady;
  if (ready) {
    result = s_pending;
    memset(&s_pending, 0, sizeof(s_pending));
    s_pendingReady = false;
  }
  portEXIT_CRITICAL(&s_photoMux);
  return ready;
}

AircraftPhotoState aircraftPhotoState() {
  portENTER_CRITICAL(&s_photoMux);
  const AircraftPhotoState state = s_state;
  portEXIT_CRITICAL(&s_photoMux);
  return state;
}
