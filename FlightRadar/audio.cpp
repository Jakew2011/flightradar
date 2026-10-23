#include "audio.h"
#include "config.h"
#include "audio_data.h"
#include "settings.h"
#include <Arduino.h>
#include <string.h>
#include <stdlib.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <driver/i2s.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include "wifi_manager.h"

// Set to 0 once the speaker is confirmed working to quiet this down again -
// with it on, every I2S write and state transition is logged, which is a lot
// of Serial traffic during a full clip but is exactly what's needed to see
// where playback actually stops.
#define AUDIO_DEBUG 1

#if AUDIO_DEBUG
  #define AUDIO_LOGV(...) Serial.printf(__VA_ARGS__)
#else
  #define AUDIO_LOGV(...)
#endif

struct WavInfo {
  uint32_t sampleRate = 0;
  uint16_t bitsPerSample = 0;
  uint16_t channels = 0;
  const uint8_t* data = nullptr;
  uint32_t dataLen = 0;
};

// Reads just enough of a WAV container (RIFF/WAVE, "fmt " and "data" chunks)
// to play it - ignores any other chunks (LIST, cue, etc.) a recording app or
// web host might have added. Requires uncompressed 16-bit PCM. On failure,
// "why" (if given) is filled with the specific reason - a silent built-in
// clip would otherwise be a very confusing failure mode to track down.
static bool parseWav(const uint8_t* buf, uint32_t len, WavInfo& info, char* why = nullptr, size_t whyN = 0) {
  auto fail = [&](const char* reason) {
    if (why && whyN) snprintf(why, whyN, "%s", reason);
    return false;
  };
  if (len < 44) return fail("too short to be a WAV file");
  if (memcmp(buf, "RIFF", 4)) return fail("missing RIFF header");
  if (memcmp(buf + 8, "WAVE", 4)) return fail("missing WAVE header");
  uint32_t pos = 12;
  bool haveFmt = false, haveData = false;
  while (pos + 8 <= len) {
    char id[5] = {0};
    memcpy(id, buf + pos, 4);
    uint32_t sz;
    memcpy(&sz, buf + pos + 4, 4);
    const uint32_t body = pos + 8;
    if (body + sz > len) break;                      // malformed / truncated
    if (!memcmp(id, "fmt ", 4) && sz >= 16) {
      uint16_t audioFormat, channels, bits;
      uint32_t rate;
      memcpy(&audioFormat, buf + body, 2);
      memcpy(&channels, buf + body + 2, 2);
      memcpy(&rate, buf + body + 4, 4);
      memcpy(&bits, buf + body + 14, 2);
      if (audioFormat != 1 /* PCM */) {
        char msg[48];
        snprintf(msg, sizeof(msg), "not PCM (audioFormat=%u)", (unsigned)audioFormat);
        return fail(msg);
      }
      info.channels = channels;
      info.sampleRate = rate;
      info.bitsPerSample = bits;
      haveFmt = true;
    } else if (!memcmp(id, "data", 4)) {
      info.data = buf + body;
      info.dataLen = sz;
      haveData = true;
    }
    pos = body + sz + (sz & 1);                       // chunks are word-aligned
  }
  if (!haveFmt) return fail("no 'fmt ' chunk found");
  if (!haveData) return fail("no 'data' chunk found");
  if (info.bitsPerSample != 16) {
    char msg[40];
    snprintf(msg, sizeof(msg), "not 16-bit (got %u-bit)", (unsigned)info.bitsPerSample);
    return fail(msg);
  }
  if (info.channels < 1 || info.channels > 2) {
    char msg[40];
    snprintf(msg, sizeof(msg), "bad channel count (%u)", (unsigned)info.channels);
    return fail(msg);
  }
  if (info.sampleRate < 4000 || info.sampleRate > 48000) {
    char msg[48];
    snprintf(msg, sizeof(msg), "sample rate out of range (%lu Hz)", (unsigned long)info.sampleRate);
    return fail(msg);
  }
  return true;
}

static void* allocLarge(size_t bytes) {
  void* p = psramFound() ? ps_malloc(bytes) : nullptr;
  if (!p) p = malloc(bytes);
  return p;
}

static TaskHandle_t s_task = nullptr;
static WavInfo       s_wav;
static uint8_t*      s_fetchedBuf = nullptr;   // non-null if s_wav points into a downloaded buffer
static constexpr uint32_t AUDIO_NOTIFY_CLIP = 1u;
static constexpr uint32_t AUDIO_NOTIFY_ALERT = 2u;
static constexpr uint32_t AUDIO_NOTIFY_TEST = 4u;
static constexpr uint32_t AUDIO_NOTIFY_ALERT_7700 = 8u;
static constexpr uint32_t AUDIO_NOTIFY_POLICE = 16u;
static constexpr uint32_t AUDIO_NOTIFY_COASTGUARD = 32u;
static constexpr uint32_t AUDIO_NOTIFY_AIRAMB = 64u;

static uint32_t s_i2sWriteCalls = 0;
static uint32_t s_i2sBytesWritten = 0;
static volatile bool s_clipBusy = false;   // true for the duration of an actual clip playback

bool audioClipBusy() { return s_clipBusy; }

static bool writeI2s(const int16_t* samples, size_t sampleCount) {
  const uint8_t* bytes = (const uint8_t*)samples;
  size_t remaining = sampleCount * sizeof(int16_t);
  const size_t total = remaining;
  while (remaining) {
    size_t written = 0;
    const esp_err_t result = i2s_write(I2S_NUM_0, bytes, remaining, &written, portMAX_DELAY);
    s_i2sWriteCalls++;
    if (result != ESP_OK || written == 0) {
      Serial.printf("[audio] I2S write FAILED: %s, wrote %u of %u bytes requested (call #%lu, "
                    "%lu total bytes written so far)\n",
                    esp_err_to_name(result), (unsigned)written, (unsigned)remaining,
                    (unsigned long)s_i2sWriteCalls, (unsigned long)s_i2sBytesWritten);
      return false;
    }
    s_i2sBytesWritten += (uint32_t)written;
    bytes += written;
    remaining -= written;
  }
  AUDIO_LOGV("[audio][v] writeI2s: wrote %u bytes OK (call #%lu, running total %lu bytes)\n",
             (unsigned)total, (unsigned long)s_i2sWriteCalls, (unsigned long)s_i2sBytesWritten);
  return true;
}

static bool playTone(uint16_t frequency, uint16_t durationMs, int16_t amplitude) {
  Serial.printf("[audio] playTone: %u Hz, %u ms, amplitude %d - entering\n",
                (unsigned)frequency, (unsigned)durationMs, (int)amplitude);
  if (frequency == 0) {
    Serial.println("[audio] playTone: ABORT - frequency is 0 (would divide by zero)");
    return false;
  }
  const esp_err_t rateResult = i2s_set_sample_rates(I2S_NUM_0, 16000);
  if (rateResult != ESP_OK) {
    Serial.printf("[audio] playTone: ABORT - i2s_set_sample_rates failed: %s\n", esp_err_to_name(rateResult));
    return false;
  }
  AUDIO_LOGV("[audio][v] playTone: sample rate set to 16000 Hz OK\n");
  const esp_err_t zeroResult = i2s_zero_dma_buffer(I2S_NUM_0);
  AUDIO_LOGV("[audio][v] playTone: i2s_zero_dma_buffer -> %s\n", esp_err_to_name(zeroResult));
  int16_t tone[512];
  const uint32_t totalFrames = (16000UL * durationMs) / 1000UL;
  const uint32_t halfPeriod = 8000UL / frequency;
  const uint32_t callsBefore = s_i2sWriteCalls;
  Serial.printf("[audio] playTone: writing %lu frames in chunks of <=256 (halfPeriod=%lu samples)\n",
                (unsigned long)totalFrames, (unsigned long)halfPeriod);
  for (uint32_t frame = 0; frame < totalFrames; frame += 256) {
    const uint32_t n = (totalFrames - frame < 256) ? (totalFrames - frame) : 256;
    for (uint32_t j = 0; j < n; ++j) {
      const int16_t sample = (((frame + j) / halfPeriod) & 1) ? -amplitude : amplitude;
      tone[j * 2] = sample;
      tone[j * 2 + 1] = sample;
    }
    if (!writeI2s(tone, n * 2)) {
      Serial.printf("[audio] playTone: ABORTED after %lu/%lu frames\n",
                    (unsigned long)frame, (unsigned long)totalFrames);
      return false;
    }
  }
  Serial.printf("[audio] playTone: DONE - %lu frames written across %lu i2s_write() calls\n",
                (unsigned long)totalFrames, (unsigned long)(s_i2sWriteCalls - callsBefore));
  return true;
}

static bool playEmergencyTone() {
  if (!playTone(1000, 120, 6000)) return false;
  vTaskDelay(pdMS_TO_TICKS(120));
  return playTone(1000, 120, 6000);
}

// Squawk 7700 specifically: loud, hard to miss, and CLEARLY intermittent -
// 10 short 400 ms beeps with 400 ms of silence between them (7.6 s total,
// comfortably under the 10 s cap), not one long unbroken tone. Deliberately
// not interruptible mid-pattern - this *is* the emergency alert. It is only
// ever started once per emergency (see the key/squawk-change gate in
// ui.cpp's uiUpdate()) - once this pattern finishes, it is not replayed
// again for that same aircraft/squawk, even if it keeps squawking 7700 for
// the rest of the time it's in range.
static bool playEmergency7700Pattern() {
  for (uint8_t beep = 0; beep < 10; ++beep) {
    if (!playTone(1000, 400, 20000)) return false;
    if (beep < 9) vTaskDelay(pdMS_TO_TICKS(400));
  }
  return true;
}

// Three short, distinct tones for police / coastguard / air ambulance - each
// deliberately different in shape from the above and from each other so
// they're recognisable by ear alone.
static bool playPoliceTone() {
  // Two-tone "yelp" alternation, 3 cycles - evokes a UK police siren without
  // trying to be a literal recording of one.
  for (uint8_t i = 0; i < 3; ++i) {
    if (!playTone(900, 180, 14000)) return false;
    if (!playTone(1300, 180, 14000)) return false;
  }
  return true;
}

static bool playCoastguardTone() {
  // Three descending tones, once - a falling "all clear"-style sweep.
  if (!playTone(1200, 220, 14000)) return false;
  if (!playTone(900, 220, 14000)) return false;
  if (!playTone(650, 260, 14000)) return false;
  return true;
}

static bool playAirAmbulanceTone() {
  // Two pairs of quick double-beeps, like a medical monitor.
  for (uint8_t i = 0; i < 2; ++i) {
    if (!playTone(1000, 150, 14000)) return false;
    vTaskDelay(pdMS_TO_TICKS(100));
    if (!playTone(1000, 150, 14000)) return false;
    if (i == 0) vTaskDelay(pdMS_TO_TICKS(300));
  }
  return true;
}

// Downloads MILITARY_WAV_URL into PSRAM/heap and, on success, points s_wav at
// it. Every failure path is logged and returns false so the caller falls
// back to the built-in placeholder clip - a bad or unreachable URL never
// stops the rest of the firmware from working.
static bool fetchWav() {
  if (!g_settings.audioUrl[0]) return false;

  WiFiClientSecure secure;
  WiFiClient plain;
  const bool https = !strncmp(g_settings.audioUrl, "https", 5);
  WiFiClient* client = https ? (WiFiClient*)&secure : &plain;
  if (https) secure.setInsecure();                 // encrypted but not certificate-verified, same as the ADS-B feed

  HTTPClient http;
  http.setConnectTimeout(g_settings.audioFetchTimeoutMs);
  http.setTimeout(g_settings.audioFetchTimeoutMs);
  if (!http.begin(*client, g_settings.audioUrl)) {
    Serial.println("[audio] MILITARY_WAV_URL could not be opened (bad URL?)");
    return false;
  }
  const int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("[audio] download failed: HTTP %d\n", code);
    http.end();
    return false;
  }
  const int len = http.getSize();
  if (len <= 0 || (uint32_t)len > g_settings.audioMaxBytes) {
    Serial.printf("[audio] download size is missing, zero, or over the %u byte cap (got %d) - "
                  "the host may be using chunked transfer, which isn't supported here\n",
                  (unsigned)g_settings.audioMaxBytes, len);
    http.end();
    return false;
  }
  uint8_t* buf = (uint8_t*)allocLarge((size_t)len);
  if (!buf) {
    Serial.printf("[audio] not enough memory for a %d byte download\n", len);
    http.end();
    return false;
  }

  WiFiClient& stream = http.getStream();
  int got = 0;
  const uint32_t deadline = millis() + g_settings.audioFetchTimeoutMs * 4;
  while (got < len) {
    const int avail = stream.available();
    if (avail > 0) {
      const size_t want = (size_t)((avail < (len - got)) ? avail : (len - got));
      const size_t n = stream.readBytes(buf + got, want);
      if (n > 0) got += (int)n;
    } else if (!http.connected()) {
      break;                                          // connection closed early
    }
    if ((int32_t)(millis() - deadline) > 0) break;      // overall stall guard
    delay(2);
  }
  http.end();

  if (got != len) {
    Serial.printf("[audio] download incomplete (%d of %d bytes) - using the placeholder clip instead\n", got, len);
    free(buf);
    return false;
  }
  WavInfo info;
  char why[48] = "";
  if (!parseWav(buf, (uint32_t)len, info, why, sizeof(why))) {
    Serial.printf("[audio] downloaded file is not a playable WAV (%s) - using the placeholder clip instead\n", why);
    free(buf);
    return false;
  }
  s_fetchedBuf = buf;
  s_wav = info;
  return true;
}

static void audioTask(void*) {
  Serial.printf("[audio] audio task starting on core %d, MILITARY_WAV_LEN=%u bytes\n",
                (int)xPortGetCoreID(), (unsigned)MILITARY_WAV_LEN);
  char why[48] = "";
  if (!parseWav(MILITARY_WAV, MILITARY_WAV_LEN, s_wav, why, sizeof(why))) {
    Serial.printf("[audio] FATAL: the built-in clip (audio_data.h) is invalid (%s) - "
                  "audio task stopped, speaker test will report unavailable\n", why);
    s_task = nullptr;
    vTaskDelete(nullptr);
    return;
  }
  Serial.printf("[audio] built-in clip parsed OK: %lu Hz, %u channel(s), %lu bytes of PCM data\n",
                (unsigned long)s_wav.sampleRate, s_wav.channels, (unsigned long)s_wav.dataLen);
  Serial.println("[audio] audio task ready, waiting for requests");

  for (;;) {
    uint32_t requests = 0;
    xTaskNotifyWait(0, UINT32_MAX, &requests, portMAX_DELAY);
    AUDIO_LOGV("[audio][v] audio task woke with requests=0x%02x "
               "(ALERT=%d TEST=%d CLIP=%d)\n", (unsigned)requests,
               (int)((requests & AUDIO_NOTIFY_ALERT) != 0), (int)((requests & AUDIO_NOTIFY_TEST) != 0),
               (int)((requests & AUDIO_NOTIFY_CLIP) != 0));
    if (requests & AUDIO_NOTIFY_ALERT_7700) playEmergency7700Pattern();
    if (requests & AUDIO_NOTIFY_ALERT) playEmergencyTone();
    if (requests & AUDIO_NOTIFY_POLICE) playPoliceTone();
    if (requests & AUDIO_NOTIFY_COASTGUARD) playCoastguardTone();
    if (requests & AUDIO_NOTIFY_AIRAMB) playAirAmbulanceTone();
    if (requests & AUDIO_NOTIFY_TEST) {
      Serial.println("[audio] speaker test requested; playing 500 Hz test tone now");
      const bool ok = playTone(500, 1000, 18000);
      Serial.printf("[audio] speaker test tone %s\n", ok ? "completed" : "FAILED - see error above");
      requests |= AUDIO_NOTIFY_CLIP;
    }
    if (requests & AUDIO_NOTIFY_CLIP) {
      s_clipBusy = true;
      if (!s_fetchedBuf && g_settings.audioUrl[0]) {
        const uint32_t waitStart = millis();
        while (!wifiIsConnected() &&
               (uint32_t)(millis() - waitStart) < g_settings.audioWifiWaitMs) {
          uint32_t pending = 0;
          if (xTaskNotifyWait(0, UINT32_MAX, &pending, pdMS_TO_TICKS(250)) == pdTRUE &&
              (pending & AUDIO_NOTIFY_ALERT)) {
            playEmergencyTone();
          }
        }
        if (wifiIsConnected() && fetchWav()) {
          Serial.printf("[audio] using configured WAV: %lu Hz, %u channel(s), %lu KB\n",
                        (unsigned long)s_wav.sampleRate, s_wav.channels,
                        (unsigned long)(s_wav.dataLen / 1024));
        } else if (!wifiIsConnected()) {
          Serial.println("[audio] WiFi unavailable; playing built-in clip");
        }
      }
      const esp_err_t clipRateResult = i2s_set_sample_rates(I2S_NUM_0, s_wav.sampleRate);
      if (clipRateResult != ESP_OK) {
        Serial.printf("[audio] playback ABORTED: could not set clip sample rate %lu Hz: %s\n",
                      (unsigned long)s_wav.sampleRate, esp_err_to_name(clipRateResult));
        s_clipBusy = false;
        continue;
      }
      const int16_t* src = (const int16_t*)s_wav.data;
      const uint32_t samples = s_wav.dataLen / 2 / s_wav.channels;
      const uint32_t CHUNK = 256;
      int16_t out[CHUNK * 2];                          // always written as interleaved stereo
      Serial.printf("[audio] playing clip: %lu samples @ %lu Hz, %u channel(s)\n",
                    (unsigned long)samples, (unsigned long)s_wav.sampleRate, s_wav.channels);
      const uint32_t clipCallsBefore = s_i2sWriteCalls;
      bool interrupted = false;
      for (uint32_t i = 0; i < samples; i += CHUNK) {
        const uint32_t n = (samples - i < CHUNK) ? (samples - i) : CHUNK;
        for (uint32_t j = 0; j < n; j++) {
          int16_t l, r;
          if (s_wav.channels == 1) {
            l = r = src[i + j];
          } else {
            l = src[(i + j) * 2];
            r = src[(i + j) * 2 + 1];
          }
          out[j * 2] = l;
          out[j * 2 + 1] = r;
        }
        if (!writeI2s(out, n * 2)) { interrupted = true; break; }
        uint32_t urgent = 0;
        if (xTaskNotifyWait(0, UINT32_MAX, &urgent, 0) == pdTRUE &&
            (urgent & AUDIO_NOTIFY_ALERT)) {
          playEmergencyTone();
          if (urgent & AUDIO_NOTIFY_CLIP)
            xTaskNotify(s_task, AUDIO_NOTIFY_CLIP, eSetBits);
          interrupted = true;
          break;
        }
      }
      Serial.printf("[audio] clip playback %s (%lu i2s_write() calls)\n",
                    interrupted ? "INTERRUPTED" : "finished",
                    (unsigned long)(s_i2sWriteCalls - clipCallsBefore));
      s_clipBusy = false;
    }
  }
}

void audioInit() {
  Serial.println("[audio] ===== audioInit() starting =====");
  Serial.printf("[audio] config: AUDIO_ENABLED(setting)=%d  BCLK=GPIO%d  LRC=GPIO%d  DIN=GPIO%d\n",
                (int)g_settings.audioEnabled, (int)AUDIO_I2S_BCLK_PIN, (int)AUDIO_I2S_LRC_PIN,
                (int)AUDIO_I2S_DOUT_PIN);
  if (!g_settings.audioEnabled) {
    Serial.println("[audio] announcements disabled in settings; speaker test remains available regardless");
  }
  i2s_config_t cfg = {};
  cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
  cfg.sample_rate = 16000;                             // re-set once the real clip's rate is known isn't
                                                        // supported mid-stream by this driver call, so pick a
                                                        // sane default; i2s_set_sample_rates() below corrects it
  cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
  cfg.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
  cfg.communication_format = (i2s_comm_format_t)I2S_COMM_FORMAT_STAND_I2S;
  cfg.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
  cfg.dma_buf_count = 4;
  cfg.dma_buf_len = 256;
  cfg.use_apll = false;

  const esp_err_t installResult = i2s_driver_install(I2S_NUM_0, &cfg, 0, nullptr);
  if (installResult != ESP_OK) {
    Serial.printf("[audio] FATAL: i2s_driver_install failed: %s - announcements disabled\n",
                  esp_err_to_name(installResult));
    return;
  }
  Serial.println("[audio] i2s_driver_install OK (16000 Hz, 16-bit, stereo, 4x256-sample DMA buffers)");

  i2s_pin_config_t pins = {};
  pins.bck_io_num = AUDIO_I2S_BCLK_PIN;
  pins.ws_io_num = AUDIO_I2S_LRC_PIN;
  pins.data_out_num = AUDIO_I2S_DOUT_PIN;
  pins.data_in_num = I2S_PIN_NO_CHANGE;
  const esp_err_t pinResult = i2s_set_pin(I2S_NUM_0, &pins);
  if (pinResult != ESP_OK) {
    Serial.printf("[audio] FATAL: i2s_set_pin failed: %s (BCLK=%d LRC=%d DIN=%d) - announcements disabled\n",
                  esp_err_to_name(pinResult), (int)AUDIO_I2S_BCLK_PIN, (int)AUDIO_I2S_LRC_PIN,
                  (int)AUDIO_I2S_DOUT_PIN);
    return;
  }
  Serial.printf("[audio] i2s_set_pin OK: BCLK=GPIO%d LRC=GPIO%d DIN=GPIO%d\n",
                (int)AUDIO_I2S_BCLK_PIN, (int)AUDIO_I2S_LRC_PIN, (int)AUDIO_I2S_DOUT_PIN);

  // Belt-and-braces: i2s_driver_install() is expected to leave the port
  // running, but explicitly starting it here is harmless if it's already
  // running and closes off one possible silent-failure mode (DMA writes
  // succeed, driver reports OK, but the peripheral's clocks were never
  // actually enabled).
  const esp_err_t startResult = i2s_start(I2S_NUM_0);
  Serial.printf("[audio] i2s_start -> %s\n", esp_err_to_name(startResult));

  const BaseType_t taskResult = xTaskCreatePinnedToCore(audioTask, "audio", 8192, nullptr, 1, &s_task, 0);
  if (taskResult != pdPASS) {
    s_task = nullptr;
    Serial.printf("[audio] FATAL: could not create audio task (xTaskCreatePinnedToCore returned %d) - "
                  "speaker test unavailable\n", (int)taskResult);
  } else {
    Serial.println("[audio] audio task created OK");
  }
  Serial.println("[audio] ===== audioInit() done =====");
}

void audioPlayMilitary() {
  if (!g_settings.audioEnabled) {
    AUDIO_LOGV("[audio][v] audioPlayMilitary: ignored, announcements disabled in settings\n");
    return;
  }
  if (!s_task || xTaskNotify(s_task, AUDIO_NOTIFY_CLIP, eSetBits) != pdPASS)
    Serial.println("[audio] military announcement unavailable: audio task is not ready");
}

bool audioTestSpeaker() {
  Serial.println("[audio] audioTestSpeaker() called");
  if (!s_task) {
    Serial.println("[audio] speaker test UNAVAILABLE: audio task does not exist - check the "
                    "i2s_driver_install/i2s_set_pin/task-create lines printed at boot above");
    return false;
  }
  if (xTaskNotify(s_task, AUDIO_NOTIFY_TEST, eSetBits) != pdPASS) {
    Serial.println("[audio] speaker test UNAVAILABLE: xTaskNotify failed (should not happen)");
    return false;
  }
  Serial.println("[audio] speaker test notification queued OK - watch for "
                  "'speaker test requested' below");
  // Notifications queue while the configured WAV is loading.
  return true;
}

void audioPlayEmergencyBeep() {
  if (!s_task || xTaskNotify(s_task, AUDIO_NOTIFY_ALERT, eSetBits) != pdPASS)
    Serial.println("[audio] emergency beep unavailable: audio task is not ready");
}

void audioPlayPolice() {
  if (!s_task || xTaskNotify(s_task, AUDIO_NOTIFY_POLICE, eSetBits) != pdPASS)
    Serial.println("[audio] police tone unavailable: audio task is not ready");
}

void audioPlayCoastguard() {
  if (!s_task || xTaskNotify(s_task, AUDIO_NOTIFY_COASTGUARD, eSetBits) != pdPASS)
    Serial.println("[audio] coastguard tone unavailable: audio task is not ready");
}

void audioPlayAirAmbulance() {
  if (!s_task || xTaskNotify(s_task, AUDIO_NOTIFY_AIRAMB, eSetBits) != pdPASS)
    Serial.println("[audio] air ambulance tone unavailable: audio task is not ready");
}

void audioPlayEmergency7700() {
  if (!s_task || xTaskNotify(s_task, AUDIO_NOTIFY_ALERT_7700, eSetBits) != pdPASS)
    Serial.println("[audio] 7700 alert unavailable: audio task is not ready");
}
