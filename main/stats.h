#pragma once

#include <stdbool.h>
#include <time.h>

#include "aircraft.h"

// Today's traffic, for the configuration page: the aircraft that appeared on
// the radar since local midnight (or since the device started, if later),
// counted once each. Kept in RAM, so a restart starts again.

typedef struct {
    char name[10];  // callsign, registration or ICAO address
    int value;      // feet or knots
    time_t at;
} stats_record_t;

typedef struct {
    bool valid;     // false until the clock is set
    time_t since;   // midnight, or when counting began
    int seen;       // distinct aircraft
    int military;
    int hours[24];  // distinct aircraft seen in each local hour
    stats_record_t highest, fastest; // value 0 while none
} stats_t;

// Count the aircraft in list within range_nm of (lat, lon). From the fetch task.
void stats_update(const aircraft_t *list, int n, double lat, double lon, int range_nm);

// A copy of today's numbers. Safe from any task.
void stats_get(stats_t *out);
