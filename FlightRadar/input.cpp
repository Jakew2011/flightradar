#include "input.h"
#include "config.h"
#include "settings.h"
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>

static QueueHandle_t s_queue = nullptr;
static bool s_backAtBoot = false;

static void emit(InputEventType t) {
  if (!s_queue) return;
  InputEvent e = { t };
  xQueueSend(s_queue, &e, 0);          // if the queue is full the event is dropped
}

// ---------------------------------------------------------------------------
//  Touchscreen (optional, XPT2046-style, config.h TOUCH_ENABLED) - purely
//  additive testing support: a tap maps to the same EVT_ENC_SHORT the
//  encoder button sends (select/confirm/next), a held tap to EVT_ENC_LONG
//  (open menu), so every screen already works with it with zero UI changes.
//  This isn't a full touch UI, just enough to confirm the touch IC and
//  wiring actually work. Hand-rolled bit-banged SPI on its own dedicated
//  pins (see config.h) rather than a hardware SPI peripheral or an extra
//  library dependency - touch sampling has no speed requirement that needs
//  it, and this keeps the touch IC fully isolated from the TFT's SPI bus.
// ---------------------------------------------------------------------------
#if TOUCH_ENABLED
static uint16_t touchXfer(uint8_t cmd) {
  digitalWrite(TOUCH_CS_PIN, LOW);
  for (int i = 7; i >= 0; --i) {
    digitalWrite(TOUCH_CLK_PIN, LOW);
    digitalWrite(TOUCH_MOSI_PIN, (cmd >> i) & 1);
    delayMicroseconds(2);
    digitalWrite(TOUCH_CLK_PIN, HIGH);
    delayMicroseconds(2);
  }
  uint16_t value = 0;
  for (int i = 0; i < 16; ++i) {           // 1 dummy bit + 12 data bits + 3 trailing bits
    digitalWrite(TOUCH_CLK_PIN, LOW);
    delayMicroseconds(2);
    value = (uint16_t)((value << 1) | (digitalRead(TOUCH_MISO_PIN) ? 1 : 0));
    digitalWrite(TOUCH_CLK_PIN, HIGH);
    delayMicroseconds(2);
  }
  digitalWrite(TOUCH_CLK_PIN, LOW);
  digitalWrite(TOUCH_CS_PIN, HIGH);
  return (uint16_t)(value >> 3);           // right-align the 12-bit result
}

// Reads raw X/Y a couple of times and averages, to steady out noisy panels.
// Returns false if the reading doesn't look like an actual touch.
static bool touchReadRaw(uint16_t& rawX, uint16_t& rawY) {
  uint32_t sx = 0, sy = 0;
  const uint8_t samples = 3;
  for (uint8_t i = 0; i < samples; ++i) {
    sx += touchXfer(0xD0);
    sy += touchXfer(0x90);
  }
  rawX = (uint16_t)(sx / samples);
  rawY = (uint16_t)(sy / samples);
  // A floating/unpressed XPT2046 reads near 0 or near 4095 (full scale);
  // a real touch lands somewhere in between.
  return rawX > 80 && rawX < 4015 && rawY > 80 && rawY < 4015;
}

static bool touchIsDown() {
#if TOUCH_IRQ_PIN >= 0
  if (digitalRead(TOUCH_IRQ_PIN) != LOW) return false;   // IRQ is active-low while touched
#endif
  uint16_t x, y;
  return touchReadRaw(x, y);
}
#endif  // TOUCH_ENABLED

struct Button {
  uint8_t  pin;
  bool     state;       // debounced level: true = pressed
  bool     raw;
  uint32_t tChange;
  uint32_t tPress;
  bool     longFired;
  InputEventType onPress;   // EVT_NONE for the encoder button (special handling)
};

// Quadrature decoding table indexed by (previous<<2 | current) for pins (A<<1 | B).
static const int8_t ENC_TABLE[16] = { 0, 1,-1, 0,  -1, 0, 0, 1,   1, 0, 0,-1,   0,-1, 1, 0 };

static uint8_t readEnc() {
  return (uint8_t)((digitalRead(ENCODER_PIN_A) ? 2 : 0) | (digitalRead(ENCODER_PIN_B) ? 1 : 0));
}

static void inputTask(void*) {
  Button btn[4] = {
    { (uint8_t)ENCODER_PIN_SW,   false, false, 0, 0, false, EVT_NONE },
    { (uint8_t)BUTTON_PIN_MENU,  false, false, 0, 0, false, EVT_MENU },
    { (uint8_t)BUTTON_PIN_BACK,  false, false, 0, 0, false, EVT_BACK },
    { (uint8_t)BUTTON_PIN_HOME,  false, false, 0, 0, false, EVT_HOME },
  };
  uint8_t prev = readEnc();
  int8_t acc = 0;
  uint32_t lastEdge = 0;

#if TOUCH_ENABLED
  bool touchDown = false, touchLongFired = false;
  uint32_t touchStart = 0, touchCooldownUntil = 0, lastTouchLog = 0;
#endif

  for (;;) {
    const uint32_t now = millis();

    // --- encoder ---
    const uint8_t cur = readEnc();
    if (cur != prev) {
      acc += ENC_TABLE[(prev << 2) | cur];
      prev = cur;
      lastEdge = now;
      if (acc >= g_settings.encoderStepsPerDetent) { acc = 0; emit(g_settings.encoderReverse ? EVT_ENC_CCW : EVT_ENC_CW); }
      else if (acc <= -(int8_t)g_settings.encoderStepsPerDetent) { acc = 0; emit(g_settings.encoderReverse ? EVT_ENC_CW : EVT_ENC_CCW); }
    } else if (acc != 0 && (now - lastEdge) > 400) {
      acc = 0;                                     // half-finished click: forget it
    }

    // --- buttons (active low) ---
    for (uint8_t i = 0; i < 4; i++) {
      Button& b = btn[i];
      const bool reading = (digitalRead(b.pin) == LOW);
      if (reading != b.raw) { b.raw = reading; b.tChange = now; }
      if (b.state != b.raw && (now - b.tChange) >= g_settings.buttonDebounceMs) {
        b.state = b.raw;
        if (b.state) {                              // pressed
          b.tPress = now;
          b.longFired = false;
          if (b.onPress != EVT_NONE) emit(b.onPress);
        } else if (b.onPress == EVT_NONE && !b.longFired) {   // encoder button released
          emit(EVT_ENC_SHORT);
        }
      }
      if (b.state && b.onPress == EVT_NONE && !b.longFired && (now - b.tPress) >= g_settings.longPressMs) {
        b.longFired = true;
        emit(EVT_ENC_LONG);
      }
    }

#if TOUCH_ENABLED
    // --- touchscreen: tap = EVT_ENC_SHORT, held tap = EVT_ENC_LONG - see the
    // comment above touchIsDown() for why this is a deliberately minimal mapping.
    if (now >= touchCooldownUntil) {
      const bool down = touchIsDown();
      if (down && !touchDown) {
        touchDown = true;
        touchLongFired = false;
        touchStart = now;
      } else if (down && touchDown && !touchLongFired && (now - touchStart) >= g_settings.longPressMs) {
        touchLongFired = true;
        emit(EVT_ENC_LONG);
      } else if (!down && touchDown) {
        touchDown = false;
        if (!touchLongFired) emit(EVT_ENC_SHORT);
        touchCooldownUntil = now + TOUCH_DEBOUNCE_MS;
      }
      if (down && (now - lastTouchLog) > 500) {
        lastTouchLog = now;
        uint16_t x, y;
        touchReadRaw(x, y);
        Serial.printf("[touch] raw x=%u y=%u\n", (unsigned)x, (unsigned)y);
      }
    }
#endif

    vTaskDelay(1);
  }
}

void inputInit() {
  pinMode(ENCODER_PIN_A, INPUT_PULLUP);
  pinMode(ENCODER_PIN_B, INPUT_PULLUP);
  pinMode(ENCODER_PIN_SW, INPUT_PULLUP);
  pinMode(BUTTON_PIN_MENU, INPUT_PULLUP);
  pinMode(BUTTON_PIN_BACK, INPUT_PULLUP);
  pinMode(BUTTON_PIN_HOME, INPUT_PULLUP);
#if TOUCH_ENABLED
  pinMode(TOUCH_CLK_PIN, OUTPUT);
  pinMode(TOUCH_MOSI_PIN, OUTPUT);
  pinMode(TOUCH_MISO_PIN, INPUT);
  pinMode(TOUCH_CS_PIN, OUTPUT);
  digitalWrite(TOUCH_CS_PIN, HIGH);
  digitalWrite(TOUCH_CLK_PIN, LOW);
#if TOUCH_IRQ_PIN >= 0
  pinMode(TOUCH_IRQ_PIN, INPUT_PULLUP);
#endif
  Serial.println("[touch] enabled (test mode): tap = select, hold = menu. Watch Serial for raw x/y to calibrate TOUCH_RAW_*_MIN/MAX in config.h.");
#endif
  delay(30);
  s_backAtBoot = (digitalRead(BUTTON_PIN_BACK) == LOW);
  s_queue = xQueueCreate(32, sizeof(InputEvent));
  xTaskCreatePinnedToCore(inputTask, "input", 3072, nullptr, 3, nullptr, 0);
}

bool inputPoll(InputEvent& ev) {
  return s_queue && xQueueReceive(s_queue, &ev, 0) == pdTRUE;
}

bool inputBackHeldAtBoot() { return s_backAtBoot; }
