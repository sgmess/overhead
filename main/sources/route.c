#include "route.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "route";

#define API "https://adsb.im/api/0/routeset"
#define CACHE_N 32
#define RESP_MAX 4096
#define RETRY_US (60 * 1000000LL) // after a failed request, not after "unknown"

typedef struct {
    char callsign[10];
    route_t route;         // codes "" when unknown
    bool done;             // looked up, successfully or not
    int64_t failed_us;     // when the request itself last failed; 0 if not
    uint32_t used;         // for evicting the least recently used
} entry_t;

// In PSRAM, like the response: static internal RAM is what esp_hosted's
// start-up allocations need before the scheduler runs
static entry_t *s_cache;
static uint32_t s_clock;
static SemaphoreHandle_t s_lock;
static TaskHandle_t s_task;

// The one lookup waiting, newest wins: only the card's aircraft matters
static char s_want[10];
static double s_want_lat, s_want_lon;

typedef struct {
    char data[RESP_MAX];
    size_t len;
    bool overflow;
} resp_t;

static esp_err_t on_http_event(esp_http_client_event_t *evt)
{
    resp_t *r = evt->user_data;
    if (evt->event_id != HTTP_EVENT_ON_DATA || r->overflow) return ESP_OK;
    if (r->len + evt->data_len >= sizeof(r->data)) {
        r->overflow = true;
        return ESP_OK;
    }
    memcpy(r->data + r->len, evt->data, evt->data_len);
    r->len += evt->data_len;
    r->data[r->len] = '\0';
    return ESP_OK;
}

static entry_t *find(const char *callsign)
{
    for (int i = 0; i < CACHE_N; i++) {
        if (s_cache[i].callsign[0] && strcmp(s_cache[i].callsign, callsign) == 0) return &s_cache[i];
    }
    return NULL;
}

static entry_t *find_or_add(const char *callsign)
{
    entry_t *e = find(callsign);
    if (e) return e;
    e = &s_cache[0];
    for (int i = 1; i < CACHE_N; i++) {
        if (s_cache[i].used < e->used) e = &s_cache[i];
    }
    *e = (entry_t){0};
    strlcpy(e->callsign, callsign, sizeof(e->callsign));
    return e;
}

// "LHR-JFK", IATA codes (ICAO when there are none), with " ?" added when
// the service thinks the route doesn't fit where the aircraft is
static void format_route(const cJSON *item, route_t *r)
{
    char *out = r->codes;
    const size_t n = sizeof(r->codes);
    const cJSON *codes = cJSON_GetObjectItemCaseSensitive(item, "_airport_codes_iata");
    if (!cJSON_IsString(codes) || strcmp(codes->valuestring, "unknown") == 0) {
        codes = cJSON_GetObjectItemCaseSensitive(item, "airport_codes");
    }
    if (!cJSON_IsString(codes) || !codes->valuestring[0] || strcmp(codes->valuestring, "unknown") == 0) return;
    strlcpy(out, codes->valuestring, n);
    r->plausible = !cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(item, "plausible"));
    if (!r->plausible) strlcat(out, " ?", n);
}

// "London|New York": each airport's town, or its name when it has none,
// "|" between them since a town's name can have a hyphen in it; and where
// each airport is
static void format_places(const cJSON *item, route_t *r)
{
    char *out = r->places;
    const size_t n = sizeof(r->places);
    bool places_ok = true;
    const cJSON *ap;
    cJSON_ArrayForEach(ap, cJSON_GetObjectItemCaseSensitive(item, "_airports"))
    {
        // And where each is, for how far along it is
        const cJSON *lat = cJSON_GetObjectItemCaseSensitive(ap, "lat");
        const cJSON *lon = cJSON_GetObjectItemCaseSensitive(ap, "lon");
        if (r->stops >= 0 && r->stops < ROUTE_STOPS_MAX && cJSON_IsNumber(lat) && cJSON_IsNumber(lon)) {
            r->lat[r->stops] = (float)lat->valuedouble;
            r->lon[r->stops] = (float)lon->valuedouble;
            r->stops++;
        } else {
            r->stops = -1; // one missing would leave the legs out of step
        }
        const cJSON *where = cJSON_GetObjectItemCaseSensitive(ap, "location");
        if (!cJSON_IsString(where) || !where->valuestring[0]) where = cJSON_GetObjectItemCaseSensitive(ap, "name");
        if (!cJSON_IsString(where) || !where->valuestring[0]) {
            out[0] = '\0'; // one unknown would leave the rest out of step
            places_ok = false;
        }
        if (!places_ok) continue;
        if (out[0]) strlcat(out, "|", n);
        strlcat(out, where->valuestring, n);
    }
    if (r->stops < 2) r->stops = 0;
}

static bool lookup(const char *callsign, double lat, double lon, route_t *route)
{
    static resp_t *resp_buf; // one task, so one buffer
    if (!resp_buf) resp_buf = heap_caps_calloc(1, sizeof(*resp_buf), MALLOC_CAP_SPIRAM);
    if (!resp_buf) return false;
    resp_t *r = resp_buf;
    char body[128];
    snprintf(body, sizeof(body), "{\"planes\":[{\"callsign\":\"%s\",\"lat\":%.4f,\"lng\":%.4f}]}", callsign, lat,
             lon);

    r->len = 0;
    r->overflow = false;
    esp_http_client_config_t cfg = {
        .url = API,
        .method = HTTP_METHOD_POST,
        .event_handler = on_http_event,
        .user_data = r,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 10000,
        .user_agent = "overhead-radar/1.0 (ESP32-P4)",
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return false;
    esp_http_client_set_header(c, "Content-Type", "application/json");
    esp_http_client_set_post_field(c, body, strlen(body));
    esp_err_t err = esp_http_client_perform(c);
    int status = esp_http_client_get_status_code(c);
    esp_http_client_cleanup(c);
    if (err != ESP_OK || status != 200 || r->overflow) {
        ESP_LOGW(TAG, "%s: %s, HTTP %d", callsign, esp_err_to_name(err), status);
        return false;
    }

    cJSON *root = cJSON_ParseWithLength(r->data, r->len);
    const cJSON *first = cJSON_IsArray(root) ? cJSON_GetArrayItem(root, 0) : NULL;
    bool ok = cJSON_IsObject(first);
    if (ok) {
        format_route(first, route);
        format_places(first, route);
    }
    cJSON_Delete(root);
    if (ok) ESP_LOGI(TAG, "%s: %s", callsign, route->codes[0] ? route->codes : "unknown");
    return ok;
}

static void route_task(void *arg)
{
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        char callsign[10];
        double lat, lon;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        strlcpy(callsign, s_want, sizeof(callsign));
        lat = s_want_lat;
        lon = s_want_lon;
        s_want[0] = '\0';
        xSemaphoreGive(s_lock);
        if (!callsign[0]) continue;

        route_t route = {0};
        bool ok = lookup(callsign, lat, lon, &route);

        xSemaphoreTake(s_lock, portMAX_DELAY);
        entry_t *e = find_or_add(callsign);
        e->used = ++s_clock;
        if (ok) {
            e->route = route;
            e->done = true;
            e->failed_us = 0;
        } else {
            e->failed_us = esp_timer_get_time();
        }
        xSemaphoreGive(s_lock);
    }
}

void route_start(void)
{
    s_cache = heap_caps_calloc(CACHE_N, sizeof(entry_t), MALLOC_CAP_SPIRAM);
    if (!s_cache) return;
    s_lock = xSemaphoreCreateMutex();
    // TLS and the request are on the heap; this is call depth, as for OpenAIP
    if (xTaskCreateWithCaps(route_task, "route", 8192, NULL, 3, &s_task, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) !=
        pdPASS) {
        xTaskCreate(route_task, "route", 8192, NULL, 3, &s_task);
    }
}

bool route_get(const char *callsign, double lat, double lon, route_t *out)
{
    *out = (route_t){0};
    if (!s_lock || !callsign[0]) return false;
    // It goes into the request's JSON as it is
    for (const char *c = callsign; *c; c++) {
        if (!((*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9'))) return false;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    entry_t *e = find(callsign);
    if (e) e->used = ++s_clock;
    if (e && e->done) {
        *out = e->route;
    } else if ((!e || !e->failed_us || esp_timer_get_time() - e->failed_us > RETRY_US) &&
               strcmp(s_want, callsign) != 0) {
        // One waiting at a time: the one this replaces was never asked for,
        // so let it ask again at once rather than after RETRY_US
        entry_t *gone = s_want[0] ? find(s_want) : NULL;
        if (gone && !gone->done) gone->failed_us = 0;
        strlcpy(s_want, callsign, sizeof(s_want));
        s_want_lat = lat;
        s_want_lon = lon;
        // Stops it asking again every second while the request is out
        if (!e) e = find_or_add(callsign);
        e->used = ++s_clock;
        e->failed_us = esp_timer_get_time();
        xTaskNotifyGive(s_task);
    }
    xSemaphoreGive(s_lock);
    return out->codes[0] != '\0';
}

// Great-circle distance in nautical miles
static double nm_between(double lat1, double lon1, double lat2, double lon2)
{
    const double r = M_PI / 180.0;
    const double a = pow(sin((lat2 - lat1) * r / 2), 2) + cos(lat1 * r) * cos(lat2 * r) * pow(sin((lon2 - lon1) * r / 2), 2);
    return 2 * 3440.065 * asin(sqrt(a));
}

bool route_progress(const route_t *r, double lat, double lon, int *percent, int *to_go_nm)
{
    if (!r->plausible || r->stops < 2) return false;
    // The leg it is on: the one it is least out of the way for
    double best = INFINITY, flown = 0, to_go = 0;
    for (int k = 0; k + 1 < r->stops; k++) {
        const double a = nm_between(r->lat[k], r->lon[k], lat, lon);
        const double b = nm_between(lat, lon, r->lat[k + 1], r->lon[k + 1]);
        const double detour = a + b - nm_between(r->lat[k], r->lon[k], r->lat[k + 1], r->lon[k + 1]);
        if (detour < best) {
            best = detour;
            flown = a;
            to_go = b;
        }
    }
    if (flown + to_go < 1) return false;
    *percent = (int)(100 * flown / (flown + to_go)); // down: 100% only once there
    *to_go_nm = (int)lround(to_go);
    return true;
}
