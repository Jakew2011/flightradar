// aircraft_photo.h - temporary aircraft photo lookup from Planespotters.net.
#pragma once
#include <stddef.h>
#include <stdint.h>

enum AircraftPhotoState : uint8_t {
  AIRCRAFT_PHOTO_IDLE,
  AIRCRAFT_PHOTO_LOADING,
  AIRCRAFT_PHOTO_READY,
  AIRCRAFT_PHOTO_NOT_FOUND,
  AIRCRAFT_PHOTO_FAILED
};

struct AircraftPhotoResult {
  char icao[8];
  uint8_t* jpeg;
  size_t jpegLength;
  AircraftPhotoState state;
};

void aircraftPhotoInit();
void aircraftPhotoRequest(const char* icao, const char* registration);  // empty ICAO clears the request
bool aircraftPhotoPoll(AircraftPhotoResult& result);  // caller owns result.jpeg
AircraftPhotoState aircraftPhotoState();
