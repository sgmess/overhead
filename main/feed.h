#pragma once

#include "aircraft.h"
#include "esp_err.h"

// Put cJSON's allocations in PSRAM. Call before anything parses JSON.
void feed_init(void);

// Fetch every aircraft within radius_nm of (lat, lon) into out[0..max).
// On failure the next call switches to the other feed.
esp_err_t feed_fetch(double lat, double lon, int radius_nm,
                     aircraft_t *out, int max, int *count);

// Short name of the feed used by the last successful fetch.
const char *feed_name(void);
