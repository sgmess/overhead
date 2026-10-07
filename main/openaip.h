#pragma once

#include <stdbool.h>
#include <stdint.h>

// Airspace and airfields around the radar centre from OpenAIP
// (https://www.openaip.net), which needs a free API key. Fetched by country
// once connected and refreshed daily, then handed to the radar to draw.

#define OPENAIP_RADIUS_NM 107 // the largest range plus a margin; OpenAIP allows up to 200 km

// How an airspace is drawn
typedef enum {
    AERO_ZONE,       // CTR, ATZ, MATZ: around an aerodrome, from the ground
    AERO_AREA,       // TMA, CTA: controlled, with a base above the ground
    AERO_RESTRICTED, // prohibited and restricted areas
    AERO_DANGER,
    AERO_MANDATORY,  // RMZ, TMZ
    AERO_KIND_COUNT,
} aero_kind_t;

typedef struct {
    float x, y; // NM east and north of the radar centre
} aero_pt_t;

typedef struct {
    uint32_t first; // index of its first outline point in aero_map_t.pts
    uint16_t n;     // points; the last joins back to the first
    uint8_t kind;   // aero_kind_t
} aero_space_t;

typedef struct {
    float x, y;
    int16_t runway_deg; // true heading of the main runway, or -1
    bool major;         // an airport rather than a strip or glider site
    char ident[11];     // ICAO code, or the start of the name
} aero_field_t;

typedef struct {
    aero_space_t *spaces;
    int n_spaces;
    aero_pt_t *pts;
    int n_pts;
    aero_field_t *fields;
    int n_fields;
} aero_map_t;

void aero_map_free(aero_map_t *map);

// Start the task that fetches, once connected, around the centre in
// settings(): at once and then daily, and ten minutes after a failure.
// Does nothing without a key or with both layers off.
void openaip_start(void);

typedef struct {
    const char *state; // "off", "loading", "ok" or "error"
    const char *detail;
    const char *countries; // ISO codes fetched, e.g. "GB"
    int airspaces, airfields;
    int age_s; // since the last successful fetch; -1 before one
} openaip_status_t;

void openaip_get_status(openaip_status_t *out);
