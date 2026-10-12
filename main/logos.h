#pragma once

#include "lvgl.h"

// Airline logos, from logos.bin (tools/make_logos.py), drawn on white for
// the card's badge. They are their owners' trademarks.

#define LOGO_MAX_W 138
#define LOGO_MAX_H 40

// The logo for an airline's ICAO code, "BAW", or NULL. Always the same
// image, refilled, so the previous logo is gone; with the display lock held.
const lv_image_dsc_t *logo_image(const char *icao);
