#include "geo.h"
#include <math.h>

static const double kPi  = 3.14159265358979323846;
static const double kD2R = kPi / 180.0;
static const double kR2D = 180.0 / kPi;

double geoWrapLonDeg(double deg) {
  if (!isfinite(deg)) return 0.0;
  deg = fmod(deg, 360.0);
  if (deg > 180.0) deg -= 360.0;
  else if (deg < -180.0) deg += 360.0;
  return deg;
}

void geoDistanceBearing(double lat1, double lon1, double lat2, double lon2,
                        float* distNm, float* bearingDeg) {
  const double phi1 = lat1 * kD2R;
  const double phi2 = lat2 * kD2R;
  const double dphi = (lat2 - lat1) * kD2R;
  const double dlam = geoWrapLonDeg(lon2 - lon1) * kD2R;

  const double sdp = sin(dphi * 0.5);
  const double sdl = sin(dlam * 0.5);
  double a = sdp * sdp + cos(phi1) * cos(phi2) * sdl * sdl;
  if (a < 0.0) a = 0.0;
  if (a > 1.0) a = 1.0;
  const double c = 2.0 * atan2(sqrt(a), sqrt(1.0 - a));
  if (distNm) *distNm = (float)(GEO_EARTH_RADIUS_NM * c);

  if (bearingDeg) {
    const double y = sin(dlam) * cos(phi2);
    const double x = cos(phi1) * sin(phi2) - sin(phi1) * cos(phi2) * cos(dlam);
    double b = atan2(y, x) * kR2D;   // atan2(0,0) == 0: safe for identical points
    if (b < 0.0) b += 360.0;
    if (b >= 360.0) b -= 360.0;
    *bearingDeg = (float)b;
  }
}

void geoToEastNorth(double lat1, double lon1, double lat2, double lon2,
                    float* eastNm, float* northNm) {
  float d = 0.f, b = 0.f;
  geoDistanceBearing(lat1, lon1, lat2, lon2, &d, &b);
  const double br = b * kD2R;
  if (eastNm)  *eastNm  = (float)(d * sin(br));
  if (northNm) *northNm = (float)(d * cos(br));
}
