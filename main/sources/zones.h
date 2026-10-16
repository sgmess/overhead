#pragma once

// Timezones offered by the configuration page and the settings screen:
// newlib only understands POSIX rules, so each comes with its own.

typedef struct {
    const char *iana;  // the browser's name for it, to suggest one
    const char *label;
    const char *posix;
} zone_t;

extern const zone_t ZONES[];
extern const int ZONE_COUNT;

// The zone with this POSIX rule, or -1 (a rule typed in on the page)
int zone_find(const char *posix);

// The zone with this IANA name, "Europe/London", or -1
int zone_find_iana(const char *iana);
