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
  delay(30);
  s_backAtBoot = (digitalRead(BUTTON_PIN_BACK) == LOW);
  s_queue = xQueueCreate(32, sizeof(InputEvent));
  xTaskCreatePinnedToCore(inputTask, "input", 3072, nullptr, 3, nullptr, 0);
}

bool inputPoll(InputEvent& ev) {
  return s_queue && xQueueReceive(s_queue, &ev, 0) == pdTRUE;
}

bool inputBackHeldAtBoot() { return s_backAtBoot; }
