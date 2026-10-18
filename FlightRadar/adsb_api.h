// adsb_api.h - live ADS-B data download (runs in its own FreeRTOS task so the
// radar animation never waits for the network).
//
// ---------------------------------------------------------------------------
//  DATA SOURCES (configure in config.h)
// ---------------------------------------------------------------------------
//  1) adsb.lol  (queried alongside adsb.fi by default) https://api.adsb.lol/v2/point/{lat}/{lon}/{radius_nm}
//     - open community data (ODbL licence), no API key needed today.
//     - The operators have said an API key (obtained by feeding data) may be
//       required in future.
//  2) airplanes.live           https://api.airplanes.live/v2/point/{lat}/{lon}/{radius_nm}
//     - 1 request per second limit; non-commercial use; their guide asks
//       users to request access, so this source may be refused.
//  3) adsb.fi open data (queried alongside adsb.lol by default) https://opendata.adsb.fi/api/v3/lat/{lat}/lon/{lon}/dist/{radius_nm}
//     - personal, non-commercial use; 1 request per second limit.
//  4) custom                   any URL that returns the same JSON (ADSB_CUSTOM_URL_FORMAT)
//
//  All return readsb-style JSON:
//    { "ac": [ { "hex":"406a7b", "flight":"BAW123  ", "r":"G-EUUU", "t":"A320",
//                "lat":51.5, "lon":-0.4, "alt_baro":34000 | "ground", "gs":450.2,
//                "track":270.1, "category":"A3", "dbFlags":0, "ownOp":"...",
//                "seen_pos":0.4, ... }, ... ], "now":..., "total":... }
//  Only the fields above are kept (everything else is filtered out while
//  streaming), and any of them may be missing.
//
//  In the default partnership mode, both feeds are queried independently.
//  Aircraft are joined by ICAO, newer position reports take precedence, and
//  missing facts are filled from the other feed. A failed feed does not prevent
//  results from the other feed from being used.
//  adsb.lol's global /v2/squawk endpoint is also polled alternately for 7500
//  (hijack) and 7700 (emergency), so alerts can be detected outside local range.
//
//  To use a different provider with the same JSON layout, only the URL format
//  below (or ADSB_CUSTOM_URL_FORMAT) has to change. A provider with a different
//  JSON layout needs changes in adsbIngestElement() in adsb_api.cpp only.
// ---------------------------------------------------------------------------
#pragma once
#include <stdint.h>
#include <time.h>
#include "config.h"

#if ADSB_MULTI_SOURCE_ENABLED
  #define ADSB_SOURCE_NAME  "adsb.lol + adsb.fi"
#elif ADSB_SOURCE == ADSB_SOURCE_ADSB_LOL
  #define ADSB_SOURCE_NAME  "adsb.lol"
  #define ADSB_URL_FORMAT   "https://api.adsb.lol/v2/point/%.4f/%.4f/%u"
#elif ADSB_SOURCE == ADSB_SOURCE_AIRPLANES_LIVE
  #define ADSB_SOURCE_NAME  "airplanes.live"
  #define ADSB_URL_FORMAT   "https://api.airplanes.live/v2/point/%.4f/%.4f/%u"
#elif ADSB_SOURCE == ADSB_SOURCE_ADSB_FI
  #define ADSB_SOURCE_NAME  "adsb.fi"
  #define ADSB_URL_FORMAT   "https://opendata.adsb.fi/api/v3/lat/%.4f/lon/%.4f/dist/%u"
#elif ADSB_SOURCE == ADSB_SOURCE_CUSTOM
  #define ADSB_SOURCE_NAME  "custom"
  #define ADSB_URL_FORMAT   ADSB_CUSTOM_URL_FORMAT
#else
  #error "ADSB_SOURCE in config.h must be 1, 2, 3 or 4."
#endif

struct AdsbStatus {
  bool     lastOk;              // last request succeeded
  bool     everOk;              // at least one request succeeded since boot
  int      lastHttpCode;
  uint16_t lastCount;           // aircraft in last successful response
  uint32_t lastSuccessMs;       // millis() of last success (0 = never)
  time_t   lastSuccessEpoch;    // wall-clock time of last success (0 = unknown)
  uint32_t failures;            // consecutive failures
  char     lastError[24];       // short text, e.g. "TIMEOUT"
};

void adsbInit();                         // start the download task
void adsbSetRange(uint16_t rangeNm);     // radar range; request radius follows it
void adsbRequestNow();                   // fetch as soon as allowed
void adsbGetStatus(AdsbStatus& out);     // thread-safe copy
