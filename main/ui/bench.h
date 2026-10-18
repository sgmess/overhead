#pragma once

#include "sdkconfig.h"

#if CONFIG_LV_USE_SNAPSHOT
// For development, with LVGL's snapshot on in a local sdkconfig: count the
// frames the display draws between start and stop, and the time from the
// start of each to the end of its drawing. With the display lock held.
typedef struct {
    int frames;
    int avg_us, max_us;
    int busy_pct; // of the time measured, how much went on drawing
    int inv_areas;   // areas marked for redrawing
    int inv_kpx;     // and their size in thousands of pixels, overlaps counted twice
    int biggest_kpx; // the largest one
    int biggest_x, biggest_y, biggest_w, biggest_h;
} bench_t;
void bench_start(void);
void bench_stop(bench_t *out);
#endif
