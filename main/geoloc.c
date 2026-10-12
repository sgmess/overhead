#include "geoloc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "settings.h"
#include "zones.h"

static const char *TAG = "geoloc";

#define RESP_MAX 2048

typedef struct {
    char data[RESP_MAX];
    size_t len;
    bool overflow;
} resp_t;

typedef struct {
    double lat, lon;
    char iana[48]; // "" when not given
    char city[48];
} place_t;

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

static void copy_str(const cJSON *item, char *out, size_t n)
{
    if (cJSON_IsString(item)) strlcpy(out, item->valuestring, n);
}

// ipinfo.io: "loc": "51.6094,0.0233", "timezone": "Europe/London"
static bool parse_ipinfo(const cJSON *root, place_t *p)
{
    const cJSON *loc = cJSON_GetObjectItemCaseSensitive(root, "loc");
    if (!cJSON_IsString(loc) || sscanf(loc->valuestring, "%lf,%lf", &p->lat, &p->lon) != 2) return false;
    copy_str(cJSON_GetObjectItemCaseSensitive(root, "timezone"), p->iana, sizeof(p->iana));
    copy_str(cJSON_GetObjectItemCaseSensitive(root, "city"), p->city, sizeof(p->city));
    return true;
}

// ipwho.is: "latitude", "longitude", "timezone": {"id": "Europe/London"}
static bool parse_ipwho(const cJSON *root, place_t *p)
{
    const cJSON *lat = cJSON_GetObjectItemCaseSensitive(root, "latitude");
    const cJSON *lon = cJSON_GetObjectItemCaseSensitive(root, "longitude");
    if (!cJSON_IsNumber(lat) || !cJSON_IsNumber(lon)) return false;
    p->lat = lat->valuedouble;
    p->lon = lon->valuedouble;
    const cJSON *tz = cJSON_GetObjectItemCaseSensitive(root, "timezone");
    copy_str(cJSON_GetObjectItemCaseSensitive(tz, "id"), p->iana, sizeof(p->iana));
    copy_str(cJSON_GetObjectItemCaseSensitive(root, "city"), p->city, sizeof(p->city));
    return true;
}

static const struct {
    const char *url;
    bool (*parse)(const cJSON *root, place_t *p);
} SERVICES[] = {
    {"https://ipinfo.io/json", parse_ipinfo}, // the closer of the two in testing
    {"https://ipwho.is/", parse_ipwho},
};

static bool ask(int i, resp_t *r, place_t *p)
{
    r->len = 0;
    r->overflow = false;
    esp_http_client_config_t cfg = {
        .url = SERVICES[i].url,
        .event_handler = on_http_event,
        .user_data = r,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 10000,
        .user_agent = "overhead-radar/1.0 (ESP32)",
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return false;
    esp_err_t err = esp_http_client_perform(c);
    int status = esp_http_client_get_status_code(c);
    esp_http_client_cleanup(c);
    if (err != ESP_OK || status != 200 || r->overflow) {
        ESP_LOGW(TAG, "%s: %s, HTTP %d", SERVICES[i].url, esp_err_to_name(err), status);
        return false;
    }
    cJSON *root = cJSON_ParseWithLength(r->data, r->len);
    *p = (place_t){0};
    bool ok = root && SERVICES[i].parse(root, p) && p->lat >= -90 && p->lat <= 90 && p->lon >= -180 &&
              p->lon <= 180 && !(p->lat == 0 && p->lon == 0);
    cJSON_Delete(root);
    if (!ok) ESP_LOGW(TAG, "%s: no position in the reply", SERVICES[i].url);
    return ok;
}

bool geoloc_locate(void)
{
    if (settings()->centre_set) return true;

    // In PSRAM, and only for as long as it takes
    resp_t *r = heap_caps_malloc(sizeof(*r), MALLOC_CAP_SPIRAM);
    if (!r) return false;
    place_t p;
    bool found = false;
    for (int i = 0; i < sizeof(SERVICES) / sizeof(SERVICES[0]) && !found; i++) found = ask(i, r, &p);
    free(r);
    if (!found) return false;

    // Chosen on the page while the request was out: that wins
    settings_t s = *settings();
    if (s.centre_set) return true;
    s.lat = p.lat;
    s.lon = p.lon;
    s.centre_set = true;
    int z = s.tz[0] ? -1 : zone_find_iana(p.iana);
    if (z >= 0) {
        strlcpy(s.tz, ZONES[z].posix, sizeof(s.tz));
        if (s.clock == 0) s.clock = 2; // UTC was only for want of a timezone
    }
    ESP_LOGI(TAG, "centre %.4f, %.4f (%s)%s%s", p.lat, p.lon, p.city[0] ? p.city : "?", z >= 0 ? ", timezone " : "",
             z >= 0 ? p.iana : "");
    return settings_change(&s, true) == ESP_OK;
}
