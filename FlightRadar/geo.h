// geo.h - great-circle distance / bearing helpers (pure C++, no Arduino deps).
#pragma once

#define GEO_EARTH_RADIUS_NM 3440.065   // mean earth radius in nautical miles

// Wrap a longitude difference into [-180, +180] degrees (handles the
// +/-180 degree line correctly).
double geoWrapLonDeg(double deg);

// Haversine great-circle distance (nautical miles) and initial bearing
// (degrees true, 0=N, 90=E, 180=S, 270=W, range 0..360) from point 1 to
// point 2. Identical points give distance 0 and bearing 0 (no division by 0).
void geoDistanceBearing(double lat1, double lon1, double lat2, double lon2,
                        float* distNm, float* bearingDeg);

// Convert point 2 into local east/north offsets (nautical miles) from point 1.
void geoToEastNorth(double lat1, double lon1, double lat2, double lon2,
                    float* eastNm, float* northNm);
