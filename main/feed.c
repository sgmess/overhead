#include "feed.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "feed";

#define RESP_MAX (4 * 1024 * 1024)

typedef struct {
    const char *name;
    const char *url_fmt; // lat, lon, radius
    const char *list_key;
} feed_def_t;

// Both services return readsb-style JSON; only the URL and list key differ.
static const feed_def_t FEEDS[] = {
    {"adsb.lol", "https://api.adsb.lol/v2/point/%.4f/%.4f/%d", "ac"},
    {"adsb.fi", "https://opendata.adsb.fi/api/v2/lat/%.4f/lon/%.4f/dist/%d", "aircraft"},
};
#define FEED_COUNT (sizeof(FEEDS) / sizeof(FEEDS[0]))

#if CONFIG_OVERHEAD_FEED_ADSB_FI
static int s_feed = 1;
#else
static int s_feed = 0;
#endif

typedef struct {
    char *data;
    size_t len;
    size_t cap;
    bool overflow;
} resp_buf_t;

static resp_buf_t s_resp;
static esp_http_client_handle_t s_client;
static int s_client_feed = -1;

// A 100 NM response is several hundred KB and cJSON makes one small node per
// value, so keep all of it in PSRAM rather than the scarce internal heap.
static void *psram_malloc(size_t size)
{
    return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

static esp_err_t on_http_event(esp_http_client_event_t *evt)
{
    resp_buf_t *b = evt->user_data;
    if (evt->event_id != HTTP_EVENT_ON_DATA || b->overflow) {
        return ESP_OK;
    }
    size_t need = b->len + evt->data_len + 1;
    if (need > RESP_MAX) {
        b->overflow = true;
        return ESP_OK;
    }
    if (need > b->cap) {
        size_t cap = b->cap ? b->cap : 64 * 1024;
        while (cap < need) cap *= 2;
        char *p = heap_caps_realloc(b->data, cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
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

static void drop_client(void)
{
    if (s_client) {
        esp_http_client_cleanup(s_client);
        s_client = NULL;
    }
    s_client_feed = -1;
}

// Keep one client per feed so consecutive polls reuse the TLS session.
static esp_http_client_handle_t get_client(const char *url)
{
    if (s_client && s_client_feed == s_feed) {
        esp_http_client_set_url(s_client, url);
        return s_client;
    }
    drop_client();
    esp_http_client_config_t cfg = {
        .url = url,
        .event_handler = on_http_event,
        .user_data = &s_resp,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 10000,
        .buffer_size = 4096,
        .keep_alive_enable = true,
        .user_agent = "overhead-radar/1.0 (ESP32-P4)",
    };
    s_client = esp_http_client_init(&cfg);
    s_client_feed = s_feed;
    return s_client;
}

static void copy_str(char *dst, size_t n, const cJSON *item)
{
    dst[0] = '\0';
    if (!cJSON_IsString(item)) return;
    strlcpy(dst, item->valuestring, n);
    // readsb pads callsigns with trailing spaces
    for (size_t i = strlen(dst); i > 0 && dst[i - 1] == ' '; i--) dst[i - 1] = '\0';
}

static double num_or(const cJSON *obj, const char *key, double fallback)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsNumber(v) ? v->valuedouble : fallback;
}

static bool parse_aircraft(const cJSON *j, int64_t now_us, aircraft_t *a)
{
    const cJSON *lat = cJSON_GetObjectItemCaseSensitive(j, "lat");
    const cJSON *lon = cJSON_GetObjectItemCaseSensitive(j, "lon");
    if (!cJSON_IsNumber(lat) || !cJSON_IsNumber(lon)) return false;

    memset(a, 0, sizeof(*a));
    a->lat = lat->valuedouble;
    a->lon = lon->valuedouble;
    copy_str(a->hex, sizeof(a->hex), cJSON_GetObjectItemCaseSensitive(j, "hex"));
    copy_str(a->callsign, sizeof(a->callsign), cJSON_GetObjectItemCaseSensitive(j, "flight"));
    copy_str(a->reg, sizeof(a->reg), cJSON_GetObjectItemCaseSensitive(j, "r"));
    copy_str(a->type, sizeof(a->type), cJSON_GetObjectItemCaseSensitive(j, "t"));
    copy_str(a->squawk, sizeof(a->squawk), cJSON_GetObjectItemCaseSensitive(j, "squawk"));

    // alt_baro is a number, or the string "ground"
    const cJSON *alt = cJSON_GetObjectItemCaseSensitive(j, "alt_baro");
    if (cJSON_IsNumber(alt)) {
        a->alt_ft = (int32_t)alt->valuedouble;
    } else if (cJSON_IsString(alt) && strcmp(alt->valuestring, "ground") == 0) {
        a->on_ground = true;
        a->alt_ft = 0;
    } else {
        a->alt_ft = AC_ALT_UNKNOWN;
    }

    a->gs_kt = (float)num_or(j, "gs", -1);
    a->track_deg = (float)num_or(j, "track", num_or(j, "true_heading", -1));
    a->vs_fpm = (int32_t)num_or(j, "baro_rate", num_or(j, "geom_rate", 0));

    double seen_pos = num_or(j, "seen_pos", 0);
    a->pos_time_us = now_us - (int64_t)(seen_pos * 1e6);

    const cJSON *em = cJSON_GetObjectItemCaseSensitive(j, "emergency");
    a->emergency = (cJSON_IsString(em) && strcmp(em->valuestring, "none") != 0) ||
                   strcmp(a->squawk, "7500") == 0 || strcmp(a->squawk, "7600") == 0 ||
                   strcmp(a->squawk, "7700") == 0;
    return true;
}

esp_err_t feed_fetch(double lat, double lon, int radius_nm,
                     aircraft_t *out, int max, int *count)
{
    static bool hooks_set;
    if (!hooks_set) {
        cJSON_Hooks hooks = {.malloc_fn = psram_malloc, .free_fn = free};
        cJSON_InitHooks(&hooks);
        hooks_set = true;
    }

    const feed_def_t *f = &FEEDS[s_feed];
    char url[160];
    snprintf(url, sizeof(url), f->url_fmt, lat, lon, radius_nm);

    s_resp.len = 0;
    s_resp.overflow = false;
    esp_http_client_handle_t c = get_client(url);
    esp_err_t err = c ? esp_http_client_perform(c) : ESP_ERR_NO_MEM;
    int status = c ? esp_http_client_get_status_code(c) : 0;

    if (err != ESP_OK || status != 200 || s_resp.overflow || s_resp.len == 0) {
        ESP_LOGW(TAG, "%s failed: %s, HTTP %d, %u bytes%s", f->name, esp_err_to_name(err),
                 status, (unsigned)s_resp.len, s_resp.overflow ? " (overflow)" : "");
        drop_client();
        s_feed = (s_feed + 1) % FEED_COUNT;
        return err != ESP_OK ? err : ESP_FAIL;
    }

    int64_t now_us = esp_timer_get_time();
    cJSON *root = cJSON_ParseWithLength(s_resp.data, s_resp.len);
    const cJSON *list = cJSON_GetObjectItemCaseSensitive(root, f->list_key);
    if (!cJSON_IsArray(list)) {
        ESP_LOGW(TAG, "%s: no '%s' array in response", f->name, f->list_key);
        cJSON_Delete(root);
        s_feed = (s_feed + 1) % FEED_COUNT;
        return ESP_ERR_INVALID_RESPONSE;
    }

    int n = 0;
    const cJSON *item;
    cJSON_ArrayForEach(item, list) {
        if (n >= max) break;
        if (parse_aircraft(item, now_us, &out[n])) n++;
    }
    cJSON_Delete(root);
    *count = n;
    ESP_LOGI(TAG, "%s: %d aircraft within %d NM (%u bytes)", f->name, n, radius_nm,
             (unsigned)s_resp.len);
    return ESP_OK;
}

const char *feed_name(void)
{
    return FEEDS[s_feed].name;
}
