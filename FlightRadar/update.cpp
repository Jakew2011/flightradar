#include "update.h"
#include "config.h"
#include "util.h"
#include "wifi_manager.h"
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Update.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <string.h>
#include <stdlib.h>

static constexpr uint32_t NOTIFY_CHECK = 1u;
static constexpr uint32_t NOTIFY_INSTALL = 2u;

static portMUX_TYPE   s_mux = portMUX_INITIALIZER_UNLOCKED;
static FwUpdateState  s_state = FW_UPDATE_IDLE;
static char           s_availableVersion[16] = "";
static char           s_lastError[40] = "";
static volatile uint8_t s_progress = 0;
static TaskHandle_t   s_task = nullptr;
static volatile bool  s_dismissed = false;

bool updateIsEnabled() {
  return FW_UPDATE_VERSION_URL[0] != 0 && FW_UPDATE_BIN_URL[0] != 0;
}

static void setState(FwUpdateState st) {
  portENTER_CRITICAL(&s_mux);
  s_state = st;
  portEXIT_CRITICAL(&s_mux);
}

static void setError(const char* msg) {
  portENTER_CRITICAL(&s_mux);
  s_state = FW_UPDATE_ERROR;
  fr_copy(s_lastError, sizeof(s_lastError), msg);
  portEXIT_CRITICAL(&s_mux);
  Serial.printf("[update] %s\n", msg);
}

FwUpdateState updateState() {
  portENTER_CRITICAL(&s_mux);
  FwUpdateState st = s_state;
  portEXIT_CRITICAL(&s_mux);
  return st;
}

void updateAvailableVersion(char* out, size_t n) {
  portENTER_CRITICAL(&s_mux);
  fr_copy(out, n, s_availableVersion);
  portEXIT_CRITICAL(&s_mux);
}

uint8_t updateProgressPercent() { return s_progress; }

void updateLastError(char* out, size_t n) {
  portENTER_CRITICAL(&s_mux);
  fr_copy(out, n, s_lastError);
  portEXIT_CRITICAL(&s_mux);
}

void updateDismiss() {
  s_dismissed = true;
  if (updateState() == FW_UPDATE_AVAILABLE) setState(FW_UPDATE_IDLE);
}

void updateCheckNow() {
  if (!updateIsEnabled() || !s_task) return;
  xTaskNotify(s_task, NOTIFY_CHECK, eSetBits);
}

void updateBeginInstall() {
  if (!updateIsEnabled() || !s_task) return;
  xTaskNotify(s_task, NOTIFY_INSTALL, eSetBits);
}

// ---------------------------------------------------------------------------
//  Helpers
// ---------------------------------------------------------------------------
static void trim(char* s) {
  size_t len = strlen(s);
  while (len && (s[len - 1] == '\n' || s[len - 1] == '\r' || s[len - 1] == ' ' || s[len - 1] == '\t'))
    s[--len] = 0;
  size_t start = 0;
  while (s[start] == ' ' || s[start] == '\t') start++;
  if (start) memmove(s, s + start, strlen(s + start) + 1);
}

static bool httpBegin(HTTPClient& http, WiFiClientSecure& secure, WiFiClient& plain,
                      WiFiClient*& outClient, const char* url) {
  const bool https = !strncmp(url, "https", 5);
  outClient = https ? (WiFiClient*)&secure : &plain;
  if (https) secure.setInsecure();     // encrypted but not certificate-verified, same as the rest of the firmware
  http.setConnectTimeout(FW_UPDATE_HTTP_TIMEOUT_MS);
  http.setTimeout(FW_UPDATE_HTTP_TIMEOUT_MS);
  return http.begin(*outClient, url);
}

// Fetches FW_UPDATE_VERSION_URL and compares it (trimmed) against FW_VERSION.
// This is a plain string comparison, not a semantic-version comparison - any
// difference, higher or lower, is reported as "available" so a VERSION file
// typo is easy to notice rather than silently ignored.
static void doCheck() {
  setState(FW_UPDATE_CHECKING);
  WiFiClientSecure secure;
  WiFiClient plain;
  WiFiClient* client = nullptr;
  HTTPClient http;
  if (!httpBegin(http, secure, plain, client, FW_UPDATE_VERSION_URL)) {
    setError("VERSION CHECK: BAD URL");
    return;
  }
  const int code = http.GET();
  if (code != HTTP_CODE_OK) {
    char msg[40];
    snprintf(msg, sizeof(msg), "VERSION CHECK: HTTP %d", code);
    http.end();
    setError(msg);
    return;
  }
  char buf[32] = "";
  const String body = http.getString();
  http.end();
  fr_copy(buf, sizeof(buf), body.c_str());
  trim(buf);
  if (!buf[0]) { setError("VERSION CHECK: EMPTY REPLY"); return; }

  if (!strcmp(buf, FW_VERSION)) {
    Serial.printf("[update] up to date (%s)\n", FW_VERSION);
    setState(FW_UPDATE_IDLE);
    return;
  }
  Serial.printf("[update] version on server (%s) differs from installed (%s)\n", buf, FW_VERSION);
  portENTER_CRITICAL(&s_mux);
  fr_copy(s_availableVersion, sizeof(s_availableVersion), buf);
  s_state = FW_UPDATE_AVAILABLE;
  portEXIT_CRITICAL(&s_mux);
}

// Downloads FW_UPDATE_BIN_URL straight into the inactive OTA partition and,
// on success, reboots into it. Every failure path leaves the currently
// running firmware untouched - Update.begin()/Update.end() only commit the
// new image if the full, correctly-sized download finished cleanly.
static void doInstall() {
  setState(FW_UPDATE_DOWNLOADING);
  s_progress = 0;
  WiFiClientSecure secure;
  WiFiClient plain;
  WiFiClient* client = nullptr;
  HTTPClient http;
  if (!httpBegin(http, secure, plain, client, FW_UPDATE_BIN_URL)) {
    setError("INSTALL: BAD URL");
    return;
  }
  const int code = http.GET();
  if (code != HTTP_CODE_OK) {
    char msg[40];
    snprintf(msg, sizeof(msg), "INSTALL: HTTP %d", code);
    http.end();
    setError(msg);
    return;
  }
  const int len = http.getSize();
  if (len <= 0) {
    http.end();
    setError("INSTALL: UNKNOWN SIZE (chunked host?)");
    return;
  }
  if (!Update.begin((size_t)len, U_FLASH)) {
    http.end();
    char msg[40];
    snprintf(msg, sizeof(msg), "INSTALL: %s", Update.errorString());
    setError(msg);
    return;
  }

  WiFiClient& stream = http.getStream();
  uint8_t buf[1024];
  int got = 0;
  uint32_t lastDataMs = millis();
  const uint32_t stallLimitMs = FW_UPDATE_HTTP_TIMEOUT_MS * 2;
  while (got < len) {
    const int avail = stream.available();
    if (avail > 0) {
      const size_t want = (size_t)((avail < (int)sizeof(buf)) ? avail : (int)sizeof(buf));
      const size_t n = stream.readBytes(buf, want);
      if (n > 0) {
        if (Update.write(buf, n) != n) {
          http.end();
          Update.abort();
          char msg[40];
          snprintf(msg, sizeof(msg), "INSTALL: WRITE FAILED (%s)", Update.errorString());
          setError(msg);
          return;
        }
        got += (int)n;
        s_progress = (uint8_t)((got * 100) / len);
        lastDataMs = millis();
      }
    } else if (!http.connected()) {
      break;                                            // connection closed early
    }
    if (millis() - lastDataMs > stallLimitMs) break;      // stall guard
    delay(2);
  }
  http.end();

  if (got != len) {
    Update.abort();
    char msg[40];
    snprintf(msg, sizeof(msg), "INSTALL: INCOMPLETE (%d of %d bytes)", got, len);
    setError(msg);
    return;
  }
  if (!Update.end(true)) {
    char msg[40];
    snprintf(msg, sizeof(msg), "INSTALL: %s", Update.errorString());
    setError(msg);
    return;
  }
  Serial.println("[update] flashed OK, rebooting into new firmware");
  delay(200);
  ESP.restart();
}

// ---------------------------------------------------------------------------
//  Task
// ---------------------------------------------------------------------------
static void updateTask(void*) {
  uint32_t lastCheck = 0;
  bool first = true;
  for (;;) {
    uint32_t requests = 0;
    xTaskNotifyWait(0, UINT32_MAX, &requests, pdMS_TO_TICKS(30000));

    if (requests & NOTIFY_INSTALL) {
      doInstall();                     // reboots on success; falls through to ERROR on failure
      continue;
    }

    const uint32_t now = millis();
    const bool due = first || (now - lastCheck) >= (uint32_t)FW_UPDATE_CHECK_INTERVAL_MS;
    if (!wifiIsConnected()) continue;
    if (!((requests & NOTIFY_CHECK) || due)) continue;

    first = false;
    lastCheck = now;
    s_dismissed = false;
    doCheck();
  }
}

void updateInit() {
  if (!updateIsEnabled()) {
    Serial.println("[update] FW_UPDATE_VERSION_URL/BIN_URL not set - update checking disabled");
    return;
  }
  if (xTaskCreatePinnedToCore(updateTask, "fwupdate", 8192, nullptr, 1, &s_task, 0) != pdPASS) {
    s_task = nullptr;
    Serial.println("[update] could not create update task");
  }
}
