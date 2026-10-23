#pragma once

#include <stdbool.h>
#include <time.h>

#include "aircraft.h"

// Traffic for the configuration page: the aircraft that appeared on the
// radar since local midnight (or since the device started, if later),
// counted once each, and a record of the days before. Saved to flash now and
// then and at a restart, so the day carries on after one.

#define STATS_TOP_AIRLINES 8 // a few more than the page shows: some airlines fly as two codes
#define STATS_HISTORY_DAYS 30
#define STATS_TOP_TYPES 8
#define STATS_RARE_TYPES 5

typedef struct {
    char name[10];  // callsign, registration or ICAO address
    int value;      // feet or knots
    time_t at;
} stats_record_t;

typedef struct {
    char icao[4];   // "BAW"
    int count;      // distinct aircraft flying as it
} stats_airline_t;

typedef struct {
    char type[5];   // ICAO designator, "A388"
    int count;      // distinct aircraft of it today
    int days;       // days it has been seen on, today's included
    time_t first;   // a time on the day it was first seen
} stats_type_t;

typedef struct {
    bool valid;     // false until the clock is set
    // Today
    time_t since;   // midnight, or when counting began
    int seen;       // distinct aircraft
    int military;
    int hours[24];  // distinct aircraft seen in each local hour
    stats_record_t highest, fastest; // value 0 while none
    stats_airline_t airlines[STATS_TOP_AIRLINES]; // most aircraft first; count 0 past the last
    stats_type_t types[STATS_TOP_TYPES];          // most aircraft first; count 0 past the last
    stats_type_t rare[STATS_RARE_TYPES];          // today's seen on fewest days, then fewest today
    int types_today;      // distinct types today
    int types_known;      // in the book: every type seen since it began
    int new_today;        // types seen today for the first time, once the book has learned
    bool learning;        // the book is under BOOK_LEARN_DAYS old: nothing counts as new yet
    // The days before
    int week_avg;   // aircraft a day over the whole days of the last 7; 0 when there are none
    int week_days;  // how many whole days that was
    time_t best_day; // midnight of the day with the most, today included
    int best_seen;  // and how many; 0 while there is none
    long long total_seen; // every day's count added up, today's included
    int total_days;
    time_t total_since;   // when counting first began
} stats_t;

// Count the aircraft in list within range_nm of (lat, lon). From the fetch task.
void stats_update(const aircraft_t *list, int n, double lat, double lon, int range_nm);

// A copy of the numbers. Safe from any task.
void stats_get(stats_t *out);

// Whether type was first seen today, after the days the type book takes to
// learn what is usual here. Safe from any task.
bool stats_new_type(const char *type);
