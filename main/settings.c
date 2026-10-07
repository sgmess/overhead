#include "settings.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "settings";

#define NS "overhead"

static settings_t s_settings;

void settings_defaults(settings_t *out)
{
    memset(out, 0, sizeof(*out));
    strlcpy(out->ssid, CONFIG_OVERHEAD_WIFI_SSID, sizeof(out->ssid));
    strlcpy(out->password, CONFIG_OVERHEAD_WIFI_PASSWORD, sizeof(out->password));
    out->lat = strtod(CONFIG_OVERHEAD_CENTER_LAT, NULL);
    out->lon = strtod(CONFIG_OVERHEAD_CENTER_LON, NULL);
    out->range_idx = CONFIG_OVERHEAD_DEFAULT_RANGE_INDEX;
#ifdef CONFIG_OVERHEAD_FEED_ADSB_FI
    out->feed = 1;
#endif
    out->fetch_s = CONFIG_OVERHEAD_FETCH_INTERVAL_SEC;
#ifdef CONFIG_OVERHEAD_SHOW_GROUND
    out->show_ground = true;
#endif
#ifdef CONFIG_OVERHEAD_SWEEP
    out->sweep = true;
#endif
    out->max_labels = CONFIG_OVERHEAD_MAX_LABELS;
#ifdef CONFIG_OVERHEAD_BATTERY_AUTO_OFF
    out->batt_auto_off = true;
#endif
#ifdef CONFIG_OVERHEAD_BATTERY_SHUTDOWN_MV
    out->batt_shutdown_mv = CONFIG_OVERHEAD_BATTERY_SHUTDOWN_MV;
#else
    out->batt_shutdown_mv = 6300;
#endif
    out->show_airspace = true;
    out->show_airfields = true;
#if CONFIG_OVERHEAD_TAB5_LANDSCAPE
    out->rotation = 90;
#elif CONFIG_OVERHEAD_TAB5_LANDSCAPE_FLIPPED
    out->rotation = 270;
#endif
}

// Two-letter codes separated by commas, or nothing
static bool valid_countries(const char *s)
{
    for (int i = 0; s[i]; i++) {
        bool letter = s[i] >= 'A' && s[i] <= 'Z';
        if (i % 3 == 2 ? s[i] != ',' : !letter) return false;
    }
    size_t n = strlen(s);
    return n == 0 || n % 3 == 2;
}

bool settings_valid(const settings_t *s)
{
    return strlen(s->password) < sizeof(s->password) && strlen(s->openaip_key) < sizeof(s->openaip_key) &&
           strlen(s->openaip_countries) < sizeof(s->openaip_countries) && valid_countries(s->openaip_countries) &&
           isfinite(s->lat) && s->lat >= -90 && s->lat <= 90 &&
           isfinite(s->lon) && s->lon >= -180 && s->lon <= 180 &&
           s->range_idx >= 0 && s->range_idx < SETTINGS_RANGE_COUNT &&
           (s->feed == 0 || s->feed == 1) &&
           s->fetch_s >= 2 && s->fetch_s <= 60 &&
           s->max_labels >= 0 && s->max_labels <= 200 &&
           s->batt_shutdown_mv >= 6100 && s->batt_shutdown_mv <= 7400 &&
           (s->rotation == 0 || s->rotation == 90 || s->rotation == 270);
}

// Each getter leaves *v alone when the key is missing, so it keeps its default
static void get_str(nvs_handle_t h, const char *key, char *v, size_t n)
{
    size_t len = n;
    nvs_get_str(h, key, v, &len);
}

static void get_int(nvs_handle_t h, const char *key, int *v)
{
    int32_t x;
    if (nvs_get_i32(h, key, &x) == ESP_OK) *v = x;
}

static void get_bool(nvs_handle_t h, const char *key, bool *v)
{
    uint8_t x;
    if (nvs_get_u8(h, key, &x) == ESP_OK) *v = x;
}

// NVS has no doubles; microdegrees are about 0.1 m
static void get_deg(nvs_handle_t h, const char *key, double *v)
{
    int32_t x;
    if (nvs_get_i32(h, key, &x) == ESP_OK) *v = x / 1e6;
}

void settings_load(void)
{
    settings_t s;
    settings_defaults(&s);

    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) == ESP_OK) {
        get_str(h, "ssid", s.ssid, sizeof(s.ssid));
        get_str(h, "pass", s.password, sizeof(s.password));
        get_deg(h, "lat", &s.lat);
        get_deg(h, "lon", &s.lon);
        get_int(h, "range", &s.range_idx);
        get_int(h, "feed", &s.feed);
        get_int(h, "fetch", &s.fetch_s);
        get_bool(h, "ground", &s.show_ground);
        get_bool(h, "sweep", &s.sweep);
        get_int(h, "labels", &s.max_labels);
        get_bool(h, "auto_off", &s.batt_auto_off);
        get_int(h, "off_mv", &s.batt_shutdown_mv);
        get_int(h, "rotation", &s.rotation);
        get_str(h, "oaip_key", s.openaip_key, sizeof(s.openaip_key));
        get_str(h, "oaip_cc", s.openaip_countries, sizeof(s.openaip_countries));
        get_bool(h, "airspace", &s.show_airspace);
        get_bool(h, "airfields", &s.show_airfields);
        nvs_close(h);
    }

    // Something stored by an older build that this one rejects: start clean
    // rather than run with it
    if (!settings_valid(&s)) {
        ESP_LOGW(TAG, "saved settings out of range, using the defaults");
        settings_defaults(&s);
    }
    s_settings = s;
    ESP_LOGI(TAG, "network '%s', centre %.4f, %.4f", s.ssid, s.lat, s.lon);
}

const settings_t *settings(void)
{
    return &s_settings;
}

esp_err_t settings_save(const settings_t *s)
{
    if (!settings_valid(s)) return ESP_ERR_INVALID_ARG;

    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    // The empty SSID is stored too: it is what says "forgotten", over a
    // network built in from menuconfig
    err = nvs_set_str(h, "ssid", s->ssid);
    if (err == ESP_OK) err = nvs_set_str(h, "pass", s->password);
    if (err == ESP_OK) err = nvs_set_i32(h, "lat", (int32_t)lround(s->lat * 1e6));
    if (err == ESP_OK) err = nvs_set_i32(h, "lon", (int32_t)lround(s->lon * 1e6));
    if (err == ESP_OK) err = nvs_set_i32(h, "range", s->range_idx);
    if (err == ESP_OK) err = nvs_set_i32(h, "feed", s->feed);
    if (err == ESP_OK) err = nvs_set_i32(h, "fetch", s->fetch_s);
    if (err == ESP_OK) err = nvs_set_u8(h, "ground", s->show_ground);
    if (err == ESP_OK) err = nvs_set_u8(h, "sweep", s->sweep);
    if (err == ESP_OK) err = nvs_set_i32(h, "labels", s->max_labels);
    if (err == ESP_OK) err = nvs_set_u8(h, "auto_off", s->batt_auto_off);
    if (err == ESP_OK) err = nvs_set_i32(h, "off_mv", s->batt_shutdown_mv);
    if (err == ESP_OK) err = nvs_set_i32(h, "rotation", s->rotation);
    if (err == ESP_OK) err = nvs_set_str(h, "oaip_key", s->openaip_key);
    if (err == ESP_OK) err = nvs_set_str(h, "oaip_cc", s->openaip_countries);
    if (err == ESP_OK) err = nvs_set_u8(h, "airspace", s->show_airspace);
    if (err == ESP_OK) err = nvs_set_u8(h, "airfields", s->show_airfields);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err == ESP_OK) s_settings = *s;
    return err;
}

esp_err_t settings_erase(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_erase_all(h);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}
