#pragma once

#include "lvgl.h"

// Country flags, from flags.bin (tools/make_flags.py; Twemoji, CC-BY 4.0).

#define FLAG_SLOTS 2 // the card and the big view can show different ones

// The flag for a two-letter code, "GB", or NULL. Each slot is one image,
// refilled, so its previous flag is gone; with the display lock held.
const lv_image_dsc_t *flag_image(const char *iso, int slot);
