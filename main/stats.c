#include "stats.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

// Distinct aircraft by ICAO address, in an open-addressed table in PSRAM. A
// busy day near Heathrow is a few thousand; past three quarters full, new
// ones go uncounted rather than slow every lookup.
#define TABLE_N 8192
#define TABLE_FULL (TABLE_N * 3 / 4)

// Readings past these are glitches, not records
#define ALT_MAX_FT 60000
#define GS_MAX_KT 750

typedef struct {
    uint32_t key;  // address + 1, so 0 is empty
    int8_t hour;   // the last local hour it was counted in
} seen_t;

static seen_t *s_table;
static SemaphoreHandle_t s_lock;
static stats_t s_stats;
static int s_day = -1; // tm_yday of the day being counted

// The slot for key, or for where it would go
static seen_t *slot(uint32_t key)
{
    uint32_t i = (key * 2654435761u) % TABLE_N;
    while (s_table[i].key && s_table[i].key != key) i = (i + 1) % TABLE_N;
    return &s_table[i];
}

static void record(stats_record_t *r, const aircraft_t *a, int value, time_t now)
{
    if (value <= r->value) return;
    strlcpy(r->name, ac_name(a), sizeof(r->name));
    r->value = value;
    r->at = now;
}

void stats_update(const aircraft_t *list, int n, double lat, double lon, int range_nm)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
        s_table = heap_caps_calloc(TABLE_N, sizeof(seen_t), MALLOC_CAP_SPIRAM);
    }
    if (!s_lock || !s_table) return;

    const time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    if (tm.tm_year < 120) return; // no clock yet: no day to count

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (tm.tm_yday != s_day) {
        // A new day, or the first count since starting
        const bool midnight = s_day >= 0;
        memset(s_table, 0, TABLE_N * sizeof(seen_t));
        memset(&s_stats, 0, sizeof(s_stats));
        struct tm start = tm;
        start.tm_hour = start.tm_min = start.tm_sec = 0;
        s_stats.since = midnight ? mktime(&start) : now;
        s_stats.valid = true;
        s_day = tm.tm_yday;
    }

    const double coslat = cos(lat * M_PI / 180.0);
    for (int i = 0; i < n; i++) {
        const aircraft_t *a = &list[i];
        const double dx = (a->lon - lon) * 60.0 * coslat, dy = (a->lat - lat) * 60.0;
        if (dx * dx + dy * dy > (double)range_nm * range_nm) continue;
        char *end;
        const unsigned long addr = strtoul(a->hex, &end, 16);
        if (end == a->hex || *end) continue; // "~": not an ICAO address

        seen_t *e = slot((uint32_t)addr + 1);
        if (!e->key) {
            if (s_stats.seen >= TABLE_FULL) continue;
            e->key = (uint32_t)addr + 1;
            e->hour = -1;
            s_stats.seen++;
            if (a->military) s_stats.military++;
        }
        if (e->hour != tm.tm_hour) {
            e->hour = tm.tm_hour;
            s_stats.hours[tm.tm_hour]++;
        }
        if (!a->on_ground && a->alt_ft != AC_ALT_UNKNOWN && a->alt_ft <= ALT_MAX_FT) {
            record(&s_stats.highest, a, a->alt_ft, now);
        }
        if (!a->on_ground && a->gs_kt > 0 && a->gs_kt <= GS_MAX_KT) {
            record(&s_stats.fastest, a, (int)lroundf(a->gs_kt), now);
        }
    }
    xSemaphoreGive(s_lock);
}

void stats_get(stats_t *out)
{
    if (!s_lock) {
        memset(out, 0, sizeof(*out));
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_stats;
    xSemaphoreGive(s_lock);
}
