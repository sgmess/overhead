#include "passes.h"

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
#include "settings.h"

static const char *TAG = "passes";

#define ACTIVE_N 24              // overhead at once; more go unlogged
#define GONE_US (60 * 1000000LL) // out of the circle this long, and the pass is over

// The log: its own partition, or on a device without one (updated only
// over WiFi since it was added) the end of the OpenAIP cache, which the map
// leaves free (openaip.c keeps below AIPCACHE_LOG_AT)
#define LOG_LABEL "passes"
#define AIPCACHE_LABEL "aipcache"
#define AIPCACHE_LOG_AT (640 * 1024)
#define SECTOR 4096

// A pass in flash: 32 bytes, so 128 a sector. Written once; a sector is
// erased as the ring comes round to it.
typedef struct {
    uint32_t seq;   // 1 up; 0xffffffff where erased
    uint32_t at;
    uint16_t alt_ft;
    uint16_t crc;   // CRC-16 of the record with this 0
    char type[4];   // these three not terminated when full
    char name[8];
    char reg[7];
    uint8_t dist_10; // tenths of a NM
} rec_t;
_Static_assert(sizeof(rec_t) == 32, "a record is 32 bytes");
#define PER_SECTOR (SECTOR / (int)sizeof(rec_t))

typedef struct {
    char hex[8]; // "" when free
    pass_t p;
    int64_t seen_us; // last inside
} active_t;

// In PSRAM
static pass_t *s_log; // today's, a ring, s_next the oldest once full
static int s_n, s_next;
static active_t *s_active;
static rec_t *s_sector; // a sector's records, read or about to be written
static int s_day = -1;  // tm_yday the RAM log is for
static SemaphoreHandle_t s_lock;

// The flash log; s_recs 0 without one. Under s_lock.
static const esp_partition_t *s_part;
static size_t s_base;     // its offset in the partition
static int s_recs;        // records it holds
static int s_head;        // the next to write
static uint32_t s_seq;    // the newest's
static int s_count;       // valid records
static time_t s_oldest;

// ---------------------------------------------------------------- records

static uint16_t rec_crc(const rec_t *r)
{
    rec_t c = *r;
    c.crc = 0;
    return esp_rom_crc16_le(0, (const uint8_t *)&c, sizeof(c));
}

// A record that was written whole: anything else is erased flash, one cut
// off by a power cut, or what the OpenAIP map left there
static bool rec_ok(const rec_t *r)
{
    return r->seq != 0xffffffff && r->seq && r->at > 1700000000 && r->at < 4102444800u && r->crc == rec_crc(r);
}

// Copy up to n chars, stopping at a terminator
static void copy_field(char *dst, size_t dst_n, const char *src, size_t n)
{
    size_t k = 0;
    while (k < n && k + 1 < dst_n && src[k]) {
        dst[k] = src[k];
        k++;
    }
    dst[k] = '\0';
}

// src into a field of n, zero-filled, not terminated when it fills it
static void put_field(char *dst, size_t n, const char *src)
{
    memset(dst, 0, n);
    memcpy(dst, src, strnlen(src, n));
}

static void to_pass(const rec_t *r, pass_t *p)
{
    *p = (pass_t){.alt_ft = r->alt_ft, .dist_nm = r->dist_10 / 10.0f, .at = (time_t)r->at};
    copy_field(p->type, sizeof(p->type), r->type, sizeof(r->type));
    copy_field(p->name, sizeof(p->name), r->name, sizeof(r->name));
    copy_field(p->reg, sizeof(p->reg), r->reg, sizeof(r->reg));
}

static void to_rec(const pass_t *p, rec_t *r)
{
    *r = (rec_t){.at = (uint32_t)p->at};
    r->alt_ft = p->alt_ft < 0 ? 0 : p->alt_ft > 65535 ? 65535 : (uint16_t)p->alt_ft;
    const float d = p->dist_nm * 10;
    r->dist_10 = d > 255 ? 255 : (uint8_t)lroundf(d);
    put_field(r->type, sizeof(r->type), p->type);
    put_field(r->name, sizeof(r->name), p->name);
    put_field(r->reg, sizeof(r->reg), p->reg);
}

// ---------------------------------------------------------------- the flash log

#if CONFIG_SPIRAM_XIP_FROM_PSRAM // flash reads and writes from tasks with PSRAM stacks
static esp_err_t read_sector(int sector, rec_t *out)
{
    return esp_partition_read(s_part, s_base + (size_t)sector * SECTOR, out, SECTOR);
}

// Find the log and where it got to; with the lock held
static void log_open(void)
{
    s_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, LOG_LABEL);
    s_base = 0;
    if (!s_part) {
        s_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, AIPCACHE_LABEL);
        s_base = AIPCACHE_LOG_AT;
        if (s_part && s_part->size <= s_base + SECTOR) s_part = NULL;
    }
    if (!s_part) return;
    const int sectors = (int)((s_part->size - s_base) / SECTOR);
    s_recs = sectors * PER_SECTOR;
    int newest = -1;
    for (int sct = 0; sct < sectors; sct++) {
        if (read_sector(sct, s_sector) != ESP_OK) continue;
        for (int k = 0; k < PER_SECTOR; k++) {
            const rec_t *r = &s_sector[k];
            if (!rec_ok(r)) continue;
            s_count++;
            if (!s_oldest || (time_t)r->at < s_oldest) s_oldest = r->at;
            if (r->seq > s_seq) {
                s_seq = r->seq;
                newest = sct * PER_SECTOR + k;
            }
        }
    }
    s_head = (newest + 1) % s_recs;
    // Written after the newest, the slot has to be erased: if it isn't (a
    // power cut part way through a write, or the map's old data), start on
    // the next sector, which is erased before its first write
    rec_t slot;
    if (s_head % PER_SECTOR &&
        esp_partition_read(s_part, s_base + (size_t)s_head * sizeof(rec_t), &slot, sizeof(slot)) == ESP_OK) {
        const uint8_t *b = (const uint8_t *)&slot;
        bool erased = true;
        for (size_t k = 0; k < sizeof(slot) && erased; k++) erased = b[k] == 0xff;
        if (!erased) s_head = (s_head / PER_SECTOR + 1) % sectors * PER_SECTOR;
    }
    ESP_LOGI(TAG, "%d passes in %s, %d KB, since %lld", s_count, s_part->label, (int)((s_part->size - s_base) / 1024),
             (long long)s_oldest);
}

// The earliest time in the log, or 0; with the lock held. A scan of it all:
// passes are written as they end, not in the order of their times, and a
// power cut can leave gaps. Only after an erase, every 128 passes.
static time_t oldest_at(void)
{
    time_t oldest = 0;
    for (int sct = 0; sct < s_recs / PER_SECTOR; sct++) {
        if (read_sector(sct, s_sector) != ESP_OK) continue;
        for (int k = 0; k < PER_SECTOR; k++) {
            const rec_t *r = &s_sector[k];
            if (rec_ok(r) && (!oldest || (time_t)r->at < oldest)) oldest = r->at;
        }
    }
    return oldest;
}

// p into the log; with the lock held
static void log_write(const pass_t *p)
{
    if (!s_recs) return;
    const int sector = s_head / PER_SECTOR;
    if (s_head % PER_SECTOR == 0) {
        // Round to an old sector: what it held goes, and the oldest is then
        // the first of the sector after it
        if (read_sector(sector, s_sector) == ESP_OK) {
            for (int k = 0; k < PER_SECTOR; k++) s_count -= rec_ok(&s_sector[k]);
        }
        if (esp_partition_erase_range(s_part, s_base + (size_t)sector * SECTOR, SECTOR) != ESP_OK) return;
        s_oldest = oldest_at();
    }
    rec_t r;
    to_rec(p, &r);
    r.seq = ++s_seq;
    r.crc = rec_crc(&r);
    if (esp_partition_write(s_part, s_base + (size_t)s_head * sizeof(rec_t), &r, sizeof(r)) != ESP_OK) {
        ESP_LOGW(TAG, "not logged");
        return;
    }
    if (!s_count || !s_oldest) s_oldest = r.at;
    s_count++;
    s_head = (s_head + 1) % s_recs;
}
#else
static void log_open(void) {}
static void log_write(const pass_t *p) { (void)p; }
static esp_err_t read_sector(int sector, rec_t *out)
{
    (void)sector;
    (void)out;
    return ESP_ERR_NOT_SUPPORTED;
}
#endif

// ---------------------------------------------------------------- passes

// Today's ring; with the lock held
static void remember(const pass_t *p)
{
    s_log[s_next] = *p;
    s_next = (s_next + 1) % PASSES_MAX;
    if (s_n < PASSES_MAX) s_n++;
}

// The pass is over: into today's and the log. With the lock held.
static void finish(active_t *a)
{
    remember(&a->p);
    log_write(&a->p);
    a->hex[0] = '\0';
}

// A new day: today's in RAM starts again. With a flash log, today's are
// read from it, so nothing is lost at a restart. With the lock held.
static void start_day(void)
{
    s_n = s_next = 0;
}

// As the device restarts: those overhead now are passes too
static void finish_at_restart(void)
{
    if (!s_lock || xSemaphoreTake(s_lock, pdMS_TO_TICKS(1000)) != pdTRUE) return;
    for (int k = 0; k < ACTIVE_N; k++) {
        if (s_active[k].hex[0]) finish(&s_active[k]);
    }
    xSemaphoreGive(s_lock);
}

void passes_update(const aircraft_t *list, int n, double lat, double lon)
{
    if (!s_lock) {
        s_log = heap_caps_calloc(PASSES_MAX, sizeof(pass_t), MALLOC_CAP_SPIRAM);
        s_active = heap_caps_calloc(ACTIVE_N, sizeof(active_t), MALLOC_CAP_SPIRAM);
        s_sector = heap_caps_malloc(SECTOR, MALLOC_CAP_SPIRAM);
        SemaphoreHandle_t lock = xSemaphoreCreateMutex();
        if (!s_log || !s_active || !s_sector || !lock) {
            heap_caps_free(s_log);
            heap_caps_free(s_active);
            heap_caps_free(s_sector);
            if (lock) vSemaphoreDelete(lock);
            s_log = NULL;
            s_active = NULL;
            s_sector = NULL;
            return;
        }
        log_open();
        s_lock = lock; // last: the others take it as everything ready
        esp_register_shutdown_handler(finish_at_restart);
    }
    const time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    if (tm.tm_year < 120) return; // no clock yet: no day, no times
    const int64_t now_us = esp_timer_get_time();
    const settings_t *set = settings();
    const double within = set->alert_nm10 / 10.0, coslat = cos(lat * M_PI / 180.0);

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (tm.tm_yday != s_day) {
        s_day = tm.tm_yday;
        start_day();
    }
    for (int i = 0; i < n; i++) {
        const aircraft_t *a = &list[i];
        if (a->on_ground || a->alt_ft == AC_ALT_UNKNOWN || a->alt_ft > set->alert_ft || !a->hex[0]) continue;
        const double dx = lon_diff(a->lon, lon) * 60.0 * coslat, dy = (a->lat - lat) * 60.0;
        const double d = sqrt(dx * dx + dy * dy);
        if (d > within) continue;
        active_t *slot = NULL, *free_slot = NULL;
        for (int k = 0; k < ACTIVE_N && !slot; k++) {
            if (strcmp(s_active[k].hex, a->hex) == 0) slot = &s_active[k];
            else if (!s_active[k].hex[0] && !free_slot) free_slot = &s_active[k];
        }
        if (!slot) {
            if (!free_slot) continue;
            slot = free_slot;
            strlcpy(slot->hex, a->hex, sizeof(slot->hex));
            slot->p = (pass_t){.dist_nm = INFINITY};
        }
        slot->seen_us = now_us;
        // Names fill in as they come; the rest is as it was at its closest
        strlcpy(slot->p.name, ac_name(a), sizeof(slot->p.name));
        if (a->type[0]) strlcpy(slot->p.type, a->type, sizeof(slot->p.type));
        if (a->reg[0]) strlcpy(slot->p.reg, a->reg, sizeof(slot->p.reg));
        if (d < slot->p.dist_nm) {
            slot->p.dist_nm = (float)d;
            slot->p.alt_ft = a->alt_ft;
            slot->p.at = now;
        }
    }
    for (int k = 0; k < ACTIVE_N; k++) {
        if (s_active[k].hex[0] && now_us - s_active[k].seen_us > GONE_US) finish(&s_active[k]);
    }
    xSemaphoreGive(s_lock);
}

// Those overhead now; with the lock held
static int add_active(pass_t *out, int max)
{
    int n = 0;
    for (int k = 0; k < ACTIVE_N && n < max; k++) {
        if (s_active[k].hex[0]) out[n++] = s_active[k].p;
    }
    return n;
}

int passes_active(pass_t *out, int max)
{
    if (!s_lock) return 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const int n = add_active(out, max);
    xSemaphoreGive(s_lock);
    return n;
}

int passes_get(pass_t *out, int max)
{
    if (!s_lock) return 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int n = add_active(out, max);
    for (int k = 0; k < s_n && n < max; k++) out[n++] = s_log[(s_next - 1 - k + PASSES_MAX) % PASSES_MAX];
    xSemaphoreGive(s_lock);
    return n;
}

void passes_log_info(passes_log_t *out)
{
    *out = (passes_log_t){0};
    if (!s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = (passes_log_t){.count = s_count, .oldest = s_oldest, .capacity = s_recs};
    xSemaphoreGive(s_lock);
}

// A logged pass: its time, and where it is
typedef struct {
    uint32_t at;
    uint16_t idx;
} found_t;

static int newest_first(const void *a, const void *b)
{
    const found_t *x = a, *y = b;
    return x->at != y->at ? (x->at < y->at ? 1 : -1) : (x->idx < y->idx ? 1 : -1);
}

static int oldest_first(const void *a, const void *b)
{
    return newest_first(b, a);
}

// Every logged pass from since on, into found (s_recs long); how many.
// Every slot is looked at: a power cut leaves a gap, and passes are written
// as they end, so neither the slots nor their order say where to stop. The
// lock is held a sector at a time.
static int scan(time_t since, found_t *found, rec_t *buf)
{
    int n = 0;
    for (int sct = 0; sct < s_recs / PER_SECTOR; sct++) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        const esp_err_t err = read_sector(sct, buf);
        xSemaphoreGive(s_lock);
        if (err != ESP_OK) continue;
        for (int k = 0; k < PER_SECTOR; k++) {
            const rec_t *r = &buf[k];
            if (rec_ok(r) && (time_t)r->at >= since) found[n++] = (found_t){r->at, (uint16_t)(sct * PER_SECTOR + k)};
        }
    }
    return n;
}

// The record at idx; false if it has gone meanwhile
static bool read_rec(int idx, rec_t *r)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool ok = esp_partition_read(s_part, s_base + (size_t)idx * sizeof(*r), r, sizeof(*r)) == ESP_OK && rec_ok(r);
    xSemaphoreGive(s_lock);
    return ok;
}

int passes_since(time_t since, pass_t *out, int max)
{
    if (!s_lock) return 0;
    if (!s_recs) return passes_get(out, max); // no log: today's
    found_t *found = heap_caps_malloc(s_recs * sizeof(found_t), MALLOC_CAP_SPIRAM);
    rec_t *buf = heap_caps_malloc(SECTOR, MALLOC_CAP_SPIRAM);
    int n = 0;
    if (found && buf) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        n = add_active(out, max);
        xSemaphoreGive(s_lock);
        const int f = scan(since, found, buf);
        qsort(found, f, sizeof(*found), newest_first);
        rec_t r;
        for (int k = 0; k < f && n < max; k++) {
            if (read_rec(found[k].idx, &r)) to_pass(&r, &out[n++]);
        }
    }
    heap_caps_free(found);
    heap_caps_free(buf);
    return n;
}

void passes_each(void (*each)(const pass_t *p, void *ctx), void *ctx)
{
    if (!s_lock || !s_recs) return;
    found_t *found = heap_caps_malloc(s_recs * sizeof(found_t), MALLOC_CAP_SPIRAM);
    rec_t *buf = heap_caps_malloc(SECTOR, MALLOC_CAP_SPIRAM);
    if (found && buf) {
        const int f = scan(0, found, buf);
        qsort(found, f, sizeof(*found), oldest_first);
        rec_t r;
        for (int k = 0; k < f; k++) {
            if (!read_rec(found[k].idx, &r)) continue;
            pass_t p;
            to_pass(&r, &p);
            each(&p, ctx);
        }
    }
    heap_caps_free(found);
    heap_caps_free(buf);
}
