#include "route.h"

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
#define ROUTE_MAX 40
#define RETRY_US (60 * 1000000LL) // after a failed request, not after "unknown"

typedef struct {
    char callsign[10];
    char route[ROUTE_MAX]; // "" when unknown
    bool done;             // looked up, successfully or not
    int64_t failed_us;     // when the request itself last failed; 0 if not
    uint32_t used;         // for evicting the least recently used
} entry_t;

static entry_t s_cache[CACHE_N];
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
static void format_route(const cJSON *item, char *out, size_t n)
{
    out[0] = '\0';
    const cJSON *codes = cJSON_GetObjectItemCaseSensitive(item, "_airport_codes_iata");
    if (!cJSON_IsString(codes) || strcmp(codes->valuestring, "unknown") == 0) {
        codes = cJSON_GetObjectItemCaseSensitive(item, "airport_codes");
    }
    if (!cJSON_IsString(codes) || !codes->valuestring[0] || strcmp(codes->valuestring, "unknown") == 0) return;
    strlcpy(out, codes->valuestring, n);
    if (cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(item, "plausible"))) strlcat(out, " ?", n);
}

static bool lookup(const char *callsign, double lat, double lon, char *route, size_t n)
{
    static resp_t resp; // one task, so one buffer
    char body[128];
    snprintf(body, sizeof(body), "{\"planes\":[{\"callsign\":\"%s\",\"lat\":%.4f,\"lng\":%.4f}]}", callsign, lat,
             lon);

    resp.len = 0;
    resp.overflow = false;
    esp_http_client_config_t cfg = {
        .url = API,
        .method = HTTP_METHOD_POST,
        .event_handler = on_http_event,
        .user_data = &resp,
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
    if (err != ESP_OK || status != 200 || resp.overflow) {
        ESP_LOGW(TAG, "%s: %s, HTTP %d", callsign, esp_err_to_name(err), status);
        return false;
    }

    cJSON *root = cJSON_ParseWithLength(resp.data, resp.len);
    const cJSON *first = cJSON_IsArray(root) ? cJSON_GetArrayItem(root, 0) : NULL;
    bool ok = cJSON_IsObject(first);
    if (ok) format_route(first, route, n);
    cJSON_Delete(root);
    if (ok) ESP_LOGI(TAG, "%s: %s", callsign, route[0] ? route : "unknown");
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

        char route[ROUTE_MAX] = "";
        bool ok = lookup(callsign, lat, lon, route, sizeof(route));

        xSemaphoreTake(s_lock, portMAX_DELAY);
        entry_t *e = find_or_add(callsign);
        e->used = ++s_clock;
        if (ok) {
            strlcpy(e->route, route, sizeof(e->route));
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
    s_lock = xSemaphoreCreateMutex();
    // TLS and the request are on the heap; this is call depth, as for OpenAIP
    if (xTaskCreateWithCaps(route_task, "route", 8192, NULL, 3, &s_task, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) !=
        pdPASS) {
        xTaskCreate(route_task, "route", 8192, NULL, 3, &s_task);
    }
}

void route_get(const char *callsign, double lat, double lon, char *out, size_t n)
{
    out[0] = '\0';
    if (!s_lock || !callsign[0]) return;
    // It goes into the request's JSON as it is
    for (const char *c = callsign; *c; c++) {
        if (!((*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9'))) return;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    entry_t *e = find(callsign);
    if (e) e->used = ++s_clock;
    if (e && e->done) {
        strlcpy(out, e->route, n);
    } else if ((!e || !e->failed_us || esp_timer_get_time() - e->failed_us > RETRY_US) &&
               strcmp(s_want, callsign) != 0) {
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
}
