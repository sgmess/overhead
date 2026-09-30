#pragma once

#include <stdbool.h>
#include <stdint.h>

#define AC_MAX 600
#define AC_ALT_UNKNOWN INT32_MIN

typedef struct {
    char hex[8];
    char callsign[10];
    char reg[12];
    char type[6];
    char squawk[6];
    double lat, lon;
    int32_t alt_ft;      // AC_ALT_UNKNOWN when missing
    int32_t vs_fpm;      // 0 when missing
    float gs_kt;         // < 0 when missing
    float track_deg;     // < 0 when missing
    int64_t pos_time_us; // esp_timer time the position was valid
    bool on_ground;
    bool emergency;
} aircraft_t;

// Best display name: callsign, then registration, then ICAO hex.
static inline const char *ac_name(const aircraft_t *a)
{
    if (a->callsign[0]) return a->callsign;
    if (a->reg[0]) return a->reg;
    return a->hex;
}
