#pragma once

// Airlines by the three-letter ICAO code that starts their callsigns: the
// IATA code that starts their flight numbers, and the name. A selection, the
// ones most often seen over Europe plus the large carriers elsewhere.

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    char icao[4];
    char iata[3]; // "" for none, or when its callsigns never match its flight numbers
    const char *name;
} airline_t;

// The airline whose code this is, "BAW", or NULL
const airline_t *airline_find(const char *icao);

// The airline a callsign belongs to: three letters and then a digit, so
// registrations such as GABCD never match. NULL when it is none of these.
const airline_t *airline_for_callsign(const char *callsign);

// The flight number for a callsign, "BA189" for BAW189, when the callsign is
// the airline's code and digits only: those are usually the flight number.
// Alphanumeric ones (BAW22B, EZY45GT) are assigned to avoid confusion on the
// radio and are not, so they give false.
bool airline_flight_number(const char *callsign, char *out, size_t n);
