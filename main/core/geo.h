#pragma once

// lon - lon0 in degrees, the short way round: -180 to 180, so an aircraft
// just across the 180th meridian is near, not most of the world away
static inline double lon_diff(double lon, double lon0)
{
    double d = lon - lon0;
    if (d > 180.0) d -= 360.0;
    else if (d < -180.0) d += 360.0;
    return d;
}
