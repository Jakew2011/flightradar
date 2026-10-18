// aircraft.h - aircraft store, classification, motion interpolation, selection.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "config.h"

enum AircraftClass : uint8_t {
  AC_AIRLINER = 0, AC_HELICOPTER, AC_LIGHT, AC_MILITARY, AC_UNKNOWN, AC_CLASS_COUNT
};

struct Aircraft {
  double   latitude, longitude;     // last reported position
  uint32_t key;                     // 24-bit ICAO address as a number (0 = invalid)
  uint16_t squawk;                  // transponder squawk; 7500/7700 trigger global alerts
  uint32_t reportMs;                // millis() time the position was valid
  uint32_t commitMs;                // millis() time this record was merged
  int32_t  altitude;                // feet (barometric), valid if hasAltitude
  float    groundSpeed;             // knots, valid if hasSpeed
  float    heading;                 // degrees true track, valid if hasHeading
  float    distance;                // nautical miles from home (current, interpolated)
  float    bearing;                 // degrees true from home (current)
  float    x0, y0;                  // east/north nm from home at reportMs
  float    vx, vy;                  // nm per second
  float    offX, offY;              // display glide offset (decays to 0)
  float    x, y;                    // current display position, nm east/north
  char     icao[8];
  char     callsign[9];
  char     registration[13];
  char     aircraftType[5];
  char     operatorName[26];
  bool     hasAltitude, hasSpeed, hasHeading, onGround, military;
  bool     visible, selected;
  AircraftClass cls;
};

void  aircraftInit();                                   // allocate buffers
void* aircraftAllocLarge(size_t bytes);                 // PSRAM if present, else heap

// Called from the network task with a complete new list.
void  aircraftSubmit(const Aircraft* list, uint16_t count, uint32_t homeRev);

// Called from the main loop once per frame: merges new data, interpolates,
// removes stale aircraft and computes distance/bearing/visibility.
void  aircraftUpdate(uint32_t nowMs, float rangeNm);

Aircraft* aircraftList();
uint16_t  aircraftCount();
uint16_t  aircraftVisibleCount();
uint16_t  aircraftVisibleMilitaryCount();

// Selection (by ICAO address, survives list updates).
bool      aircraftSelectNearest();          // nearest visible to radar centre
bool      aircraftCycle(int dir);           // +1 next farther, -1 next nearer
void      aircraftClearSelection();
bool      aircraftHasSelection();
const Aircraft* aircraftSelected();
bool      aircraftEmergencyActive();
uint16_t  aircraftEmergencySquawk();

// Helpers used while parsing. dbFlagMilitary comes from the ADS-B "dbFlags"
// bit; the function ORs in category, historic warbird types and known
// military operators, and reports the combined result via *outMilitary
// (military overrides class to AC_MILITARY, whatever the airframe).
AircraftClass aircraftClassify(const char* category, const char* type, const char* callsign,
                               bool dbFlagMilitary, bool* outMilitary);
const char*   aircraftOperatorFromCallsign(const char* callsign);   // nullptr if unknown
const char*   aircraftDisplayName(const Aircraft& a);               // callsign / reg / icao

// Called once per aircraftUpdate(). Returns true (and should be called again)
// once per aircraft that just became visible, is military, and has not been
// announced in the last MILITARY_ANNOUNCE_COOLDOWN_MS - drain it in loop()
// and play the announcement sound for each true it returns.
bool aircraftPollAnnouncement();
