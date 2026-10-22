// train.h - live National Rail departure board for one station, fetched in
// the background from the Rail Data Marketplace's Live Departure Board API
// (REST/JSON, the current replacement for the retired OpenLDBWS SOAP
// service), shown on the "Switch mode" screen in place of the radar.
// Station CRS code is in g_settings (settings.h), changeable on-device and
// from the web setup portal, so no reflash is needed to point this at a
// different station.
#pragma once
#include <stdint.h>
#include "config.h"

// One array size for both "how many rows to ask the API for" and "how many
// rows this firmware has room to store/draw" - config.h's TRAIN_MAX_ROWS is
// the single place to change that.
#define TRAIN_MAX_SERVICES TRAIN_MAX_ROWS

struct TrainService {
  char std[6];           // scheduled departure time, "HH:MM"
  char etd[20];           // "On time" / "Delayed" / "Cancelled" / revised "HH:MM"
  char destination[48];
  char platform[4];       // "" if not yet allocated by the signaller
};

struct TrainStatus {
  bool     lastOk;
  int      lastHttpCode;
  uint16_t count;
  uint32_t lastSuccessMs;
  char     lastError[40];
  char     locationName[48];   // station name the board is actually showing
  char     nrccMessage[160];   // first disruption notice, if any ("" if none)
};

void     trainInit();                    // call once from setup()
void     trainSetActive(bool active);    // only polls the API while the train screen is shown
void     trainRequestNow();              // force an immediate refresh (e.g. switching stations)
void     trainGetStatus(TrainStatus& out);
uint16_t trainGetServices(TrainService* out, uint16_t maxOut);
bool     trainIsConfigured();            // true once config.h has an OPENLDBWS_API_KEY
