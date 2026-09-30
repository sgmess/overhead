#pragma once

#include <stdbool.h>

#include "aircraft.h"

// All functions except radar_ui_range_nm() must be called with the
// display lock held (bsp_display_lock).

void radar_ui_create(double center_lat, double center_lon, void (*on_range_change)(void));

// Replace the displayed traffic with a fresh snapshot (copied).
void radar_ui_set_aircraft(const aircraft_t *list, int count, const char *feed);

// Status line shown under the range, e.g. "Connecting to WiFi".
void radar_ui_set_status(const char *text);

// Current range in NM. Safe to call without the lock.
int radar_ui_range_nm(void);
