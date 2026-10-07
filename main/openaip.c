#include "openaip.h"

#include <math.h>
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

static int64_t s_updated_us; // last success; 0 before one
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

// pdMS_TO_TICKS overflows 32 bits long before a day, so an hour at a time
static void sleep_s(int seconds)
{
    for (; seconds > 0; seconds -= 3600) vTaskDelay(pdMS_TO_TICKS((seconds < 3600 ? seconds : 3600) * 1000));
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

// ---------------------------------------------------------------- public

static void openaip_task(void *arg)
{
    const settings_t *s = settings();
    for (;;) {
        while (!net_wait_connected(60000)) {
        }
        // Let the traffic come up first
        vTaskDelay(pdMS_TO_TICKS(5000));

        s_state = "loading";
        s_detail[0] = '\0';
        bool rejected = false;
        aero_map_t *map = fetch(s, &rejected);
        if (!map) {
            ESP_LOGW(TAG, "%s", s_detail);
            s_state = "error";
            // A refused key won't start working; a new one restarts us
            if (rejected) break;
            sleep_s(RETRY_S);
            continue;
        }

        ESP_LOGI(TAG, "%s: %d airspaces (%d points), %d airfields within %d NM", s_countries, map->n_spaces,
                 map->n_pts, map->n_fields, OPENAIP_RADIUS_NM);
        s_airspaces = map->n_spaces;
        s_airfields = map->n_fields;
        s_updated_us = esp_timer_get_time();
        s_state = "ok";
        board_lock();
        radar_ui_set_aero(map); // the radar owns it now
        board_unlock();
        sleep_s(REFRESH_S);
    }
    vTaskDelete(NULL);
}

void openaip_start(void)
{
    const settings_t *s = settings();
    if (!s->openaip_key[0] || (!s->show_airspace && !s->show_airfields)) {
        strlcpy(s_detail, s->openaip_key[0] ? "Both layers are off" : "No API key", sizeof(s_detail));
        return;
    }
    // Its stack can be in PSRAM: nothing here writes flash
    if (xTaskCreateWithCaps(openaip_task, "openaip", 8192, NULL, 3, NULL,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        xTaskCreate(openaip_task, "openaip", 8192, NULL, 3, NULL);
    }
}

void openaip_get_status(openaip_status_t *out)
{
    *out = (openaip_status_t){
        .state = s_state,
        .detail = s_detail,
        .countries = s_countries,
        .airspaces = s_airspaces,
        .airfields = s_airfields,
        .age_s = s_updated_us ? (int)((esp_timer_get_time() - s_updated_us) / 1000000) : -1,
    };
}
