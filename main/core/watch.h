#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "aircraft.h"

// The watchlist: entries separated by commas or spaces, each an aircraft
// type ("A388"), registration ("G-XLEA", with or without the dash),
// callsign ("RRR1") or ICAO address ("43c6f1"), any case; a trailing '*'
// matches what starts with it ("RRR*"). Whether a matches one, and which,
// as written but upper case, into why.
bool watch_match(const aircraft_t *a, const char *list, char *why, size_t n);
