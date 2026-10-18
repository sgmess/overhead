#pragma once

#include "sdkconfig.h"

#if CONFIG_LV_USE_SNAPSHOT
// For development, with LVGL's snapshot on in a local sdkconfig: what the
// display did between start and stop. With the display lock held.
typedef struct {
    int frames;
    int avg_us, max_us; // a refresh, from its start to its end
    // Of the time measured, how much the display spent refreshing: drawing,
    // handing blocks to the panel and waiting for it. Not CPU use.
    int busy_pct;
    // Within that, inside the flush callback (with the adapter, where it
    // rotates and waits for a free frame buffer) and in LVGL's own wait for
    // a flush to finish; the rest of busy is drawing
    int flush_pct, wait_pct;
    int drawn_kpx; // pixels handed to the panel, in thousands: what was really drawn
    // Redraw requests: LVGL reports each before dropping duplicates, and
    // more while it sizes the draw buffer, so a count of requests, not areas
    // drawn. The largest shows when the whole screen was asked for.
    int requests;
    int biggest_x, biggest_y, biggest_w, biggest_h;
} bench_t;
void bench_start(void);
void bench_stop(bench_t *out);
#endif
