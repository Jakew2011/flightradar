// input.h - rotary encoder + buttons. A background task polls the pins every
// millisecond (debounced) and queues events, so no click or turn is missed
// while the screen is being redrawn.
#pragma once
#include <stdint.h>

enum InputEventType : uint8_t {
  EVT_NONE = 0,
  EVT_ENC_CW,      // encoder turned clockwise (one click)
  EVT_ENC_CCW,     // encoder turned counter-clockwise (one click)
  EVT_ENC_SHORT,   // encoder button short press
  EVT_ENC_LONG,    // encoder button long press (fires while still held)
  EVT_MENU,        // MENU button
  EVT_BACK,        // BACK button
  EVT_HOME         // HOME button
};

struct InputEvent {
  InputEventType type;
};

void inputInit();                        // configure pins, start the task
bool inputPoll(InputEvent& ev);          // true if an event was waiting
bool inputBackHeldAtBoot();              // BACK was held when the board started
