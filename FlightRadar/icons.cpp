#include "icons.h"
#include <math.h>

// Icon geometry: 1 unit = half a pixel at scale 1.0, nose towards -Y.
// Filled shapes are lists of triangles (x1,y1,x2,y2,x3,y3); thin parts are lines
// (x1,y1,x2,y2).
struct IconDef {
  const int8_t* tris;  uint8_t nTris;
  const int8_t* lines; uint8_t nLines;
};

static const int8_t AIRLINER_TRIS[] = {
  0,-18, -2,-12, 2,-12,     // nose
  -2,-12, 2,-12, 2,14,      // fuselage
  -2,-12, -2,14, 2,14,
  -2,-5, -17,7, -2,3,       // swept wings
  2,-5, 17,7, 2,3,
  -2,10, -8,16, -2,14,      // tailplane
  2,10, 8,16, 2,14,
};
static const int8_t LIGHT_TRIS[] = {
  0,-14, -2,-8, 2,-8,       // nose
  -2,-8, 2,-8, 1,13,        // fuselage
  -2,-8, -1,13, 1,13,
  -2,-7, -17,-7, -17,-2,    // straight wings
  -2,-7, -17,-2, -2,-2,
  2,-7, 17,-7, 17,-2,
  2,-7, 17,-2, 2,-2,
  -1,9, -7,9, -7,12,        // tailplane
  -1,9, -7,12, -1,12,
  1,9, 7,9, 7,12,
  1,9, 7,12, 1,12,
};
static const int8_t HELI_TRIS[] = {
  0,-9, -4,-2, 4,-2,        // cabin
  -4,-2, 4,-2, 0,5,
  -3,17, 3,17, 0,12,        // tail fin
};
static const int8_t HELI_LINES[] = {
  0,4, 0,16,                // tail boom
  -13,-13, 13,9,            // rotor (X)
  13,-13, -13,9,
};
static const int8_t MIL_TRIS[] = {
  0,-18, -2,-4, 2,-4,       // nose
  -2,-4, 2,-4, 0,14,        // fuselage
  0,-6, -15,12, 0,7,        // delta wings
  0,-6, 15,12, 0,7,
};
static const int8_t UNK_LINES[] = {
  0,-8, 8,0,  8,0, 0,8,  0,8, -8,0,  -8,0, 0,-8,   // hollow diamond
};

static const IconDef ICONS[AC_CLASS_COUNT] = {
  { AIRLINER_TRIS, sizeof(AIRLINER_TRIS) / 6, nullptr, 0 },                       // AC_AIRLINER
  { HELI_TRIS,     sizeof(HELI_TRIS) / 6,     HELI_LINES, sizeof(HELI_LINES) / 4 }, // AC_HELICOPTER
  { LIGHT_TRIS,    sizeof(LIGHT_TRIS) / 6,    nullptr, 0 },                       // AC_LIGHT
  { MIL_TRIS,      sizeof(MIL_TRIS) / 6,      nullptr, 0 },                       // AC_MILITARY
  { nullptr, 0,                               UNK_LINES, sizeof(UNK_LINES) / 4 },   // AC_UNKNOWN
};

void iconDraw(TFT_eSprite& spr, AircraftClass cls, int16_t cx, int16_t cy,
              float headingDeg, uint16_t colour, float scale) {
  if (cls >= AC_CLASS_COUNT) cls = AC_UNKNOWN;
  const IconDef& d = ICONS[cls];
  const float rad = headingDeg * 0.017453292519943295f;
  const float s = sinf(rad), c = cosf(rad);
  const float k = 0.5f * scale;

  auto tx = [&](int8_t ux, int8_t uy) -> int16_t { return (int16_t)lroundf(cx + (ux * c - uy * s) * k); };
  auto ty = [&](int8_t ux, int8_t uy) -> int16_t { return (int16_t)lroundf(cy + (ux * s + uy * c) * k); };

  for (uint8_t i = 0; i < d.nTris; i++) {
    const int8_t* t = d.tris + i * 6;
    spr.fillTriangle(tx(t[0], t[1]), ty(t[0], t[1]), tx(t[2], t[3]), ty(t[2], t[3]),
                     tx(t[4], t[5]), ty(t[4], t[5]), colour);
  }
  for (uint8_t i = 0; i < d.nLines; i++) {
    const int8_t* l = d.lines + i * 4;
    spr.drawLine(tx(l[0], l[1]), ty(l[0], l[1]), tx(l[2], l[3]), ty(l[2], l[3]), colour);
  }
}
