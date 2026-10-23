#pragma once

#include <stdbool.h>
#include <time.h>

#include "aircraft.h"

// The weather at an airfield near the radar, from aviationweather.gov's
// METARs (free, no key): settings()->wx_station, or the nearest station
// reporting to the centre. Fetched every WEATHER_EVERY_S, and at once when
// the station or the centre changes. With it, the runway in use there, from
// the traffic: which way the aircraft low over it are landing and taking off.

#define WEATHER_EVERY_S (15 * 60)

typedef struct {
    bool valid;         // a METAR has come
    char station[5];    // "EGLL"
    char name[48];      // "London/Heathrow Intl"
    int wind_dir;       // degrees true, WIND_VARIABLE, or WIND_CALM
    int wind_kt, gust_kt; // gust 0 when none
    bool has_temp;
    int temp_c, dew_c;
    int qnh;            // hPa; 0 unknown
    char raw[160];      // the METAR as sent
    time_t obs;         // when it was observed
    int landing, departing; // runway numbers in use, 1..36; 0 unknown
    char detail[48];    // why there is none: "Looking for the nearest station", an error
} weather_t;

#define WIND_VARIABLE -1
#define WIND_CALM -2

void weather_start(void);

// The station or the centre changed: fetch again now. Safe from any task.
void weather_refresh(void);

// The latest. Safe from any task.
void weather_get(weather_t *out);

// Look at the latest positions for the runway in use. From the fetch task.
void weather_traffic(const aircraft_t *list, int n);
