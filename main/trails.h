#pragma once

#include <stdint.h>

#include "aircraft.h"

// Where each aircraft has been: its position reports over the last
// settings()->trail_s seconds, at most TRAIL_PTS of them, spaced evenly.

#define TRAIL_PTS 24

typedef struct {
    float lat, lon;
} trail_pt_t;

// Allocate the history, if trails are on. Before trails_update().
void trails_init(void);

// Add each aircraft's latest report. slot_out[i] is list[i]'s trail for
// trails_get(), or -1; it stays valid until the next call.
void trails_update(const aircraft_t *list, int count, int16_t *slot_out);

// A trail's points within the trail length, oldest first. Returns how many.
int trails_get(int slot, trail_pt_t out[TRAIL_PTS]);
