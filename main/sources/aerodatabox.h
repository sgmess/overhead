#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#include "route.h"

// Flight status and aircraft details from AeroDataBox's direct API, with the
// key on the configuration page (settings()->adb_key); without one nothing
// here asks. Every answer costs credits, so only the aircraft someone picked
// is looked up: a flight for 2 (4 when the aircraft's flights don't
// have it and the callsign's are asked too), an aircraft for 1, up to
// settings()->adb_daily a day. Answers are kept for the session; a flight's
// times are asked for again after a while if it is still being looked at.

void adb_start(void);

// One end of a flight. Times are the airport's own, "10:42".
typedef struct {
    char sched[6];   // scheduled; "" unknown
    char best[6];    // the actual or estimated time, else the scheduled one
    bool live;       // best is an actual or estimated time
    int delay_min;   // best against sched, when both are known; negative is early
    char terminal[8];
    char gate[8];
} adb_end_t;

typedef struct {
    char number[12];  // "BA 117"
    char status[16];  // "En route", "Delayed"; "" unknown
    bool departed;    // has left the gate, as far as anyone knows
    bool arrived;
    bool diverted;
    route_t route;    // codes "" unknown; as route.c's, so describe.c treats both alike
    adb_end_t dep, arr;
} adb_flight_t;

typedef struct {
    char model[40]; // "Airbus A320neo"; "" unknown
    int built;      // year of its first flight or delivery; 0 unknown
    int seats;      // 0 unknown
} adb_aircraft_t;

// callsign's flight into out, false (and out empty) while it is being looked
// up, when nobody knows it, or without a key. With ask, a miss or an old
// answer queues a lookup; hex, the aircraft, finds it more often than the
// callsign does, and (lat, lon), where it is, picks the leg it is flying.
// Safe from any task.
bool adb_flight(const char *callsign, const char *hex, double lat, double lon, bool ask, adb_flight_t *out);

// hex's details into out, as adb_flight().
bool adb_aircraft(const char *hex, bool ask, adb_aircraft_t *out);

typedef struct {
    const char *state; // "off" (no key), "ok", "error", "limit" (the day's credits are spent)
    char detail[64];   // why, for "error" and "limit"
    int used_today;    // credits spent since local midnight, restarts and power cuts included
    int lookups;       // answered since the restart
} adb_status_t;

void adb_get_status(adb_status_t *out);

// For development, in a debug build: n requests to an endpoint that costs
// nothing, on a new connection each or kept (keep), timed, as JSON into out
esp_err_t adb_debug_timing(int n, bool keep, char *out, size_t size);
