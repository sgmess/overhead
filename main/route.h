#pragma once

#include <stdbool.h>
#include <stddef.h>

// Origin and destination by callsign, from adsb.im's route database (the one
// tar1090 uses). Answers are cached for the session.

void route_start(void);

// The route for callsign as "LHR-JFK" into out, " ?" added when it doesn't
// fit the position, or "" while it is being
// looked up or when nobody knows it. A miss queues a lookup; (lat, lon) is
// where the aircraft is, which lets the service say whether the route fits.
// Safe from any task.
void route_get(const char *callsign, double lat, double lon, char *out, size_t n);
