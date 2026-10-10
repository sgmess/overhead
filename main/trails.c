#include "trails.h"

#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "settings.h"

static const char *TAG = "trails";

#define SLOTS AC_MAX
#define BUCKETS 1024 // a power of two, about twice SLOTS
#define FORGET_S 30  // after the trail would have aged out anyway

typedef struct {
    char hex[8];   // "" when free
    uint32_t seen_s;
    uint8_t n, head; // ring of n points; head is the next to write
    uint32_t t_s[TRAIL_PTS];
    trail_pt_t pts[TRAIL_PTS];
} trail_t;

static trail_t *s_trails;
static int16_t *s_buckets;

static uint32_t now_s(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000000);
}

static uint32_t hash(const char *s)
{
    uint32_t h = 2166136261u; // FNV-1a
    for (; *s; s++) h = (h ^ (uint8_t)*s) * 16777619u;
    return h;
}

void trails_init(void)
{
    if (settings()->trail_s <= 0) return;
    s_trails = heap_caps_calloc(SLOTS, sizeof(trail_t), MALLOC_CAP_SPIRAM);
    s_buckets = heap_caps_malloc(BUCKETS * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!s_trails || !s_buckets) {
        ESP_LOGE(TAG, "no memory for trails");
        free(s_trails);
        free(s_buckets);
        s_trails = NULL;
        s_buckets = NULL;
    }
}

void trails_update(const aircraft_t *list, int count, int16_t *slot_out)
{
    if (!s_trails) {
        for (int i = 0; i < count; i++) slot_out[i] = -1;
        return;
    }
    const int len_s = settings()->trail_s;
    const uint32_t now = now_s();
    const uint32_t spacing = len_s / TRAIL_PTS > 0 ? len_s / TRAIL_PTS : 1;

    // A fresh table each time, without the aircraft no longer reported: a
    // few hundred inserts, which beats deleting from open addressing
    memset(s_buckets, 0xff, BUCKETS * sizeof(int16_t));
    for (int k = 0; k < SLOTS; k++) {
        trail_t *t = &s_trails[k];
        if (!t->hex[0]) continue;
        if (now - t->seen_s > (uint32_t)(len_s + FORGET_S)) {
            t->hex[0] = '\0';
            continue;
        }
        uint32_t b = hash(t->hex) & (BUCKETS - 1);
        while (s_buckets[b] >= 0) b = (b + 1) & (BUCKETS - 1);
        s_buckets[b] = k;
    }

    int free_k = 0;
    for (int i = 0; i < count; i++) {
        const aircraft_t *a = &list[i];
        slot_out[i] = -1;
        if (!a->hex[0]) continue;

        uint32_t b = hash(a->hex) & (BUCKETS - 1);
        int k = -1;
        for (; s_buckets[b] >= 0; b = (b + 1) & (BUCKETS - 1)) {
            if (strcmp(s_trails[s_buckets[b]].hex, a->hex) == 0) {
                k = s_buckets[b];
                break;
            }
        }
        if (k < 0) {
            while (free_k < SLOTS && s_trails[free_k].hex[0]) free_k++;
            if (free_k == SLOTS) continue; // full; it gets one when another goes
            k = free_k;
            s_trails[k] = (trail_t){0};
            strlcpy(s_trails[k].hex, a->hex, sizeof(s_trails[k].hex));
            s_buckets[b] = k; // the free bucket the probe stopped at
        }

        trail_t *t = &s_trails[k];
        t->seen_s = now;
        slot_out[i] = k;
        const uint32_t pos_s = (uint32_t)(a->pos_time_us / 1000000);
        const uint32_t last_s = t->n ? t->t_s[(t->head + TRAIL_PTS - 1) % TRAIL_PTS] : 0;
        if (t->n && pos_s < last_s + spacing) continue;
        t->t_s[t->head] = pos_s;
        t->pts[t->head] = (trail_pt_t){(float)a->lat, (float)a->lon};
        t->head = (t->head + 1) % TRAIL_PTS;
        if (t->n < TRAIL_PTS) t->n++;
    }
}

int trails_get(int slot, trail_pt_t out[TRAIL_PTS])
{
    if (!s_trails || slot < 0 || slot >= SLOTS) return 0;
    const trail_t *t = &s_trails[slot];
    const uint32_t now = now_s(), len = settings()->trail_s;
    const uint32_t oldest = now > len ? now - len : 0;
    int n = 0;
    for (int j = 0; j < t->n; j++) {
        int idx = (t->head + TRAIL_PTS - t->n + j) % TRAIL_PTS;
        if (t->t_s[idx] >= oldest) out[n++] = t->pts[idx];
    }
    return n;
}
