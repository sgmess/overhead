#include "stats.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_rom_crc.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "geo.h"

static const char *TAG = "stats";

// Distinct aircraft by ICAO address, in an open-addressed table in PSRAM. A
// busy day near Heathrow is a few thousand; past three quarters full, new
// ones go uncounted rather than slow every lookup.
#define TABLE_N 8192
#define TABLE_FULL (TABLE_N * 3 / 4)

// Airlines by the three letters their callsigns start with; a day near a
// big airport sees a hundred or so
#define AIRLINE_SLOTS 192

// Kept in flash every SAVE_US while something changed, and as the device
// restarts, in the next of SLOTS slots each time
#define PART_LABEL "stats"
#define SLOT_SIZE (32 * 1024)
#define SLOTS 8
#define SAVE_US (10 * 60 * 1000000LL)
#define SAVE_MAGIC 0x54415453 // "STAT"
#define SAVE_VERSION 3        // 3: types_t after the packed entries; 2, without, still loads

// Aircraft types (ICAO designators, "A388"): today's counts, and every type
// ever seen with the days it was seen on, for the rarest and the new
#define TYPE_SLOTS 128
#define BOOK_N 320
#define BOOK_LEARN_DAYS 3 // a type is only "new" once the book has this many days behind it

// Readings past these are glitches, not records
#define ALT_MAX_FT 60000
#define GS_MAX_KT 750

#define NO_HOUR 31      // seen_t.hour before the first count
#define AIRLINE_BIT 0x80 // in a packed entry: its airline has been counted
#define TYPE_BIT 0x40    // and its type

typedef struct {
    uint32_t key;  // address + 1, so 0 is empty
    uint8_t hour;  // the last local hour it was counted in, or NO_HOUR
    bool airline;  // its airline has been counted
    bool typed;    // its type has been counted
} seen_t;

typedef struct {
    char type[4]; // not terminated when four long
    uint16_t count;
} type_count_t;

typedef struct {
    char type[4];
    uint16_t days;  // days it was seen on
    uint16_t first; // the day first seen, as day_no()
    uint16_t last;  // and last
} book_entry_t;

typedef struct {
    type_count_t today[TYPE_SLOTS];
    book_entry_t book[BOOK_N];
    uint16_t book_since; // the day the book began, as day_no(); 0 empty
} types_t;

typedef struct {
    char icao[4];
    uint16_t count;
} airline_count_t;

// One day's counts
typedef struct {
    time_t since;
    time_t last;    // the last count, as of the last save
    int32_t seen, military;
    int32_t hours[24];
    stats_record_t highest, fastest;
    airline_count_t airlines[AIRLINE_SLOTS];
} today_t;

// The days before, newest first
typedef struct {
    struct {
        time_t day;   // its midnight
        int32_t seen;
        bool whole;   // counted from midnight to midnight
    } days[STATS_HISTORY_DAYS];
    int32_t n_days;
    time_t best_day;
    int32_t best_seen;
    int64_t total_seen; // the days before today
    int32_t total_days;
    time_t total_since; // 0 until the first day
} history_t;

static seen_t *s_table;
static types_t *s_types; // in PSRAM
static SemaphoreHandle_t s_lock;
static today_t s_today;
static history_t s_history;
static int s_day = -1; // tm_yday of the day being counted
static int s_year;     // and its tm_year
static int s_last_hour = -1;

typedef struct {
    uint32_t magic, version;
    uint32_t seq;        // the newest valid slot is the one to load
    int32_t year, yday;  // the day counted
    uint32_t n;          // packed entries after the header
    uint32_t sizes;      // sizeof(today_t) + sizeof(history_t) when written
    uint32_t crc;        // of what follows the crc
    today_t today;
    history_t history;
} save_hdr_t;

// Each entry the address in the top 24 bits, the hour and AIRLINE_BIT below
static uint32_t *s_packed;

_Static_assert(sizeof(save_hdr_t) + TABLE_FULL * sizeof(uint32_t) + sizeof(types_t) <= SLOT_SIZE,
               "a save must fit a slot");

static uint32_t s_seq;
static int s_slot = -1;      // the last slot written or loaded
static bool s_dirty;
static int64_t s_saved_us;

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
    s_dirty = true;
}

// Count one more aircraft for the airline callsign starts with, if it is
// an airline's: three letters and a digit. False when it isn't one.
static bool count_airline(const char *callsign)
{
    for (int k = 0; k < 3; k++) {
        if (callsign[k] < 'A' || callsign[k] > 'Z') return false;
    }
    if (callsign[3] < '0' || callsign[3] > '9') return false;
    for (int k = 0; k < AIRLINE_SLOTS; k++) {
        airline_count_t *c = &s_today.airlines[k];
        if (!c->icao[0]) memcpy(c->icao, callsign, 3); // a new one
        if (memcmp(c->icao, callsign, 3) == 0) {
            c->count++;
            return true;
        }
    }
    return true; // full: uncounted
}

// A local day as a number: days since 1970, from its midnight
static uint16_t day_no(time_t midnight_t)
{
    return (uint16_t)((midnight_t + 43200) / 86400); // noon: a clock change can't move it a day
}

static bool same_type(const char a[4], const char *b)
{
    return strncmp(a, b, 4) == 0 && (strlen(b) >= 4 || a[strlen(b)] == '\0');
}

// The feed's types for airport vehicles and towers, which aren't aircraft
static bool not_aircraft(const char *type)
{
    return strncmp(type, "GND", 4) == 0 || strncmp(type, "TWR", 4) == 0;
}

// Count one more aircraft of type today, and the day in the book. With the
// lock held. True when something changed.
static bool count_type(const char *type, uint16_t today)
{
    const size_t len = strlen(type);
    if (!len || len > 4) return false;
    if (not_aircraft(type)) return false;
    type_count_t *c = NULL;
    for (int k = 0; k < TYPE_SLOTS && !c; k++) {
        type_count_t *t = &s_types->today[k];
        if (!t->type[0]) strncpy(t->type, type, 4); // a new one today
        if (same_type(t->type, type)) c = t;
    }
    if (!c) return false; // full: uncounted
    c->count++;
    if (c->count > 1) return true; // the book has today already
    if (!s_types->book_since) s_types->book_since = today;
    for (int k = 0; k < BOOK_N; k++) {
        book_entry_t *b = &s_types->book[k];
        if (!b->type[0]) {
            strncpy(b->type, type, 4);
            b->days = 1;
            b->first = b->last = today;
            ESP_LOGI(TAG, "first %s", type);
            return true;
        }
        if (same_type(b->type, type)) {
            if (b->last != today) {
                b->days++;
                b->last = today;
            }
            return true;
        }
    }
    return true; // the book is full: today's count only
}

static time_t midnight(const struct tm *tm)
{
    struct tm start = *tm;
    start.tm_hour = start.tm_min = start.tm_sec = 0;
    start.tm_isdst = -1;
    return mktime(&start);
}

// Move a day's count into the history; with the lock held
static void close_day(const today_t *t, time_t day)
{
    if (!t->seen) return;
    history_t *h = &s_history;
    memmove(&h->days[1], &h->days[0], sizeof(h->days[0]) * (STATS_HISTORY_DAYS - 1));
    h->days[0].day = day;
    h->days[0].seen = t->seen;
    // From midnight, and still counting late that evening (it saves at least
    // hourly), so a day the device was off for part of doesn't pull the
    // average down
    h->days[0].whole = t->since == day && t->last >= day + 22 * 3600;
    if (h->n_days < STATS_HISTORY_DAYS) h->n_days++;
    if (t->seen > h->best_seen) {
        h->best_seen = t->seen;
        h->best_day = day;
    }
    h->total_seen += t->seen;
    h->total_days++;
    if (!h->total_since || t->since < h->total_since) h->total_since = t->since;
}

#if CONFIG_SPIRAM_XIP_FROM_PSRAM
static const esp_partition_t *part(void)
{
    return esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, PART_LABEL);
}

static uint32_t save_crc(const save_hdr_t *h, const uint32_t *packed, const types_t *types)
{
    uint32_t crc = esp_rom_crc32_le(0, (const uint8_t *)&h->today, sizeof(h->today) + sizeof(h->history));
    crc = esp_rom_crc32_le(crc, (const uint8_t *)packed, h->n * sizeof(uint32_t));
    return h->version >= 3 ? esp_rom_crc32_le(crc, (const uint8_t *)types, sizeof(*types)) : crc;
}

// The types after a slot's packed entries, for a version 3 save
static esp_err_t read_types(const esp_partition_t *p, size_t at, const save_hdr_t *h, types_t *types)
{
    if (h->version < 3) {
        memset(types, 0, sizeof(*types));
        return ESP_OK;
    }
    return esp_partition_read(p, at + sizeof(*h) + h->n * sizeof(uint32_t), types, sizeof(*types));
}
#endif

// What was last saved: today's counts back into the table, and the days
// before; a saved day that has passed goes into the history. With the lock
// held. Flash reads and writes need XIP, as for the OpenAIP cache
// (main/sources/openaip.c).
static void load(const struct tm *tm)
{
#if CONFIG_SPIRAM_XIP_FROM_PSRAM
    const esp_partition_t *p = part();
    if (!p || !s_packed || !s_types) return;
    save_hdr_t *h = heap_caps_malloc(sizeof(*h), MALLOC_CAP_SPIRAM); // too big for a stack
    types_t *types = heap_caps_malloc(sizeof(*types), MALLOC_CAP_SPIRAM);
    if (!h || !types) {
        heap_caps_free(h);
        heap_caps_free(types);
        return;
    }
    // The newest save that reads back whole: power lost part way through
    // one leaves the save before it
    uint32_t best_seq = 0, top_seq = 0;
    int best_slot = -1;
    for (int k = 0; k < SLOTS && (k + 1) * SLOT_SIZE <= (int)p->size; k++) {
        const size_t at = k * SLOT_SIZE;
        if (esp_partition_read(p, at, h, sizeof(*h)) != ESP_OK) continue;
        if (h->magic != SAVE_MAGIC || (h->version != SAVE_VERSION && h->version != 2) ||
            h->sizes != sizeof(today_t) + sizeof(history_t) || h->n > TABLE_FULL)
            continue;
        if (h->seq > top_seq) top_seq = h->seq;
        if (best_slot >= 0 && h->seq <= best_seq) continue;
        if (esp_partition_read(p, at + sizeof(*h), s_packed, h->n * sizeof(uint32_t)) != ESP_OK ||
            read_types(p, at, h, types) != ESP_OK || save_crc(h, s_packed, types) != h->crc) {
            ESP_LOGW(TAG, "save %u in slot %d unreadable", (unsigned)h->seq, k);
            continue;
        }
        best_seq = h->seq;
        best_slot = k;
    }
    if (best_slot >= 0) {
        s_seq = top_seq;    // past any unreadable one, so the next save is newest
        s_slot = best_slot; // the next save goes in the one after
        const size_t at = best_slot * SLOT_SIZE;
        // Again: a later candidate may have been read over it
        if (esp_partition_read(p, at, h, sizeof(*h)) != ESP_OK ||
            esp_partition_read(p, at + sizeof(*h), s_packed, h->n * sizeof(uint32_t)) != ESP_OK ||
            read_types(p, at, h, types) != ESP_OK || save_crc(h, s_packed, types) != h->crc) {
            ESP_LOGW(TAG, "saved counts unreadable");
        } else if (h->year == tm->tm_year && h->yday == tm->tm_yday) {
            s_history = h->history;
            s_today = h->today;
            *s_types = *types;
            for (uint32_t i = 0; i < h->n; i++) {
                const uint32_t key = (s_packed[i] >> 8) + 1;
                seen_t *e = slot(key);
                e->key = key;
                e->hour = s_packed[i] & 0x1f;
                e->airline = (s_packed[i] & AIRLINE_BIT) != 0;
                e->typed = (s_packed[i] & TYPE_BIT) != 0;
            }
            ESP_LOGI(TAG, "%d aircraft so far today, from before the restart", (int)s_today.seen);
        } else {
            // Another day's: it ended while the device was off. The book
            // carries on; that day's type counts don't.
            s_history = h->history;
            memcpy(s_types->book, types->book, sizeof(types->book));
            s_types->book_since = types->book_since;
            struct tm then;
            localtime_r(&h->today.since, &then);
            close_day(&h->today, midnight(&then));
            s_dirty = true;
        }
    }
    heap_caps_free(h);
    heap_caps_free(types);
#else
    (void)tm;
#endif
}

// One save at a time, held across the flash write too: a restart that came
// mid-write would otherwise find nothing left unsaved (save() clears s_dirty
// as it packs), skip its own save, and cut that write off
static SemaphoreHandle_t s_save_lock;

static void save_unlocked(void);

// Write the counts to the next slot
static void save(void)
{
    if (!s_save_lock) return;
    xSemaphoreTake(s_save_lock, portMAX_DELAY);
    save_unlocked();
    xSemaphoreGive(s_save_lock);
}

static void save_unlocked(void)
{
#if CONFIG_SPIRAM_XIP_FROM_PSRAM
    const esp_partition_t *p = part();
    if (!p || !s_packed || !s_types || !s_lock || s_day < 0) return;
    const int slots = (int)(p->size / SLOT_SIZE) < SLOTS ? (int)(p->size / SLOT_SIZE) : SLOTS;
    if (slots < 1) return;
    save_hdr_t *h = heap_caps_calloc(1, sizeof(*h), MALLOC_CAP_SPIRAM);
    types_t *types = heap_caps_malloc(sizeof(*types), MALLOC_CAP_SPIRAM);
    if (!h || !types) {
        heap_caps_free(h);
        heap_caps_free(types);
        return;
    }

    // Packed under the lock, written without it. Not for long at a restart:
    // the fetch task could be holding it.
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
        heap_caps_free(h);
        heap_caps_free(types);
        return;
    }
    *h = (save_hdr_t){
        .magic = SAVE_MAGIC,
        .version = SAVE_VERSION,
        .year = s_year,
        .yday = s_day,
        .sizes = sizeof(today_t) + sizeof(history_t),
        .today = s_today,
        .history = s_history,
    };
    for (int i = 0; i < TABLE_N && h->n < TABLE_FULL; i++) {
        const seen_t *e = &s_table[i];
        if (!e->key) continue;
        s_packed[h->n++] = (e->key - 1) << 8 | e->hour | (e->airline ? AIRLINE_BIT : 0) | (e->typed ? TYPE_BIT : 0);
    }
    *types = *s_types;
    s_dirty = false;
    xSemaphoreGive(s_lock);
    h->seq = ++s_seq;
    h->crc = save_crc(h, s_packed, types);

    // The header last: cut off part way, the slot is just missing
    s_slot = (s_slot + 1) % slots;
    const size_t at = s_slot * SLOT_SIZE;
    esp_err_t err = esp_partition_erase_range(p, at, SLOT_SIZE);
    if (err == ESP_OK) err = esp_partition_write(p, at + sizeof(*h), s_packed, h->n * sizeof(uint32_t));
    if (err == ESP_OK) err = esp_partition_write(p, at + sizeof(*h) + h->n * sizeof(uint32_t), types, sizeof(*types));
    if (err == ESP_OK) err = esp_partition_write(p, at, h, sizeof(*h));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "not saved: %s", esp_err_to_name(err));
        s_dirty = true; // still to save: the next turn, or the restart, tries again
    }
    s_saved_us = esp_timer_get_time();
    heap_caps_free(h);
    heap_caps_free(types);
#endif
}

// As the device restarts: an update, or a settings change that needs one
static void save_at_restart(void)
{
    // A save in progress takes well under a second; then save what it missed
    if (!s_save_lock || xSemaphoreTake(s_save_lock, pdMS_TO_TICKS(2000)) != pdTRUE) return;
    if (s_dirty) save_unlocked();
    xSemaphoreGive(s_save_lock);
}

void stats_update(const aircraft_t *list, int n, double lat, double lon, int range_nm)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
        s_save_lock = xSemaphoreCreateMutex();
        s_table = heap_caps_calloc(TABLE_N, sizeof(seen_t), MALLOC_CAP_SPIRAM);
        s_packed = heap_caps_malloc(TABLE_FULL * sizeof(uint32_t), MALLOC_CAP_SPIRAM);
        s_types = heap_caps_calloc(1, sizeof(types_t), MALLOC_CAP_SPIRAM);
        esp_register_shutdown_handler(save_at_restart);
    }
    if (!s_lock || !s_table || !s_types) return;

    const time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    if (tm.tm_year < 120) return; // no clock yet: no day to count

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (tm.tm_yday == s_day && tm.tm_hour != s_last_hour) s_dirty = true; // a save each hour, for today_t.last
    s_last_hour = tm.tm_hour;
    if (tm.tm_yday != s_day) {
        // A new day, or the first count since starting
        const bool started = s_day >= 0;
        if (started) {
            struct tm then;
            localtime_r(&s_today.since, &then);
            close_day(&s_today, midnight(&then));
        }
        memset(s_table, 0, TABLE_N * sizeof(seen_t));
        memset(&s_today, 0, sizeof(s_today));
        memset(s_types->today, 0, sizeof(s_types->today));
        s_today.since = started ? midnight(&tm) : now;
        s_day = tm.tm_yday;
        s_year = tm.tm_year;
        if (!started) load(&tm); // the first count since starting: carry on from the last save
        s_dirty = true;
    }
    s_today.last = now;

    const double coslat = cos(lat * M_PI / 180.0);
    for (int i = 0; i < n; i++) {
        const aircraft_t *a = &list[i];
        const double dx = lon_diff(a->lon, lon) * 60.0 * coslat, dy = (a->lat - lat) * 60.0;
        if (dx * dx + dy * dy > (double)range_nm * range_nm) continue;
        char *end;
        const unsigned long addr = strtoul(a->hex, &end, 16);
        if (end == a->hex || *end) continue; // "~": not an ICAO address

        seen_t *e = slot((uint32_t)addr + 1);
        if (!e->key) {
            if (s_today.seen >= TABLE_FULL) continue;
            e->key = (uint32_t)addr + 1;
            e->hour = NO_HOUR;
            s_today.seen++;
            if (a->military) s_today.military++;
        }
        if (e->hour != tm.tm_hour) {
            e->hour = tm.tm_hour;
            s_today.hours[tm.tm_hour]++;
            s_dirty = true; // every new aircraft passes here too
        }
        // Once each, when its callsign first shows: some come a little later
        if (!e->airline && a->callsign[0]) {
            e->airline = true;
            if (count_airline(a->callsign)) s_dirty = true;
        }
        // And its type, the same
        if (!e->typed && a->type[0]) {
            e->typed = true;
            if (count_type(a->type, day_no(midnight(&tm)))) s_dirty = true;
        }
        if (!a->on_ground && a->alt_ft != AC_ALT_UNKNOWN && a->alt_ft <= ALT_MAX_FT) {
            record(&s_today.highest, a, a->alt_ft, now);
        }
        if (!a->on_ground && a->gs_kt > 0 && a->gs_kt <= GS_MAX_KT) {
            record(&s_today.fastest, a, (int)lroundf(a->gs_kt), now);
        }
    }
    xSemaphoreGive(s_lock);

    if (s_dirty && esp_timer_get_time() - s_saved_us >= SAVE_US) save();
}

// Whether the book is too young to call a type new; with the lock held
static bool book_learning(uint16_t today)
{
    return !s_types->book_since || today - s_types->book_since < BOOK_LEARN_DAYS;
}

bool stats_new_type(const char *type)
{
    if (!s_lock || !s_types || !type[0] || s_day < 0) return false;
    bool fresh = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    struct tm tm;
    localtime_r(&s_today.since, &tm);
    const uint16_t today = day_no(midnight(&tm));
    if (!book_learning(today)) {
        for (int b = 0; b < BOOK_N && s_types->book[b].type[0]; b++) {
            const book_entry_t *e = &s_types->book[b];
            if (same_type(e->type, type)) {
                fresh = e->first == today;
                break;
            }
        }
    }
    xSemaphoreGive(s_lock);
    return fresh;
}

void stats_get(stats_t *out)
{
    memset(out, 0, sizeof(*out));
    if (!s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const today_t *t = &s_today;
    const history_t *h = &s_history;
    out->valid = s_day >= 0;
    out->since = t->since;
    out->seen = t->seen;
    out->military = t->military;
    for (int k = 0; k < 24; k++) out->hours[k] = t->hours[k];
    out->highest = t->highest;
    out->fastest = t->fastest;

    // The airlines with the most, by insertion into a short list
    for (int k = 0; k < AIRLINE_SLOTS && t->airlines[k].icao[0]; k++) {
        const airline_count_t *c = &t->airlines[k];
        int at = STATS_TOP_AIRLINES;
        while (at > 0 && c->count > out->airlines[at - 1].count) at--;
        if (at == STATS_TOP_AIRLINES) continue;
        memmove(&out->airlines[at + 1], &out->airlines[at],
                sizeof(out->airlines[0]) * (STATS_TOP_AIRLINES - 1 - at));
        memcpy(out->airlines[at].icao, c->icao, 4);
        out->airlines[at].count = c->count;
    }

    // Whole days in the week before today
    const time_t week_ago = t->since - 7 * 86400 - 3600; // an hour's slack for the clocks changing
    long long sum = 0;
    for (int k = 0; k < h->n_days && h->days[k].day >= week_ago; k++) {
        if (!h->days[k].whole) continue;
        sum += h->days[k].seen;
        out->week_days++;
    }
    if (out->week_days) out->week_avg = (int)((sum + out->week_days / 2) / out->week_days);

    // Types: the most today, and today's seen on the fewest days
    struct tm since_tm;
    localtime_r(&t->since, &since_tm);
    const uint16_t today = day_no(midnight(&since_tm)); // the day being counted
    for (int k = 0; s_types && k < TYPE_SLOTS && s_types->today[k].type[0]; k++) {
        const type_count_t *c = &s_types->today[k];
        if (not_aircraft(c->type)) continue; // counted by an older build
        stats_type_t st = {.count = c->count};
        memcpy(st.type, c->type, 4);
        for (int b = 0; b < BOOK_N && s_types->book[b].type[0]; b++) {
            const book_entry_t *e = &s_types->book[b];
            if (memcmp(e->type, c->type, 4) == 0) {
                st.days = e->days;
                st.first = (time_t)e->first * 86400 + 43200; // noon UTC that day: the right date anywhere
                if (e->first == e->last && e->days == 1 && !book_learning(today)) out->new_today++;
                break;
            }
        }
        out->types_today++;
        int at = STATS_TOP_TYPES;
        while (at > 0 && st.count > out->types[at - 1].count) at--;
        if (at < STATS_TOP_TYPES) {
            memmove(&out->types[at + 1], &out->types[at], sizeof(st) * (STATS_TOP_TYPES - 1 - at));
            out->types[at] = st;
        }
        if (!st.days) continue; // not in the book: it was full
        at = STATS_RARE_TYPES;
        while (at > 0 && (!out->rare[at - 1].days || st.days < out->rare[at - 1].days ||
                          (st.days == out->rare[at - 1].days && st.count < out->rare[at - 1].count)))
            at--;
        if (at < STATS_RARE_TYPES) {
            memmove(&out->rare[at + 1], &out->rare[at], sizeof(st) * (STATS_RARE_TYPES - 1 - at));
            out->rare[at] = st;
        }
    }
    for (int b = 0; s_types && b < BOOK_N && s_types->book[b].type[0]; b++) {
        if (!not_aircraft(s_types->book[b].type)) out->types_known++;
    }
    out->learning = !s_types || book_learning(today);

    out->best_day = h->best_day;
    out->best_seen = h->best_seen;
    if (t->seen > out->best_seen) {
        struct tm tm;
        localtime_r(&t->since, &tm);
        out->best_day = midnight(&tm);
        out->best_seen = t->seen;
    }
    out->total_seen = h->total_seen + t->seen;
    out->total_days = h->total_days + 1;
    out->total_since = h->total_since && h->total_since < t->since ? h->total_since : t->since;
    xSemaphoreGive(s_lock);
}
