#pragma once

#include <stdint.h>

// The country an aircraft is registered in, from the block of 24-bit ICAO
// addresses each state is allocated (ICAO Annex 10, Volume III, Table 9-1).

typedef struct {
    uint32_t first, last;
    char iso[3]; // "GB", which names its flag in flags.bin
    const char *name;
} country_t;

extern const country_t COUNTRIES[];
extern const int COUNTRY_COUNT;

// The country for an address as the feeds give it, "4010ee", or NULL: an
// unallocated block, or an address that isn't an ICAO one ("~" first)
const country_t *country_for_hex(const char *hex);
