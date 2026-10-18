// icons.h - vector aircraft icons (rotated by heading) drawn into a sprite.
#pragma once
#include <TFT_eSPI.h>
#include "aircraft.h"

// Draw the icon for class "cls" centred on (cx, cy). headingDeg: 0 = nose
// pointing up (north), clockwise. scale 1.0 = ~18 px across.
void iconDraw(TFT_eSprite& spr, AircraftClass cls, int16_t cx, int16_t cy,
              float headingDeg, uint16_t colour, float scale);
