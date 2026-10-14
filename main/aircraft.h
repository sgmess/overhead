#pragma once

#include <stdbool.h>
#include <stdint.h>

#define AC_MAX 600
#define AC_ALT_UNKNOWN INT32_MIN

// What sort of aircraft, from the ADS-B emitter category, as bits for the
// display filter (settings_t.show_kinds)
#define AC_KIND_LARGE 1 // A3-A5: airliners, bizjets and up
#define AC_KIND_LIGHT 2 // A1, A2, B1, B4: light aircraft, gliders, microlights
#define AC_KIND_HELI 4  // A7
#define AC_KIND_OTHER 8 // anything else, or no category sent
#define AC_KIND_ALL 15

// Why an aircraft is in an emergency, from its squawk or the feed's field
typedef enum {
    AC_EMERG_NONE,
    AC_EMERG_GENERAL, // 7700
    AC_EMERG_RADIO,   // 7600, or "nordo"
    AC_EMERG_HIJACK,  // 7500, or "unlawful"
    AC_EMERG_MEDICAL, // "lifeguard"
    AC_EMERG_FUEL,    // "minfuel"
    AC_EMERG_DOWNED,
} ac_emerg_t;

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
    bool emergency;      // emerg != AC_EMERG_NONE
    uint8_t emerg;       // ac_emerg_t
    bool military;       // readsb's dbFlags bit 0, from the feed's aircraft database
    bool heli;           // emitter category A7, rotorcraft
    uint8_t kind;        // one AC_KIND_* bit
} aircraft_t;

// Best display name: callsign, then registration, then ICAO hex.
static inline const char *ac_name(const aircraft_t *a)
{
    if (a->callsign[0]) return a->callsign;
    if (a->reg[0]) return a->reg;
    return a->hex;
}
