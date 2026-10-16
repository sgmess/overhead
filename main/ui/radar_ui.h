#pragma once

#include <stdbool.h>

#include "aircraft.h"
#include "battery.h"
#include "openaip.h"
#include "sdkconfig.h"
#include "settings.h"

// All functions except radar_ui_range_nm() must be called with the
// display lock held (board_lock).

void radar_ui_create(double center_lat, double center_lon, void (*on_range_change)(void));

// Replace the displayed traffic with a fresh snapshot (copied).
void radar_ui_set_aircraft(const aircraft_t *list, int count, const char *feed);

// Status line shown under the range, e.g. "Connecting to WiFi".
void radar_ui_set_status(const char *text);

// Battery indicator, shown where the layout has room for it (the Tab5's
// top-right corner); ignored on the round boards, which have no battery.
void radar_ui_set_battery(const battery_status_t *st);

// Airspace and airfields to draw under the traffic; takes ownership of map
// and frees the one before.
void radar_ui_set_aero(aero_map_t *map);

// Show how to join the setup portal's network, or hide that with NULL.
void radar_ui_show_setup(const char *ssid, const char *url);

// The settings changed (old is what ran before): apply what isn't simply
// read as it draws.
void radar_ui_apply_settings(const settings_t *old);

// A new centre: the traffic and the airspace go until fetched for it.
void radar_ui_set_centre(double lat, double lon);

// Open the card of the aircraft with this ICAO address, as a tap would; NULL
// closes it. With the display lock held.
void radar_ui_select(const char *hex);

#if CONFIG_LV_USE_SNAPSHOT
// For development: the aircraft with this address squawks 7700 until
// called again; NULL or "" ends it. With the display lock held.
void radar_ui_debug_emergency(const char *hex);

// For development: 0 the scope, 1 the big view, 2 the list (round boards),
// as swipes would.
void radar_ui_debug_view(int view);
#endif

// Current range in NM. Safe to call without the lock.
int radar_ui_range_nm(void);
