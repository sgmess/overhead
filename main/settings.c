#include "settings.h"

#include <math.h>
#include <string.h>

#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "settings";

#define NS "overhead"

static settings_t s_settings;

// Before anything is saved: no network (so the setup portal opens) and the
// radar centred on Heathrow. Everything here can be changed on the
// configuration page.
void settings_defaults(settings_t *out)
{
    *out = (settings_t){
        .lat = 51.4700,
        .lon = -0.4543,
        .range_idx = 2, // 25 NM
        .feed = 0,      // adsb.lol, with adsb.fi as the fallback
        .fetch_s = 5,
        .show_ground = true,
        .sweep = true,
#if defined(OVERHEAD_BOARD_S3_28C)
        .max_labels = 25, // a smaller scope
#else
        .max_labels = 40,
#endif
        // The Tab5's NP-F550 latches into protection below 6.0 V and has to
        // be refitted before it charges again
        .batt_auto_off = true,
        .batt_shutdown_mv = 6300,
        .rotation = 0, // the Tab5 in portrait, its panel's native way up
        .show_airspace = true,
        .show_airfields = true,
        .brightness = 100,
        .dim_mode = 1, // sunset to sunrise
        .dim_brightness = 30,
        .dim_from = 22 * 60,
        .dim_to = 7 * 60,
        .clock = 0, // UTC until a timezone is chosen
        .tag_fields = TAG_CALLSIGN | TAG_ALT,
#if defined(OVERHEAD_BOARD_S3_28C)
        .trail_s = 0, // every line drawn is more PSRAM traffic against the scanout
#else
        .trail_s = 120,
#endif
        .show_military = true,
        .show_route = true,
        .alert = false, // near an airport it would never stop
        .alert_nm10 = 20,
        .alert_ft = 5000,
        .alert_sound = true,
    };
}

// A POSIX TZ string: names, offsets and rules, nothing that could upset the
// clock's printf or the page that shows it again
static bool valid_tz(const char *s)
{
    for (; *s; s++) {
        if (!((*s >= 'A' && *s <= 'Z') || (*s >= 'a' && *s <= 'z') || (*s >= '0' && *s <= '9') ||
              strchr("<>+-,:./", *s))) {
            return false;
        }
    }
    return true;
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
           (s->rotation == 0 || s->rotation == 90 || s->rotation == 270) &&
           s->brightness >= 5 && s->brightness <= 100 &&
           s->dim_mode >= 0 && s->dim_mode <= 2 &&
           s->dim_brightness >= 1 && s->dim_brightness <= 100 &&
           s->dim_from >= 0 && s->dim_from < 24 * 60 && s->dim_to >= 0 && s->dim_to < 24 * 60 &&
           strlen(s->tz) < sizeof(s->tz) && valid_tz(s->tz) &&
           s->clock >= 0 && s->clock <= 2 &&
           s->tag_fields >= 1 && s->tag_fields <= TAG_ALL &&
           s->trail_s >= 0 && s->trail_s <= 600 &&
           s->alert_nm10 >= 2 && s->alert_nm10 <= 100 &&
           s->alert_ft >= 500 && s->alert_ft <= 20000;
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
        get_int(h, "bright", &s.brightness);
        get_int(h, "dim_mode", &s.dim_mode);
        get_int(h, "dim_bright", &s.dim_brightness);
        get_int(h, "dim_from", &s.dim_from);
        get_int(h, "dim_to", &s.dim_to);
        get_str(h, "tz", s.tz, sizeof(s.tz));
        get_int(h, "clock", &s.clock);
        get_int(h, "tag", &s.tag_fields);
        get_int(h, "trail", &s.trail_s);
        get_bool(h, "military", &s.show_military);
        get_bool(h, "route", &s.show_route);
        get_bool(h, "alert", &s.alert);
        get_int(h, "alert_nm10", &s.alert_nm10);
        get_int(h, "alert_ft", &s.alert_ft);
        get_bool(h, "alert_snd", &s.alert_sound);
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
    if (err == ESP_OK) err = nvs_set_i32(h, "bright", s->brightness);
    if (err == ESP_OK) err = nvs_set_i32(h, "dim_mode", s->dim_mode);
    if (err == ESP_OK) err = nvs_set_i32(h, "dim_bright", s->dim_brightness);
    if (err == ESP_OK) err = nvs_set_i32(h, "dim_from", s->dim_from);
    if (err == ESP_OK) err = nvs_set_i32(h, "dim_to", s->dim_to);
    if (err == ESP_OK) err = nvs_set_str(h, "tz", s->tz);
    if (err == ESP_OK) err = nvs_set_i32(h, "clock", s->clock);
    if (err == ESP_OK) err = nvs_set_i32(h, "tag", s->tag_fields);
    if (err == ESP_OK) err = nvs_set_i32(h, "trail", s->trail_s);
    if (err == ESP_OK) err = nvs_set_u8(h, "military", s->show_military);
    if (err == ESP_OK) err = nvs_set_u8(h, "route", s->show_route);
    if (err == ESP_OK) err = nvs_set_u8(h, "alert", s->alert);
    if (err == ESP_OK) err = nvs_set_i32(h, "alert_nm10", s->alert_nm10);
    if (err == ESP_OK) err = nvs_set_i32(h, "alert_ft", s->alert_ft);
    if (err == ESP_OK) err = nvs_set_u8(h, "alert_snd", s->alert_sound);
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
