// radar.h - radar scope drawing (rings, compass, sweep, aircraft, labels).
#pragma once
#include <stdint.h>

// Clear the radar sprite and draw sweep + grid (+ compass letters).
void radarDrawBase(uint32_t nowMs, float rangeNm, bool compass);
// Draw all visible aircraft, labels and the selection marker on top.
void radarDrawAircraft(float rangeNm);
// Copy the radar sprite to the screen.
void radarPush();
// Convenience: base + aircraft + push.
void radarRender(uint32_t nowMs, float rangeNm);
// Sweep revolution time in ms for the current "radar speed" setting (0 = off).
uint16_t radarSweepPeriodMs();
