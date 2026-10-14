#pragma once

#include "lvgl.h"

// Airline logos, from logos.bin (tools/make_logos.py), drawn on white for
// the card's badge. They are their owners' trademarks.

#define LOGO_MAX_W 138
#define LOGO_MAX_H 40

#define LOGO_SLOTS 2 // the card and the big view can show different ones

// The logo for an airline's ICAO code, "BAW", or NULL. Each slot is one
// image, refilled, so its previous logo is gone; with the display lock held.
const lv_image_dsc_t *logo_image(const char *icao, int slot);
