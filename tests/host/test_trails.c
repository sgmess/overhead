// main/core/trails.c and geo.h on the PC: a report from before boot, and
// longitudes across the 180th meridian.

#include "fake.h"
#include "geo.h"
#include "trails.c"

#include <math.h>

int main(void)
{
    printf("Trails\n");
    g_set.trail_s = 120;
    trails_init();
    aircraft_t a = {.hex = "4ca123", .lat = 51, .lon = -0.4};
    int16_t slot;
    trail_pt_t pts[TRAIL_PTS];
    // 8 s after boot, a position 40 s old: from before boot
    g_now_us = 8000000;
    a.pos_time_us = g_now_us - 40000000;
    trails_update(&a, 1, &slot);
    // then a fresh one every 5 s for 230 s
    for (int t = 13; t <= 238; t += 5) {
        g_now_us = (int64_t)t * 1000000;
        a.pos_time_us = g_now_us - 1000000;
        a.lon += 0.01;
        trails_update(&a, 1, &slot);
    }
    const int n = trails_get(slot, pts);
    CHECK(n == TRAIL_PTS, "a report from before boot doesn't hold off the rest: %d points", n);

    printf("The 180th meridian\n");
    CHECK(fabs(lon_diff(-179.9, 179.9) - 0.2) < 1e-9, "-179.9 from 179.9: 0.2 degrees east");
    CHECK(fabs(lon_diff(179.9, -179.9) + 0.2) < 1e-9, "179.9 from -179.9: 0.2 degrees west");
    CHECK(fabs(lon_diff(-0.5, -0.4) + 0.1) < 1e-9, "near Greenwich, as it was");
    CHECK(fabs(lon_diff(170, -170) + 20) < 1e-9, "170 from -170: 20 degrees west");
    return fake_done();
}
