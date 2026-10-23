#include "weather.h"

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
#include "geo.h"
#include "net.h"
#include "settings.h"

static const char *TAG = "weather";

#define API "https://aviationweather.gov/api/data/metar"
#define RESP_MAX (32 * 1024) // a few stations' METARs, ~500 bytes each
#define RETRY_S 120          // after a failed request

// The runway in use: aircraft within RUNWAY_NM of the station, below
// RUNWAY_AGL_FT above it, climbing or descending, over the last SAMPLE_KEEP_US
#define RUNWAY_NM 6.0
#define RUNWAY_AGL_FT 3000
#define RUNWAY_VS_FPM 300
#define SAMPLES 16
#define SAMPLE_KEEP_US (20 * 60 * 1000000LL)
#define RUNWAY_AGREE 0.85 // how closely their tracks must agree: the mean resultant length

typedef struct {
    char hex[8];
    float track;
    int64_t at_us;
} sample_t;

typedef struct {
    char data[RESP_MAX];
    size_t len;
    bool overflow;
} resp_t;

static SemaphoreHandle_t s_lock;
static TaskHandle_t s_task;
static resp_t *s_resp; // PSRAM
// Under s_lock
static weather_t s_wx;
static double s_st_lat, s_st_lon; // the station
static int s_st_elev_ft;
static bool s_st_known;
static sample_t *s_arr, *s_dep; // SAMPLES each, in PSRAM: static internal RAM is short

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

static const char *str(const cJSON *o, const char *key)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsString(v) && v->valuestring ? v->valuestring : "";
}

// o's key when it is a number from lo to hi
static bool num(const cJSON *o, const char *key, double lo, double hi, double *out)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    if (!cJSON_IsNumber(v) || !isfinite(v->valuedouble) || v->valuedouble < lo || v->valuedouble > hi) return false;
    *out = v->valuedouble;
    return true;
}

// GET url into s_resp, parsed; NULL when it failed
static cJSON *get(const char *url)
{
    s_resp->len = 0;
    s_resp->overflow = false;
    esp_http_client_config_t cfg = {
        .url = url,
        .event_handler = on_http_event,
        .user_data = s_resp,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 15000,
        .user_agent = "overhead-radar/1.0 (+https://github.com/sgmess/overhead)",
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return NULL;
    const esp_err_t err = esp_http_client_perform(c);
    const int status = esp_http_client_get_status_code(c);
    esp_http_client_cleanup(c);
    if (err != ESP_OK || status != 200 || s_resp->overflow) {
        ESP_LOGW(TAG, "%s: %s, HTTP %d%s", url, esp_err_to_name(err), status, s_resp->overflow ? ", too big" : "");
        return NULL;
    }
    return cJSON_ParseWithLength(s_resp->data, s_resp->len);
}

// One METAR into the weather, and where its station is
static bool parse(const cJSON *m, weather_t *w, double *lat, double *lon, int *elev_ft)
{
    const char *id = str(m, "icaoId");
    if (!id[0] || strlen(id) > 4) return false;
    *w = (weather_t){.valid = true};
    strlcpy(w->station, id, sizeof(w->station));
    // "London/Heathrow Intl, EN, GB": the name alone
    strlcpy(w->name, str(m, "name"), sizeof(w->name));
    char *comma = strchr(w->name, ',');
    if (comma) *comma = '\0';
    strlcpy(w->raw, str(m, "rawOb"), sizeof(w->raw));
    double v;
    if (num(m, "obsTime", 1e9, 4e9, &v)) w->obs = (time_t)v;
    const cJSON *wdir = cJSON_GetObjectItemCaseSensitive(m, "wdir");
    double spd = 0;
    num(m, "wspd", 0, 300, &spd);
    w->wind_kt = (int)lround(spd);
    if (num(m, "wgst", 0, 300, &v)) w->gust_kt = (int)lround(v);
    if (cJSON_IsString(wdir)) {
        w->wind_dir = WIND_VARIABLE; // "VRB"
    } else if (num(m, "wdir", 0, 360, &v)) {
        w->wind_dir = (int)lround(v);
    } else {
        w->wind_dir = WIND_VARIABLE;
    }
    if (w->wind_kt == 0) w->wind_dir = WIND_CALM;
    double t, d;
    if (num(m, "temp", -90, 60, &t) && num(m, "dewp", -90, 60, &d)) {
        w->has_temp = true;
        w->temp_c = (int)lround(t);
        w->dew_c = (int)lround(d);
    }
    if (num(m, "altim", 850, 1100, &v)) w->qnh = (int)lround(v);
    if (!num(m, "lat", -90, 90, lat) || !num(m, "lon", -180, 180, lon)) return false;
    *elev_ft = num(m, "elev", -500, 6000, &v) ? (int)lround(v * 3.28084) : 0;
    return true;
}

// The METAR from the station nearest (lat, lon), looking further out if
// none is near; or from station when one is set. What came, into w.
static bool fetch(const char *station, double lat, double lon, weather_t *w, double *st_lat, double *st_lon,
                  int *elev_ft)
{
    char url[160];
    for (int pass = 0; pass < 2; pass++) {
        if (station[0]) {
            if (pass) break;
            snprintf(url, sizeof(url), API "?ids=%s&format=json", station);
        } else {
            const double dlat = pass ? 1.2 : 0.4, dlon = dlat / fmax(cos(lat * M_PI / 180), 0.2);
            snprintf(url, sizeof(url), API "?bbox=%.3f,%.3f,%.3f,%.3f&format=json", lat - dlat, lon - dlon,
                     lat + dlat, lon + dlon);
        }
        cJSON *root = get(url);
        if (!root) return false;
        double best = INFINITY;
        const cJSON *m;
        cJSON_ArrayForEach(m, root)
        {
            weather_t c;
            double clat, clon;
            int celev;
            if (!cJSON_IsObject(m) || !parse(m, &c, &clat, &clon, &celev)) continue;
            const double dx = lon_diff(clon, lon) * cos(lat * M_PI / 180), dy = clat - lat;
            const double d = dx * dx + dy * dy;
            if (d < best) {
                best = d;
                *w = c;
                *st_lat = clat;
                *st_lon = clon;
                *elev_ft = celev;
            }
        }
        cJSON_Delete(root);
        if (isfinite(best)) return true;
    }
    snprintf(w->detail, sizeof(w->detail), station[0] ? "No METAR from %s" : "No station reporting nearby", station);
    return false;
}

static void weather_task(void *arg)
{
    settings_t *s = arg; // too big for its stack; in PSRAM
    for (;;) {
        settings_copy(s);
        bool ok = false;
        if (!s->show_weather) {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            s_wx = (weather_t){0};
            s_st_known = false;
            xSemaphoreGive(s_lock);
        } else if (net_is_connected()) {
            weather_t w = {0};
            double lat = 0, lon = 0;
            int elev = 0;
            ok = fetch(s->wx_station, s->lat, s->lon, &w, &lat, &lon, &elev);
            xSemaphoreTake(s_lock, portMAX_DELAY);
            if (ok) {
                // A new station's runway samples are another airfield's
                if (strcmp(w.station, s_wx.station) != 0) {
                    memset(s_arr, 0, SAMPLES * sizeof(*s_arr));
                    memset(s_dep, 0, SAMPLES * sizeof(*s_dep));
                }
                w.landing = s_wx.landing;
                w.departing = s_wx.departing;
                s_wx = w;
                s_st_lat = lat;
                s_st_lon = lon;
                s_st_elev_ft = elev;
                s_st_known = true;
                ESP_LOGI(TAG, "%s", s_wx.raw);
            } else if (!s_wx.valid) {
                strlcpy(s_wx.detail, w.detail[0] ? w.detail : "No answer from aviationweather.gov",
                        sizeof(s_wx.detail));
            }
            xSemaphoreGive(s_lock);
        }
        const int wait_s = ok || !s->show_weather ? WEATHER_EVERY_S : net_is_connected() ? RETRY_S : 10;
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(wait_s * 1000));
    }
}

void weather_start(void)
{
    s_resp = heap_caps_malloc(sizeof(resp_t), MALLOC_CAP_SPIRAM);
    s_arr = heap_caps_calloc(SAMPLES, sizeof(sample_t), MALLOC_CAP_SPIRAM);
    s_dep = heap_caps_calloc(SAMPLES, sizeof(sample_t), MALLOC_CAP_SPIRAM);
    settings_t *set = heap_caps_malloc(sizeof(settings_t), MALLOC_CAP_SPIRAM);
    if (!s_resp || !s_arr || !s_dep || !set) {
        ESP_LOGE(TAG, "not started: no memory");
        return; // s_lock unset: weather_get() says nothing, weather_traffic() does nothing
    }
    SemaphoreHandle_t lock = xSemaphoreCreateMutex();
    if (lock) s_lock = lock; // before the task: it takes it at once
    // TLS and the request are on the heap; this is call depth, as for route.c
    if (lock &&
        (xTaskCreateWithCaps(weather_task, "weather", 8192, set, 2, &s_task, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) ==
             pdPASS ||
         xTaskCreate(weather_task, "weather", 8192, set, 2, &s_task) == pdPASS)) {
        return;
    }
    ESP_LOGE(TAG, "not started: no memory");
    s_task = NULL;
}

void weather_refresh(void)
{
    if (s_task) xTaskNotifyGive(s_task);
}

void weather_get(weather_t *out)
{
    *out = (weather_t){0};
    if (!s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_wx;
    xSemaphoreGive(s_lock);
}

// ---------------------------------------------------------------- the runway

// a's track into ring: one sample an aircraft, the newest, or over the oldest
static void sample(sample_t ring[SAMPLES], const aircraft_t *a, int64_t now)
{
    sample_t *slot = &ring[0];
    for (int k = 0; k < SAMPLES; k++) {
        if (strcmp(ring[k].hex, a->hex) == 0) {
            slot = &ring[k];
            break;
        }
        if (ring[k].at_us < slot->at_us) slot = &ring[k];
    }
    strlcpy(slot->hex, a->hex, sizeof(slot->hex));
    slot->track = a->track_deg;
    slot->at_us = now;
}

// The runway the recent samples agree on, 1..36, or 0
static int runway(const sample_t ring[SAMPLES], int64_t now)
{
    double sx = 0, sy = 0;
    int n = 0;
    for (int k = 0; k < SAMPLES; k++) {
        if (!ring[k].hex[0] || now - ring[k].at_us > SAMPLE_KEEP_US) continue;
        const double t = ring[k].track * M_PI / 180;
        sx += sin(t);
        sy += cos(t);
        n++;
    }
    if (n < 2 || hypot(sx, sy) / n < RUNWAY_AGREE) return 0;
    const int rwy = (int)lround(fmod(atan2(sx, sy) * 180 / M_PI + 360, 360) / 10);
    return rwy == 0 ? 36 : rwy;
}

void weather_traffic(const aircraft_t *list, int n)
{
    if (!s_lock) return;
    const int64_t now = esp_timer_get_time();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_st_known) {
        const double coslat = cos(s_st_lat * M_PI / 180);
        for (int i = 0; i < n; i++) {
            const aircraft_t *a = &list[i];
            if (a->on_ground || a->alt_ft == AC_ALT_UNKNOWN || a->track_deg < 0 || a->gs_kt < 80 || a->gs_kt > 250) {
                continue;
            }
            if (a->alt_ft - s_st_elev_ft > RUNWAY_AGL_FT || abs(a->vs_fpm) < RUNWAY_VS_FPM) continue;
            const double dx = lon_diff(a->lon, s_st_lon) * 60 * coslat, dy = (a->lat - s_st_lat) * 60;
            if (dx * dx + dy * dy > RUNWAY_NM * RUNWAY_NM) continue;
            sample(a->vs_fpm < 0 ? s_arr : s_dep, a, now);
        }
        s_wx.landing = runway(s_arr, now);
        s_wx.departing = runway(s_dep, now);
    }
    xSemaphoreGive(s_lock);
}
