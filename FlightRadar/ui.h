// ui.h - screens, menus, input handling and frame scheduling.
#pragma once
#include <stdint.h>
#include "input.h"

void uiInit();
void uiHandleEvent(const InputEvent& ev, uint32_t nowMs);
void uiUpdate(uint32_t nowMs);          // call every loop(); draws when a frame is due
void uiShowMilitaryAlert();             // full-screen red warning for the duration of the announcement
