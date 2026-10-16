#pragma once

#include "sdkconfig.h"

// The device's own settings screen: everything that needs no typing, each
// change applied as it is made. A long press on the scope opens it. Both
// functions with the display lock held.

// The dark theme its controls are drawn with. Once, before any widgets, so
// the rest of the screen looks the same before and after it first opens.
void settings_ui_init(void);

void settings_ui_open(void);

#if CONFIG_LV_USE_SNAPSHOT
// For development: scroll the open settings screen up and down without a
// finger, counting the frames drawn, until stopped; then how many, and the
// time from the start of each to the end of its drawing
typedef struct {
    int frames;
    int avg_us, max_us;
} settings_ui_bench_t;
void settings_ui_debug_scroll_start(void);
void settings_ui_debug_scroll_stop(settings_ui_bench_t *out);
#endif
