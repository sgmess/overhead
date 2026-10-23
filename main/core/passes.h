#pragma once

#include <time.h>

#include "aircraft.h"

// The aircraft that passed overhead: within the overhead alert's distance
// and below its ceiling (whether the alert is on or not), each pass logged
// once, as it was at its closest. Every pass is kept in flash as it ends:
// in the passes partition (16,384), or on a device updated only over WiFi
// since that was added, the end of aipcache (12,288), the oldest going
// first. So a restart loses nothing. A device with neither keeps today's in
// RAM alone.

#define PASSES_MAX 200 // today's, in RAM: what a device without a flash log shows

typedef struct {
    char name[10];   // callsign, registration or ICAO address
    char type[6];
    char reg[12];
    int alt_ft;      // at its closest
    float dist_nm;   // its closest
    time_t at;       // when
} pass_t;

// Look at the latest positions, around (lat, lon). From the fetch task.
void passes_update(const aircraft_t *list, int n, double lat, double lon);

// Today's passes from RAM into out, those overhead now first, then newest
// first; how many. For a device without a flash log (see passes_since()).
// Safe from any task.
int passes_get(pass_t *out, int max);

typedef struct {
    int count;     // passes in the flash log; 0 without one
    time_t oldest; // the first of them
    int capacity;  // how many it holds
} passes_log_t;

void passes_log_info(passes_log_t *out);

// Those overhead now, their passes not over yet, as far as they have got;
// how many. Safe from any task.
int passes_active(pass_t *out, int max);

// The logged passes since since, newest first by time, after those overhead
// now; how many. Today's alone without a flash log. Every slot of the log is
// read and sorted, so a gap or a pass logged late is still found: a fraction
// of a second for a full log. Safe from any task.
int passes_since(time_t since, pass_t *out, int max);

// Each logged pass, oldest first by time, to each(p, ctx), for a download.
// The lock is held only while a sector or a record is read, so a slow
// each() holds nothing up.
void passes_each(void (*each)(const pass_t *p, void *ctx), void *ctx);
