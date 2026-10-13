#include "openaip.h"

#include <math.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "board.h"
#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_partition.h"
#include "esp_rom_crc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "net.h"
#include "radar_ui.h"
#include "settings.h"

static const char *TAG = "openaip";

#define API "https://api.core.openaip.net/api"

// OpenAIP's geographic queries (bbox, or pos and dist) that match fewer
// items than the page limit run into its 20 s query limit and fail with 408
// "query exceeded the maximum allowed time"; so does any page after the
// first. Measured 2026-10-07. Filtering by country is quick (about 4 s per
// page of 250, and later pages work), so the whole country is fetched and
// whatever is out of range is dropped here.
#define PAGE_LIMIT 250
#define MAX_PAGES 20
#define MAX_COUNTRIES 4
// The service also has a rate limiter that answers 429 to requests a few
// seconds apart, so they are spaced out and a 429 is waited out
#define REQUEST_GAP_MS 10000
#define BUSY_WAIT_MS 60000
#define TRIES 4
#define RESP_MAX (4 * 1024 * 1024)
#define REFRESH_S (24 * 3600)
#define RETRY_S (10 * 60)
// Outline points closer than this to the previous one are dropped: about
// 3 px at the closest range, and it more than halves a busy area's points
#define MIN_STEP_NM 0.05f

// Which OpenAIP airspace types are drawn, and how. FIRs, airways, sectors
// and the like are left out: they would cover the whole scope in lines.
static const struct {
    uint8_t type;
    uint8_t kind;
} SPACE_TYPES[] = {
    {1, AERO_RESTRICTED}, // Restricted
    {2, AERO_DANGER},     // Danger
    {3, AERO_RESTRICTED}, // Prohibited
    {4, AERO_ZONE},       // CTR
    {5, AERO_MANDATORY},  // TMZ
    {6, AERO_MANDATORY},  // RMZ
    {7, AERO_AREA},       // TMA
    {13, AERO_ZONE},      // ATZ
    {14, AERO_ZONE},      // MATZ
    {26, AERO_AREA},      // CTA
    {36, AERO_ZONE},      // Military CTR
};
#define SPACE_TYPE_COUNT (sizeof(SPACE_TYPES) / sizeof(SPACE_TYPES[0]))

// Airport types: heliports, water and closed aerodromes and agricultural
// strips are left out
static const struct {
    uint8_t type;
    bool major;
} FIELD_TYPES[] = {
    {0, true},  // Airport
    {3, true},  // International airport
    {5, true},  // Military aerodrome
    {9, true},  // IFR airport or airfield
    {2, false}, // Civil airfield
    {1, false}, // Glider site
    {6, false}, // Ultralight site
};
#define FIELD_TYPE_COUNT (sizeof(FIELD_TYPES) / sizeof(FIELD_TYPES[0]))

typedef struct {
    char *data;
    size_t len, cap;
    bool overflow;
} buf_t;

static int64_t s_updated_us; // last success, or when the cache was read; 0 before
static time_t s_fetched;     // when the map shown was fetched, if the clock was set; 0 if not
static const char *s_state = "off";
static char s_detail[80];
static char s_countries[MAX_COUNTRIES * 3]; // as last fetched, e.g. "GB" or "FR,CH"
static int s_airspaces, s_airfields;

static void *psram_realloc(void *p, size_t size)
{
    return heap_caps_realloc(p, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

// Grow *p to hold at least need elements of size elem
static bool reserve(void **p, int *cap, int need, size_t elem)
{
    if (need <= *cap) return true;
    int n = *cap ? *cap : 64;
    while (n < need) n *= 2;
    void *q = psram_realloc(*p, (size_t)n * elem);
    if (!q) return false;
    *p = q;
    *cap = n;
    return true;
}

void aero_map_free(aero_map_t *map)
{
    if (!map) return;
    free(map->spaces);
    free(map->pts);
    free(map->fields);
    free(map);
}

static TaskHandle_t s_task;

// pdMS_TO_TICKS overflows 32 bits long before a day, so an hour at a time.
// A change of settings (openaip_restart()) ends it early.
static void sleep_s(int seconds)
{
    for (; seconds > 0; seconds -= 3600) {
        if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS((seconds < 3600 ? seconds : 3600) * 1000))) return;
    }
}

static bool configured(const settings_t *s)
{
    if (s->openaip_key[0] && (s->show_airspace || s->show_airfields)) return true;
    strlcpy(s_detail, s->openaip_key[0] ? "Both layers are off" : "No API key", sizeof(s_detail));
    return false;
}

// ---------------------------------------------------------------- HTTP

static esp_err_t on_http_event(esp_http_client_event_t *evt)
{
    buf_t *b = evt->user_data;
    if (evt->event_id != HTTP_EVENT_ON_DATA || b->overflow) return ESP_OK;
    size_t need = b->len + evt->data_len + 1;
    if (need > RESP_MAX) {
        b->overflow = true;
        return ESP_OK;
    }
    if (need > b->cap) {
        size_t cap = b->cap ? b->cap : 64 * 1024;
        while (cap < need) cap *= 2;
        char *p = psram_realloc(b->data, cap);
        if (!p) {
            b->overflow = true;
            return ESP_OK;
        }
        b->data = p;
        b->cap = cap;
    }
    memcpy(b->data + b->len, evt->data, evt->data_len);
    b->len += evt->data_len;
    b->data[b->len] = '\0';
    return ESP_OK;
}

typedef struct {
    esp_http_client_handle_t client;
    buf_t buf;
    bool rejected; // the key was refused
    bool first;    // no request made yet, so no gap needed
} session_t;

// One list request, parsed. NULL on failure, with s_detail saying why.
static cJSON *get_list(session_t *ss, const char *url)
{
    if (!ss->first) vTaskDelay(pdMS_TO_TICKS(REQUEST_GAP_MS));
    ss->first = false;

    buf_t *b = &ss->buf;
    esp_err_t err;
    int status = 0;
    for (int tries = 1;; tries++) {
        b->len = 0;
        b->overflow = false;
        esp_http_client_set_url(ss->client, url);
        err = esp_http_client_perform(ss->client);
        status = err == ESP_OK ? esp_http_client_get_status_code(ss->client) : 0;
        if ((err == ESP_OK && status != 429) || tries == TRIES) break;
        // Busy, or the connection dropped. Start the next try on a fresh
        // connection: the server closes an idle one while we wait.
        ESP_LOGI(TAG, "%s, retrying in %d s", err == ESP_OK ? "rate limited" : esp_err_to_name(err),
                 BUSY_WAIT_MS / 1000);
        esp_http_client_close(ss->client);
        vTaskDelay(pdMS_TO_TICKS(BUSY_WAIT_MS));
    }
    if (err != ESP_OK) {
        snprintf(s_detail, sizeof(s_detail), "Couldn't reach OpenAIP (%s)", esp_err_to_name(err));
        return NULL;
    }
    // An unknown key gets 404 "Failed to load user permissions", not 401
    if (status == 401 || status == 403 || status == 404) {
        snprintf(s_detail, sizeof(s_detail), "API key not accepted (HTTP %d)", status);
        ss->rejected = true;
        return NULL;
    }
    if (status != 200 || b->overflow) {
        snprintf(s_detail, sizeof(s_detail), b->overflow ? "Response too large" : "OpenAIP answered HTTP %d",
                 status);
        // Its error bodies are JSON with a "message" saying what it didn't like
        if (!b->overflow && b->len && b->data[0] == '{') ESP_LOGW(TAG, "%s: %.300s", url, b->data);
        return NULL;
    }
    cJSON *root = cJSON_ParseWithLength(b->data, b->len);
    if (!cJSON_IsArray(cJSON_GetObjectItemCaseSensitive(root, "items"))) {
        snprintf(s_detail, sizeof(s_detail), "Unexpected response from OpenAIP");
        cJSON_Delete(root);
        return NULL;
    }
    return root;
}

// ---------------------------------------------------------------- parsing

typedef struct {
    double lat0, lon0, coslat0;
    aero_map_t *map;
    int cap_spaces, cap_pts, cap_fields;
} builder_t;

// Flat-earth, exactly as the radar projects aircraft
static aero_pt_t project(const builder_t *bld, double lon, double lat)
{
    return (aero_pt_t){
        .x = (float)((lon - bld->lon0) * 60.0 * bld->coslat0),
        .y = (float)((lat - bld->lat0) * 60.0),
    };
}

static bool lonlat(const cJSON *pair, double *lon, double *lat)
{
    const cJSON *a = cJSON_GetArrayItem(pair, 0), *b = cJSON_GetArrayItem(pair, 1);
    if (!cJSON_IsNumber(a) || !cJSON_IsNumber(b)) return false;
    *lon = a->valuedouble;
    *lat = b->valuedouble;
    return true;
}

static int int_or(const cJSON *obj, const char *key, int fallback)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsNumber(v) ? v->valueint : fallback;
}

static void add_airspace(builder_t *bld, const cJSON *item)
{
    const int type = int_or(item, "type", -1);
    int kind = -1;
    for (size_t i = 0; i < SPACE_TYPE_COUNT; i++) {
        if (SPACE_TYPES[i].type == type) kind = SPACE_TYPES[i].kind;
    }
    if (kind < 0) return; // the filter in the URL should have done this already

    // The outer ring of a GeoJSON polygon
    const cJSON *geom = cJSON_GetObjectItemCaseSensitive(item, "geometry");
    const cJSON *ring = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(geom, "coordinates"), 0);
    const int n_in = cJSON_GetArraySize(ring);
    aero_map_t *m = bld->map;
    if (n_in < 3 || !reserve((void **)&m->pts, &bld->cap_pts, m->n_pts + n_in, sizeof(aero_pt_t)) ||
        !reserve((void **)&m->spaces, &bld->cap_spaces, m->n_spaces + 1, sizeof(aero_space_t))) {
        return;
    }

    const int first = m->n_pts;
    float x0 = INFINITY, y0 = INFINITY, x1 = -INFINITY, y1 = -INFINITY;
    const cJSON *pair;
    cJSON_ArrayForEach(pair, ring) {
        double lon, lat;
        if (!lonlat(pair, &lon, &lat)) continue;
        aero_pt_t p = project(bld, lon, lat);
        if (m->n_pts > first) {
            const aero_pt_t *last = &m->pts[m->n_pts - 1];
            if (fabsf(p.x - last->x) < MIN_STEP_NM && fabsf(p.y - last->y) < MIN_STEP_NM) continue;
        }
        m->pts[m->n_pts++] = p;
        x0 = fminf(x0, p.x);
        y0 = fminf(y0, p.y);
        x1 = fmaxf(x1, p.x);
        y1 = fmaxf(y1, p.y);
    }
    // Kept if its bounding box reaches the square round the largest range,
    // which also keeps a long edge that crosses it between far-off corners
    const float r = OPENAIP_RADIUS_NM;
    const int n = m->n_pts - first;
    if (n < 3 || n > UINT16_MAX || x1 < -r || x0 > r || y1 < -r || y0 > r) {
        m->n_pts = first;
        return;
    }
    m->spaces[m->n_spaces++] = (aero_space_t){.first = first, .n = n, .kind = kind};
}

static void add_airfield(builder_t *bld, const cJSON *item)
{
    const int type = int_or(item, "type", -1);
    int major = -1;
    for (size_t i = 0; i < FIELD_TYPE_COUNT; i++) {
        if (FIELD_TYPES[i].type == type) major = FIELD_TYPES[i].major;
    }
    if (major < 0) return;

    const cJSON *geom = cJSON_GetObjectItemCaseSensitive(item, "geometry");
    double lon, lat;
    if (!lonlat(cJSON_GetObjectItemCaseSensitive(geom, "coordinates"), &lon, &lat)) return;
    const aero_pt_t p = project(bld, lon, lat);
    aero_map_t *m = bld->map;
    if (hypotf(p.x, p.y) > OPENAIP_RADIUS_NM ||
        !reserve((void **)&m->fields, &bld->cap_fields, m->n_fields + 1, sizeof(aero_field_t))) {
        return;
    }

    aero_field_t *f = &m->fields[m->n_fields++];
    *f = (aero_field_t){.x = p.x, .y = p.y, .runway_deg = -1, .major = major};

    const cJSON *icao = cJSON_GetObjectItemCaseSensitive(item, "icaoCode");
    const cJSON *name = cJSON_GetObjectItemCaseSensitive(item, "name");
    if (cJSON_IsString(icao) && icao->valuestring[0]) {
        strlcpy(f->ident, icao->valuestring, sizeof(f->ident));
    } else if (cJSON_IsString(name)) {
        strlcpy(f->ident, name->valuestring, sizeof(f->ident));
    }

    // The main runway's heading, else the first one's
    const cJSON *rwy;
    cJSON_ArrayForEach(rwy, cJSON_GetObjectItemCaseSensitive(item, "runways")) {
        const int hdg = int_or(rwy, "trueHeading", -1);
        if (hdg < 0) continue;
        if (f->runway_deg < 0 || cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(rwy, "mainRunway"))) {
            f->runway_deg = hdg % 180;
        }
    }
}

// ---------------------------------------------------------------- fetching

// Every page of one endpoint for one country through add(). False on failure.
static bool fetch_country(session_t *ss, builder_t *bld, const char *what, const char *query, const char *cc,
                          void (*add)(builder_t *, const cJSON *))
{
    char url[512];
    for (int page = 1; page <= MAX_PAGES; page++) {
        snprintf(url, sizeof(url), API "/%s?country=%s&limit=%d&page=%d%s", what, cc, PAGE_LIMIT, page, query);
        cJSON *root = get_list(ss, url);
        if (!root) return false;
        const cJSON *items = cJSON_GetObjectItemCaseSensitive(root, "items");
        const int n = cJSON_GetArraySize(items);
        const cJSON *item;
        cJSON_ArrayForEach(item, items) add(bld, item);
        const int total = int_or(root, "totalCount", -1);
        cJSON_Delete(root);
        ESP_LOGI(TAG, "%s %s page %d: %d of %d, %u bytes", cc, what, page, n, total, (unsigned)ss->buf.len);
        if (n < PAGE_LIMIT || (total >= 0 && page * PAGE_LIMIT >= total)) return true;
    }
    ESP_LOGW(TAG, "%s %s: stopped at %d pages", cc, what, MAX_PAGES);
    return true;
}

// The country of the airfield nearest the centre. A distance query that
// fills its one-item page comes back quickly, unlike a complete one.
static bool find_country(session_t *ss, double lat, double lon, char *cc, size_t n)
{
    char url[160];
    snprintf(url, sizeof(url), API "/airports?pos=%.4f,%.4f&dist=150000&limit=1&fields=country", lat, lon);
    cJSON *root = get_list(ss, url);
    if (!root) return false;
    const cJSON *item = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(root, "items"), 0);
    const cJSON *c = cJSON_GetObjectItemCaseSensitive(item, "country");
    if (cJSON_IsArray(c)) c = cJSON_GetArrayItem(c, 0);
    bool ok = cJSON_IsString(c) && strlen(c->valuestring) == 2;
    if (ok) {
        strlcpy(cc, c->valuestring, n);
    } else {
        snprintf(s_detail, sizeof(s_detail), "No airfield nearby to tell the country; set it in the settings");
    }
    cJSON_Delete(root);
    return ok;
}

static aero_map_t *fetch(const settings_t *s, bool *rejected)
{
    // The types wanted, as repeated query parameters
    char spaces_q[160] = "&fields=type,geometry", fields_q[160] = "&fields=name,icaoCode,type,geometry,runways";
    for (size_t i = 0; i < SPACE_TYPE_COUNT; i++) {
        snprintf(spaces_q + strlen(spaces_q), sizeof(spaces_q) - strlen(spaces_q), "&type=%d", SPACE_TYPES[i].type);
    }
    for (size_t i = 0; i < FIELD_TYPE_COUNT; i++) {
        snprintf(fields_q + strlen(fields_q), sizeof(fields_q) - strlen(fields_q), "&type=%d", FIELD_TYPES[i].type);
    }

    session_t ss = {.first = true};
    builder_t bld = {
        .lat0 = s->lat,
        .lon0 = s->lon,
        .coslat0 = cos(s->lat * M_PI / 180.0),
        .map = heap_caps_calloc(1, sizeof(aero_map_t), MALLOC_CAP_SPIRAM),
    };
    esp_http_client_config_t cfg = {
        .url = API,
        .event_handler = on_http_event,
        .user_data = &ss.buf,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 30000,
        .buffer_size = 4096,
        .keep_alive_enable = true,
        .user_agent = "overhead-radar/1.0 (ESP32-P4)",
    };
    ss.client = bld.map ? esp_http_client_init(&cfg) : NULL;
    bool ok = ss.client != NULL;
    if (!ok) snprintf(s_detail, sizeof(s_detail), "Out of memory");
    if (ok) {
        esp_http_client_set_header(ss.client, "x-openaip-api-key", s->openaip_key);
        esp_http_client_set_header(ss.client, "Accept", "application/json");

        // The countries set in the settings, or the nearest airfield's
        char list[sizeof(s->openaip_countries)];
        strlcpy(list, s->openaip_countries, sizeof(list));
        if (!list[0]) ok = find_country(&ss, s->lat, s->lon, list, sizeof(list));
        if (ok) strlcpy(s_countries, list, sizeof(s_countries));

        char *save = NULL;
        int done = 0;
        for (char *cc = ok ? strtok_r(list, ", ", &save) : NULL; cc && ok && done < MAX_COUNTRIES;
             cc = strtok_r(NULL, ", ", &save), done++) {
            ok = fetch_country(&ss, &bld, "airspaces", spaces_q, cc, add_airspace) &&
                 fetch_country(&ss, &bld, "airports", fields_q, cc, add_airfield);
        }
        esp_http_client_cleanup(ss.client);
    }
    *rejected = ss.rejected;
    free(ss.buf.data);
    if (!ok) {
        aero_map_free(bld.map);
        return NULL;
    }
    return bld.map;
}

// ---------------------------------------------------------------- cache
//
// The last map, kept in its own partition so the airspace is on the scope a
// second after boot instead of after the minute and a half a fetch takes.
// Devices updated only over WiFi have no such partition (an update can't add
// one) and fetch at every boot as before.

#define CACHE_LABEL "aipcache"
#define CACHE_MAGIC 0x5049414f // "OAIP"
// Up when what a fetch keeps changes (types, MIN_STEP_NM, the structs)
#define CACHE_VERSION 1
#define CACHE_DATA 4096 // the header has the first sector to itself

typedef struct {
    uint32_t magic, version;
    int32_t lat_e6, lon_e6; // the centre it was projected around
    char countries_set[12]; // the setting it was fetched for; "" automatic
    char countries[12];     // what was fetched
    int64_t fetched;        // unix time, 0 if the clock wasn't set
    uint32_t n_spaces, n_pts, n_fields;
    uint32_t crc; // of the data
} cache_hdr_t;

static const esp_partition_t *cache_part(void)
{
    return esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, CACHE_LABEL);
}

static bool cache_matches(const cache_hdr_t *h, const settings_t *s)
{
    return h->magic == CACHE_MAGIC && h->version == CACHE_VERSION && h->lat_e6 == (int32_t)lround(s->lat * 1e6) &&
           h->lon_e6 == (int32_t)lround(s->lon * 1e6) && strcmp(h->countries_set, s->openaip_countries) == 0;
}

// The cached map for these settings, or NULL. Without XIP, even reading
// flash turns the cache off and this task's PSRAM stack with it (a boot loop
// on the 3.4C, October 2026); cache_save() never writes there either.
static aero_map_t *cache_load(const settings_t *s, cache_hdr_t *hdr)
{
#if !CONFIG_SPIRAM_XIP_FROM_PSRAM
    return NULL;
#endif
    const esp_partition_t *p = cache_part();
    if (!p || esp_partition_read(p, 0, hdr, sizeof(*hdr)) != ESP_OK || !cache_matches(hdr, s)) return NULL;
    const size_t sz_s = hdr->n_spaces * sizeof(aero_space_t), sz_p = hdr->n_pts * sizeof(aero_pt_t),
                 sz_f = hdr->n_fields * sizeof(aero_field_t);
    if (CACHE_DATA + sz_s + sz_p + sz_f > p->size) return NULL;

    aero_map_t *map = heap_caps_calloc(1, sizeof(*map), MALLOC_CAP_SPIRAM);
    if (!map) return NULL;
    map->spaces = heap_caps_malloc(sz_s + 1, MALLOC_CAP_SPIRAM);
    map->pts = heap_caps_malloc(sz_p + 1, MALLOC_CAP_SPIRAM);
    map->fields = heap_caps_malloc(sz_f + 1, MALLOC_CAP_SPIRAM);
    map->n_spaces = hdr->n_spaces;
    map->n_pts = hdr->n_pts;
    map->n_fields = hdr->n_fields;
    bool ok = map->spaces && map->pts && map->fields &&
              esp_partition_read(p, CACHE_DATA, map->spaces, sz_s) == ESP_OK &&
              esp_partition_read(p, CACHE_DATA + sz_s, map->pts, sz_p) == ESP_OK &&
              esp_partition_read(p, CACHE_DATA + sz_s + sz_p, map->fields, sz_f) == ESP_OK;
    if (ok) {
        uint32_t crc = esp_rom_crc32_le(0, (const uint8_t *)map->spaces, sz_s);
        crc = esp_rom_crc32_le(crc, (const uint8_t *)map->pts, sz_p);
        crc = esp_rom_crc32_le(crc, (const uint8_t *)map->fields, sz_f);
        ok = crc == hdr->crc;
    }
    if (!ok) {
        ESP_LOGW(TAG, "cached map unreadable, fetching");
        aero_map_free(map);
        return NULL;
    }
    return map;
}

// Keep map for the next boot. Writes flash, which on a board without XIP
// takes PSRAM away, this task's stack included; without XIP this does
// nothing, and the map is fetched at every boot.
static void cache_save(const settings_t *s, const aero_map_t *map)
{
#if CONFIG_SPIRAM_XIP_FROM_PSRAM
    const esp_partition_t *p = cache_part();
    if (!p) return;
    const size_t sz_s = map->n_spaces * sizeof(aero_space_t), sz_p = map->n_pts * sizeof(aero_pt_t),
                 sz_f = map->n_fields * sizeof(aero_field_t);
    const size_t total = CACHE_DATA + sz_s + sz_p + sz_f;
    if (total > p->size) {
        ESP_LOGW(TAG, "map of %u bytes is bigger than the cache", (unsigned)total);
        return;
    }
    cache_hdr_t h = {
        .magic = CACHE_MAGIC,
        .version = CACHE_VERSION,
        .lat_e6 = (int32_t)lround(s->lat * 1e6),
        .lon_e6 = (int32_t)lround(s->lon * 1e6),
        .fetched = s_fetched,
        .n_spaces = map->n_spaces,
        .n_pts = map->n_pts,
        .n_fields = map->n_fields,
    };
    strlcpy(h.countries_set, s->openaip_countries, sizeof(h.countries_set));
    strlcpy(h.countries, s_countries, sizeof(h.countries));
    h.crc = esp_rom_crc32_le(0, (const uint8_t *)map->spaces, sz_s);
    h.crc = esp_rom_crc32_le(h.crc, (const uint8_t *)map->pts, sz_p);
    h.crc = esp_rom_crc32_le(h.crc, (const uint8_t *)map->fields, sz_f);

    // The header last: cut off part way, the cache is just missing
    const size_t erase = (total + 4095) & ~(size_t)4095;
    esp_err_t err = esp_partition_erase_range(p, 0, erase);
    if (err == ESP_OK) err = esp_partition_write(p, CACHE_DATA, map->spaces, sz_s);
    if (err == ESP_OK) err = esp_partition_write(p, CACHE_DATA + sz_s, map->pts, sz_p);
    if (err == ESP_OK) err = esp_partition_write(p, CACHE_DATA + sz_s + sz_p, map->fields, sz_f);
    if (err == ESP_OK) err = esp_partition_write(p, 0, &h, sizeof(h));
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "map kept for the next boot (%u bytes)", (unsigned)total);
    } else {
        ESP_LOGW(TAG, "map not kept: %s", esp_err_to_name(err));
    }
#else
    (void)s;
    (void)map;
#endif
}

static bool clock_set(void)
{
    return time(NULL) > 1700000000; // after November 2023: SNTP has run
}

// ---------------------------------------------------------------- public

static void openaip_task(void *arg)
{
    const settings_t *s = settings();
    for (;;) {
        if (!configured(s)) {
            s_state = "off";
            board_lock();
            radar_ui_set_aero(NULL);
            board_unlock();
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            continue;
        }

        // The kept map at once, if it was fetched for this centre and these
        // countries; fetched again when it is a day old
        cache_hdr_t hdr;
        aero_map_t *cached = cache_load(s, &hdr);
        if (cached) {
            ESP_LOGI(TAG, "%s: %d airspaces, %d airfields, from the cache", hdr.countries, cached->n_spaces,
                     cached->n_fields);
            s_airspaces = cached->n_spaces;
            s_airfields = cached->n_fields;
            strlcpy(s_countries, hdr.countries, sizeof(s_countries));
            s_fetched = hdr.fetched;
            s_updated_us = esp_timer_get_time();
            s_state = "ok";
            s_detail[0] = '\0';
            board_lock();
            radar_ui_set_aero(cached); // the radar owns it now
            board_unlock();

            // Its age needs the clock, which SNTP sets soon after joining
            while (!net_wait_connected(60000)) {
            }
            for (int i = 0; i < 30 && !clock_set(); i++) vTaskDelay(pdMS_TO_TICKS(1000));
            const int64_t age = clock_set() && hdr.fetched ? time(NULL) - hdr.fetched : -1;
            if (age >= 0 && age < REFRESH_S) {
                sleep_s(REFRESH_S - (int)age);
                continue;
            }
            // A day old, or of no known age: fetch, with it on the scope meanwhile
        } else {
            // Said before waiting for the network, so the status doesn't read
            // "off" for the first seconds after boot
            s_state = "loading";
            s_detail[0] = '\0';
        }

        while (!net_wait_connected(60000)) {
        }
        bool rejected = false;
        aero_map_t *map = fetch(s, &rejected);
        if (!map) {
            ESP_LOGW(TAG, "%s", s_detail);
            // A cached map stays up, and stays "ok": it is still right
            if (!cached) s_state = "error";
            // A refused key won't start working; a new one wakes us
            if (rejected) {
                ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            } else {
                sleep_s(RETRY_S);
            }
            continue;
        }

        ESP_LOGI(TAG, "%s: %d airspaces (%d points), %d airfields within %d NM", s_countries, map->n_spaces,
                 map->n_pts, map->n_fields, OPENAIP_RADIUS_NM);
        s_airspaces = map->n_spaces;
        s_airfields = map->n_fields;
        s_fetched = clock_set() ? time(NULL) : 0;
        s_updated_us = esp_timer_get_time();
        s_state = "ok";
        s_detail[0] = '\0';
        // Before the radar has it: once it does, a new centre can free it
        cache_save(s, map);
        board_lock();
        radar_ui_set_aero(map);
        board_unlock();
        sleep_s(REFRESH_S);
    }
}

void openaip_start(void)
{
    // Its stack can be in PSRAM: nothing here writes flash. It waits for a
    // key while there is none.
    if (xTaskCreateWithCaps(openaip_task, "openaip", 8192, NULL, 3, &s_task,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        xTaskCreate(openaip_task, "openaip", 8192, NULL, 3, &s_task);
    }
}

void openaip_restart(void)
{
    if (s_task) xTaskNotifyGive(s_task);
}

void openaip_get_status(openaip_status_t *out)
{
    *out = (openaip_status_t){
        .state = s_state,
        .detail = s_detail,
        .countries = s_countries,
        .airspaces = s_airspaces,
        .airfields = s_airfields,
        // From the fetch itself when known: a map read from the cache may be hours old
        .age_s = s_fetched && clock_set() ? (int)(time(NULL) - s_fetched)
                 : s_updated_us           ? (int)((esp_timer_get_time() - s_updated_us) / 1000000)
                                          : -1,
    };
}
