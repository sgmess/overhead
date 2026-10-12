#pragma once

#include "lvgl.h"

// Country flags, from flags.bin (tools/make_flags.py; Twemoji, CC-BY 4.0).

// The flag for a two-letter code, "GB", or NULL. Always the same image,
// refilled, so the previous flag is gone; with the display lock held.
const lv_image_dsc_t *flag_image(const char *iso);
