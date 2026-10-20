// main/sources/feed.c on the PC: a feed answer with numbers out of range
// or infinite (cJSON reads 1e999 as infinity), through feed_fetch().

#include "fake.h"
#include "feed.c"

#include <stdlib.h>

static const char ANSWER[] = "{\"aircraft\":["
    // fine
    "{\"hex\":\"400aaa\",\"flight\":\"BAW1    \",\"lat\":51.5,\"lon\":-0.4,\"alt_baro\":3000,\"gs\":200,"
    "\"track\":90,\"baro_rate\":500,\"seen_pos\":2,\"year\":\"2001\"},"
    // no usable position: dropped
    "{\"hex\":\"400bad\",\"lat\":1e999,\"lon\":0},"
    "{\"hex\":\"400bad\",\"lat\":91,\"lon\":0},"
    "{\"hex\":\"400bad\",\"lat\":51,\"lon\":-181},"
    "{\"hex\":\"400bad\",\"lat\":51,\"lon\":-1e999},"
    // a position, and everything else out of range: unknown
    "{\"hex\":\"400bbb\",\"lat\":51.4,\"lon\":-0.5,\"alt_baro\":1e999,\"gs\":1e999,\"track\":400,"
    "\"baro_rate\":-1e999,\"seen_pos\":1e999,\"dbFlags\":1e300,\"year\":\"99999999999999999999\"},"
    // on the ground
    "{\"hex\":\"400ccc\",\"lat\":51.47,\"lon\":-0.45,\"alt_baro\":\"ground\",\"gs\":12}"
    "]}";

int main(void)
{
    printf("Feed numbers\n");
    g_set.feed = 1; // adsb.fi: "aircraft"
    g_http = (struct fake_http){.body = ANSWER, .status = 200};
    aircraft_t ac[8];
    int n = 0;
    const esp_err_t err = feed_fetch(51.47, -0.45, 25, ac, 8, &n);
    CHECK(err == ESP_OK && n == 3, "out of range positions dropped: %d of 7 kept", n);
    if (n != 3) return fake_done();

    const aircraft_t *a = &ac[0], *b = &ac[1], *g = &ac[2];
    CHECK(strcmp(a->hex, "400aaa") == 0 && a->alt_ft == 3000 && a->gs_kt == 200 && a->track_deg == 90 &&
              a->vs_fpm == 500 && a->year == 2001 && a->pos_time_us == g_now_us - 2000000,
          "a good one is read as it is");
    CHECK(b->alt_ft == AC_ALT_UNKNOWN, "altitude 1e999: unknown");
    CHECK(b->gs_kt == -1, "speed 1e999: unknown");
    CHECK(b->track_deg == -1, "track 400: unknown");
    CHECK(b->vs_fpm == 0, "climb -1e999: level");
    CHECK(b->pos_time_us == g_now_us, "position age 1e999: now");
    CHECK(!b->military, "flags 1e300: none");
    CHECK(b->year == 0, "year 99999999999999999999: unknown");
    CHECK(g->on_ground && g->alt_ft == 0, "\"ground\": on the ground");
    return fake_done();
}
