#pragma once

#include <stdbool.h>
#include <stddef.h>

// Origin and destination by callsign, from adsb.im's route database (the one
// tar1090 uses). Answers are cached for the session.

void route_start(void);

#define ROUTE_PLACES_MAX 64
#define ROUTE_STOPS_MAX 4

typedef struct {
    char codes[40];                // "LHR-JFK", " ?" added when it doesn't fit the position
    char places[ROUTE_PLACES_MAX]; // where those airports are, "London|New York"
    bool plausible;                // the service thinks it fits the position
    int stops;                     // airports with a position: 0 unless all have one
    float lat[ROUTE_STOPS_MAX], lon[ROUTE_STOPS_MAX];
} route_t;

// The route for callsign into out, false (and out empty) while it is being
// looked up or when nobody knows it. A miss queues a lookup; (lat, lon) is
// where the aircraft is, which lets the service say whether the route fits.
// Safe from any task.
bool route_get(const char *callsign, double lat, double lon, route_t *out);

// How far an aircraft at (lat, lon) is along r: the share of its leg flown,
// in percent, and the nautical miles to that leg's end. False when r has no
// positions or doesn't fit.
bool route_progress(const route_t *r, double lat, double lon, int *percent, int *to_go_nm);
