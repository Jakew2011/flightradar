// aircraft_route.h - temporary departure/arrival lookup by flight callsign.
#pragma once
#include <stdint.h>

enum AircraftRouteState : uint8_t {
  AIRCRAFT_ROUTE_IDLE,
  AIRCRAFT_ROUTE_LOADING,
  AIRCRAFT_ROUTE_READY,
  AIRCRAFT_ROUTE_NOT_FOUND,
  AIRCRAFT_ROUTE_FAILED
};

struct AircraftRouteResult {
  char icao[8];
  char callsign[9];
  char departure[5];
  char arrival[5];
  AircraftRouteState state;
};

void aircraftRouteInit();
void aircraftRouteRequest(const char* icao, const char* callsign);
bool aircraftRoutePoll(AircraftRouteResult& result);
AircraftRouteState aircraftRouteState();
