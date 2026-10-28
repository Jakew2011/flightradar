// aircraft.h - aircraft store, classification, motion interpolation, selection.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "config.h"

enum AircraftClass : uint8_t {
  AC_AIRLINER = 0, AC_HELICOPTER, AC_LIGHT, AC_MILITARY, AC_UNKNOWN, AC_CLASS_COUNT
};

// Best-effort UK emergency-service detection (callsign/registration pattern
// matching - see classifyService() in aircraft.cpp). Independent of
// AircraftClass: a police/coastguard/air-ambulance aircraft still shows as
// AC_HELICOPTER (or whatever it really is) on the radar, this just adds a
// distinct audio alert and header count.
enum AircraftService : uint8_t {
  SVC_NONE = 0, SVC_POLICE, SVC_COASTGUARD, SVC_AIR_AMBULANCE
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
  AircraftService service;
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
uint16_t  aircraftVisibleServiceCount();                // police + coastguard + air ambulance, combined

// Selection (by ICAO address, survives list updates).
bool      aircraftSelectNearest();          // nearest visible to radar centre
bool      aircraftCycle(int dir);           // +1 next farther, -1 next nearer
void      aircraftClearSelection();
bool      aircraftHasSelection();
const Aircraft* aircraftSelected();
bool      aircraftEmergencyActive();
uint16_t  aircraftEmergencySquawk();
// Lets the radar give control back to the user during an active special-
// squawk emergency: the header alert (and audio, for the two that have any)
// keep going, but the radar stops force-centring/force-zooming on that
// specific aircraft, so range changes and BACK/HOME work normally again.
// Re-engages automatically the next time a *different* aircraft (or the
// same one, after its emergency clears and starts again) squawks one of
// these.
void      aircraftDismissEmergencyFollow();

// ---------------------------------------------------------------------------
//  Special ("emergency-class") squawk codes. One shared table so the radar
//  centring/forced-visibility behaviour, the header/toast text, and the
//  global out-of-range lookup (adsb_api.cpp) all agree on exactly the same
//  set instead of each hard-coding its own "== 7500 || == 7700" check.
//  7500 (hijack) and 7700 (general emergency) are the only two that make a
//  sound; 7600 (radio failure) gets the same radar/visual treatment -
//  forced visible regardless of range, auto-centred/followed, shown in the
//  header, protected from being evicted from a full aircraft list - but
//  silently.
// ---------------------------------------------------------------------------
struct SpecialSquawk {
  uint16_t    code;
  const char* headerLabel;  // short word for the radar header, e.g. "HIJACK"
  const char* toastLabel;   // full toast text, e.g. "HIJACK SQUAWK 7500"
  bool        audible;      // true only for 7500 and 7700
  uint8_t     priority;     // lower = takes priority when more than one is active at once
};
extern const SpecialSquawk SPECIAL_SQUAWKS[];
extern const size_t        SPECIAL_SQUAWK_COUNT;
// nullptr if sq isn't one of the codes in SPECIAL_SQUAWKS (including sq==0).
const SpecialSquawk* aircraftSpecialSquawk(uint16_t sq);

// Whatever the radar is currently centred on this frame (home, or an active
// emergency/search-lock aircraft - see aircraftUpdate()). Valid any time
// after aircraftUpdate() has run at least once; for drawing anything else
// (airport markers, etc.) in the same east/north frame the aircraft list uses.
void aircraftMapCenter(double& lat, double& lon);

// Search-and-lock by callsign or registration (case-insensitive; hyphens in
// a registration are ignored both when entering and matching - "G-ABCD" and
// "GABCD" are the same query). The query is remembered and re-checked every
// aircraftUpdate(), so it locks on the moment a matching aircraft appears,
// even after it was out of range or momentarily lost. While locked, the
// radar centres on that aircraft (emergency squawks still take priority) and
// it's auto-selected, same as the emergency behaviour. Any manual selection
// (aircraftSelectNearest/aircraftCycle/aircraftClearSelection) clears the lock.
void        aircraftLockSearch(const char* query);   // up to 8 significant characters
void        aircraftClearLock();
bool        aircraftLockActive();     // true once the query has actually matched an aircraft
bool        aircraftLockPending();    // true while a query is set but hasn't matched (yet)
const char* aircraftLockQuery();      // current query as entered/normalized, "" if none

// Helpers used while parsing. dbFlagMilitary comes from the ADS-B "dbFlags"
// bit; the function ORs in category, historic warbird types and known
// military operators, and reports the combined result via *outMilitary
// (military overrides class to AC_MILITARY, whatever the airframe).
AircraftClass aircraftClassify(const char* category, const char* type, const char* callsign,
                               bool dbFlagMilitary, bool* outMilitary);
const char*   aircraftOperatorFromCallsign(const char* callsign);   // nullptr if unknown
const char*   aircraftDisplayName(const Aircraft& a);               // callsign / reg / icao

// Best-effort UK police / coastguard / air-ambulance detection from the
// callsign and/or registration (whichever the feed provided). Independent of
// aircraftClassify() - call separately once both fields are populated.
AircraftService aircraftClassifyService(const char* callsign, const char* registration);
const char*     aircraftServiceName(AircraftService svc);          // "POLICE" / "COASTGUARD" / "AIR AMBULANCE" / ""

// Called once per aircraftUpdate(). Returns true (and should be called again)
// once per aircraft that just became visible, is military, and has not been
// announced in the last MILITARY_ANNOUNCE_COOLDOWN_MS - drain it in loop()
// and play the announcement sound for each true it returns.
bool aircraftPollAnnouncement();

// Same idea as aircraftPollAnnouncement(), for police/coastguard/air-ambulance
// aircraft: drain in loop() and play the matching audioPlay*() tone for each
// AircraftService it returns.
bool aircraftPollServiceAnnouncement(AircraftService& outService);
