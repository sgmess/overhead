#include "web.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "backlight.h"
#include "board.h"
#include "battery.h"
#include "display_watch.h"
#include "aerodatabox.h"
#include "airlines.h"
#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "net.h"
#include "openaip.h"
#include "ota.h"
#include "settings.h"
#include "stats.h"
#include "zones.h"

static const char *TAG = "web";

#define BODY_MAX 2048 // the settings form is about 1 KB
#define SCAN_MAX 24
#define RESTART_DELAY_MS 1500 // lets the reply reach the browser first

extern const char setup_html_start[] asm("_binary_setup_html_start");
extern const char setup_html_end[] asm("_binary_setup_html_end");
extern const char config_html_start[] asm("_binary_config_html_start");
extern const char config_html_end[] asm("_binary_config_html_end");


// Every change ends in a restart, and the flash write happens just before it,
// in the esp_timer task: the server's own stack is in PSRAM, which is
// unreachable while flash is being written.
typedef enum {
    COMMIT_SAVE,
    COMMIT_ERASE,
} commit_t;

static commit_t s_commit;
static settings_t s_pending;
static esp_timer_handle_t s_commit_timer;

static void commit_and_restart(void *arg)
{
    esp_err_t err = s_commit == COMMIT_SAVE ? settings_save(&s_pending) : settings_erase();
    if (err != ESP_OK) ESP_LOGE(TAG, "settings not stored: %s", esp_err_to_name(err));
    esp_restart();
}

// Accept s, already checked with settings_valid(), or with NULL erase them all
static esp_err_t commit(const settings_t *s)
{
    if (esp_timer_is_active(s_commit_timer)) return ESP_ERR_INVALID_STATE;
    s_commit = s ? COMMIT_SAVE : COMMIT_ERASE;
    if (s) s_pending = *s;
    ESP_LOGW(TAG, "%s, restarting", s ? "settings changed" : "settings reset to defaults");
    return esp_timer_start_once(s_commit_timer, RESTART_DELAY_MS * 1000LL);
}

static bool in_setup(void)
{
    return net_setup_open(NULL, NULL) && !net_is_connected();
}

static esp_err_t send_html(httpd_req_t *req, const char *start, const char *end)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, start, end - start - 1); // EMBED_TXTFILES adds a NUL
}

static esp_err_t send_json(httpd_req_t *req, cJSON *json)
{
    char *text = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    if (!text) return httpd_resp_send_500(req);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_sendstr(req, text);
    free(text);
    return err;
}

static esp_err_t send_result(httpd_req_t *req, esp_err_t result, const char *error)
{
    cJSON *json = cJSON_CreateObject();
    cJSON_AddBoolToObject(json, "ok", result == ESP_OK);
    if (result != ESP_OK) {
        httpd_resp_set_status(req, result == ESP_ERR_INVALID_ARG ? "400 Bad Request" : "500 Internal Server Error");
        cJSON_AddStringToObject(json, "error", error ? error : esp_err_to_name(result));
    }
    return send_json(req, json);
}

// Every POST has to say it is JSON. A page on another site can post a form
// or text/plain to this device without asking, but not application/json: the
// browser asks first (a CORS preflight), and this server never says yes. So a
// site visited from the same network can't change the settings or forget the
// WiFi. The real handler is the URI's user_ctx.
static esp_err_t json_only(httpd_req_t *req)
{
    char type[64] = "";
    httpd_req_get_hdr_value_str(req, "Content-Type", type, sizeof(type));
    if (strncasecmp(type, "application/json", 16) != 0) {
        httpd_resp_set_status(req, "415 Unsupported Media Type");
        cJSON *json = cJSON_CreateObject();
        cJSON_AddBoolToObject(json, "ok", false);
        cJSON_AddStringToObject(json, "error", "Send Content-Type: application/json");
        return send_json(req, json);
    }
    return ((esp_err_t (*)(httpd_req_t *))req->user_ctx)(req);
}

// The request body as JSON, or NULL
static cJSON *read_json(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len > BODY_MAX) return NULL;
    char *body = malloc(req->content_len + 1);
    if (!body) return NULL;
    int got = 0, timeouts = 0;
    while (got < req->content_len) {
        int n = httpd_req_recv(req, body + got, req->content_len - got);
        // A few, then give up: one client that stops sending mid-body would
        // otherwise hold the only server task, and with it every page
        if (n == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < 3) continue;
        if (n <= 0) {
            free(body);
            return NULL;
        }
        got += n;
    }
    body[got] = '\0';
    cJSON *json = cJSON_Parse(body);
    free(body);
    return json;
}

static void take_int(const cJSON *json, const char *key, int *v)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(json, key);
    if (cJSON_IsNumber(item)) *v = item->valueint;
}

static void take_double(const cJSON *json, const char *key, double *v)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(json, key);
    if (cJSON_IsNumber(item)) *v = item->valuedouble;
}

// The centre, which counts as chosen once it differs from the one running:
// the pages send it back unchanged with everything else
static void take_centre(const cJSON *json, settings_t *s)
{
    const double lat = s->lat, lon = s->lon; // s starts as a copy of the running ones
    take_double(json, "lat", &s->lat);
    take_double(json, "lon", &s->lon);
    if (s->lat != lat || s->lon != lon) s->centre_set = true;
}

static void take_bool(const cJSON *json, const char *key, bool *v)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(json, key);
    if (cJSON_IsBool(item)) *v = cJSON_IsTrue(item);
}

// "14:05" in local time
static void add_time(cJSON *obj, const char *key, time_t t)
{
    struct tm tm;
    localtime_r(&t, &tm);
    char buf[8];
    strftime(buf, sizeof(buf), "%H:%M", &tm);
    cJSON_AddStringToObject(obj, key, buf);
}

// "Mon 13 Oct"
static void fmt_day(time_t t, char *out, size_t n)
{
    struct tm tm;
    localtime_r(&t, &tm);
    char wday[8], mon[8];
    strftime(wday, sizeof(wday), "%a", &tm);
    strftime(mon, sizeof(mon), "%b", &tm);
    snprintf(out, n, "%s %d %s", wday, tm.tm_mday, mon);
}

static void add_record(cJSON *obj, const char *key, const stats_record_t *r)
{
    if (!r->value) return;
    cJSON *rec = cJSON_AddObjectToObject(obj, key);
    cJSON_AddStringToObject(rec, "name", r->name);
    cJSON_AddNumberToObject(rec, "value", r->value);
    add_time(rec, "at", r->at);
}

// Today's traffic; nothing until the clock is set
static void add_stats(cJSON *json)
{
    stats_t st;
    stats_get(&st);
    if (!st.valid) return;
    cJSON *o = cJSON_AddObjectToObject(json, "stats");
    add_time(o, "since", st.since);
    cJSON_AddNumberToObject(o, "seen", st.seen);
    cJSON_AddNumberToObject(o, "military", st.military);
    cJSON_AddItemToObject(o, "hours", cJSON_CreateIntArray(st.hours, 24));
    add_record(o, "highest", &st.highest);
    add_record(o, "fastest", &st.fastest);

    // The top five, as one where an airline flies as two codes (BAW and
    // SHT, British Airways' Shuttle), most first
    struct {
        const char *name; // the code when it isn't a known airline
        int count;
    } top[STATS_TOP_AIRLINES];
    int n_top = 0;
    for (int k = 0; k < STATS_TOP_AIRLINES && st.airlines[k].count; k++) {
        const airline_t *known = airline_find(st.airlines[k].icao);
        const char *name = known ? known->name : st.airlines[k].icao;
        int j = 0;
        while (j < n_top && strcmp(top[j].name, name) != 0) j++;
        if (j == n_top) top[n_top++] = (typeof(top[0])){name, 0};
        top[j].count += st.airlines[k].count;
        while (j > 0 && top[j].count > top[j - 1].count) { // back into order
            const typeof(top[0]) t = top[j];
            top[j] = top[j - 1];
            top[--j] = t;
        }
    }
    cJSON *airlines = cJSON_AddArrayToObject(o, "airlines");
    for (int k = 0; k < n_top && k < 5; k++) {
        cJSON *al = cJSON_CreateObject();
        cJSON_AddStringToObject(al, "name", top[k].name);
        cJSON_AddNumberToObject(al, "count", top[k].count);
        cJSON_AddItemToArray(airlines, al);
    }

    // The days before
    if (st.week_days) {
        cJSON_AddNumberToObject(o, "week_avg", st.week_avg);
        cJSON_AddNumberToObject(o, "week_days", st.week_days);
    }
    char day[32];
    if (st.best_seen) {
        cJSON *best = cJSON_AddObjectToObject(o, "best");
        fmt_day(st.best_day, day, sizeof(day));
        cJSON_AddStringToObject(best, "day", day);
        cJSON_AddNumberToObject(best, "seen", st.best_seen);
    }
    cJSON *total = cJSON_AddObjectToObject(o, "total");
    cJSON_AddNumberToObject(total, "seen", (double)st.total_seen);
    cJSON_AddNumberToObject(total, "days", st.total_days);
    fmt_day(st.total_since, day, sizeof(day));
    cJSON_AddStringToObject(total, "since", day);
}

// ---------------------------------------------------------------- pages

static esp_err_t on_root(httpd_req_t *req)
{
    if (in_setup()) return send_html(req, setup_html_start, setup_html_end);
    return send_html(req, config_html_start, config_html_end);
}

static esp_err_t on_setup_page(httpd_req_t *req)
{
    return send_html(req, setup_html_start, setup_html_end);
}

// Phones check for a captive portal by fetching a known URL (Apple's
// hotspot-detect.html, Android's generate_204, Windows' connecttest.txt);
// the DNS sends those here, and anything but the expected reply makes them
// show the page it redirects to.
static esp_err_t on_not_found(httpd_req_t *req, httpd_err_code_t code)
{
    const char *url;
    if (!net_setup_open(NULL, &url)) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not found");
        return ESP_FAIL;
    }
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", url);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, NULL, 0);
}

// ---------------------------------------------------------------- API

static esp_err_t on_state(httpd_req_t *req)
{
    settings_t now;
    settings_copy(&now);
    const settings_t *s = &now;
    const esp_app_desc_t *app = esp_app_get_description();
    cJSON *json = cJSON_CreateObject();

    cJSON_AddStringToObject(json, "board", BOARD_NAME);
    cJSON_AddStringToObject(json, "firmware", app->version);
    char fw[96];
    ota_summary(fw, sizeof(fw));
    cJSON_AddStringToObject(json, "firmware_status", fw);
    cJSON_AddStringToObject(json, "project", app->project_name);
    cJSON_AddStringToObject(json, "update_repo", CONFIG_OVERHEAD_OTA_REPO);
    cJSON_AddBoolToObject(json, "can_rotate", BOARD_CAN_ROTATE);
    cJSON_AddBoolToObject(json, "has_speaker", BOARD_HAS_SPEAKER);
    cJSON_AddBoolToObject(json, "dimmed", backlight_night());
    cJSON_AddBoolToObject(json, "asleep", backlight_asleep());
    add_stats(json);
    cJSON_AddNumberToObject(json, "uptime_s", (double)(esp_timer_get_time() / 1000000));
    cJSON_AddNumberToObject(json, "display_restarts", display_watch_restarts()); // since power-on
    cJSON_AddNumberToObject(json, "heap_internal", heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(json, "heap_psram", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    const char *setup_ssid;
    cJSON_AddBoolToObject(json, "setup", in_setup());
    if (net_setup_open(&setup_ssid, NULL)) cJSON_AddStringToObject(json, "setup_ssid", setup_ssid);
    char ip[16];
    if (net_get_ip(ip, sizeof(ip))) {
        cJSON_AddStringToObject(json, "ip", ip);
        cJSON_AddNumberToObject(json, "rssi", net_rssi());
    }
    cJSON_AddStringToObject(json, "hostname", NET_HOSTNAME ".local");

    battery_status_t batt;
    const bool has_battery = battery_get(&batt);
    cJSON_AddBoolToObject(json, "has_battery", has_battery);
    if (has_battery) {
        static const char *const STATES[] = {"No battery", "Checking", "External power", "Charging", "On battery"};
        cJSON *b = cJSON_AddObjectToObject(json, "battery");
        cJSON_AddStringToObject(b, "state", STATES[batt.state]);
        if (batt.state >= BATT_EXTERNAL) {
            cJSON_AddNumberToObject(b, "percent", batt.percent);
            cJSON_AddNumberToObject(b, "volts", (int)(batt.volts * 100 + 0.5f) / 100.0);
        }
    }

    // Never the password itself, only whether there is one
    cJSON *set = cJSON_AddObjectToObject(json, "settings");
    cJSON_AddStringToObject(set, "ssid", s->ssid);
    cJSON_AddBoolToObject(set, "has_password", s->password[0] != '\0');
    cJSON_AddNumberToObject(set, "lat", s->lat);
    cJSON_AddNumberToObject(set, "lon", s->lon);
    cJSON_AddBoolToObject(set, "centre_set", s->centre_set);
    cJSON_AddNumberToObject(set, "range_idx", s->range_idx);
    cJSON_AddNumberToObject(set, "feed", s->feed);
    cJSON_AddNumberToObject(set, "fetch_s", s->fetch_s);
    cJSON_AddBoolToObject(set, "show_ground", s->show_ground);
    cJSON_AddBoolToObject(set, "sweep", s->sweep);
    cJSON_AddNumberToObject(set, "max_labels", s->max_labels);
    cJSON_AddBoolToObject(set, "batt_auto_off", s->batt_auto_off);
    cJSON_AddNumberToObject(set, "batt_shutdown_mv", s->batt_shutdown_mv);
    cJSON_AddNumberToObject(set, "rotation", s->rotation);
    cJSON_AddBoolToObject(set, "has_openaip_key", s->openaip_key[0] != '\0'); // nor this key
    cJSON_AddStringToObject(set, "openaip_countries", s->openaip_countries);
    cJSON_AddBoolToObject(set, "show_airspace", s->show_airspace);
    cJSON_AddBoolToObject(set, "show_airfields", s->show_airfields);
    cJSON_AddNumberToObject(set, "brightness", s->brightness);
    cJSON_AddNumberToObject(set, "dim_mode", s->dim_mode);
    cJSON_AddNumberToObject(set, "dim_brightness", s->dim_brightness);
    cJSON_AddNumberToObject(set, "dim_from", s->dim_from);
    cJSON_AddNumberToObject(set, "dim_to", s->dim_to);
    cJSON_AddStringToObject(set, "tz", s->tz);
    cJSON_AddNumberToObject(set, "clock", s->clock);
    cJSON_AddNumberToObject(set, "tag_fields", s->tag_fields);
    cJSON_AddNumberToObject(set, "trail_s", s->trail_s);
    cJSON_AddBoolToObject(set, "show_military", s->show_military);
    cJSON_AddBoolToObject(set, "show_route", s->show_route);
    cJSON_AddBoolToObject(set, "alert", s->alert);
    cJSON_AddNumberToObject(set, "alert_nm10", s->alert_nm10);
    cJSON_AddNumberToObject(set, "alert_ft", s->alert_ft);
    cJSON_AddBoolToObject(set, "alert_sound", s->alert_sound);
    cJSON_AddBoolToObject(set, "show_vector", s->show_vector);
    cJSON_AddNumberToObject(set, "filter_min_ft", s->filter_min_ft);
    cJSON_AddNumberToObject(set, "filter_max_ft", s->filter_max_ft);
    cJSON_AddNumberToObject(set, "show_kinds", s->show_kinds);
    cJSON_AddBoolToObject(set, "emerg_alert", s->emerg_alert);
    cJSON_AddBoolToObject(set, "show_centrelines", s->show_centrelines);
    cJSON_AddBoolToObject(set, "sleep", s->sleep);
    cJSON_AddNumberToObject(set, "sleep_from", s->sleep_from);
    cJSON_AddNumberToObject(set, "sleep_to", s->sleep_to);
    cJSON_AddBoolToObject(set, "has_adb_key", s->adb_key[0] != '\0'); // nor this one
    cJSON_AddNumberToObject(set, "adb_daily", s->adb_daily);

    openaip_status_t oa;
    openaip_get_status(&oa);
    cJSON *o = cJSON_AddObjectToObject(json, "openaip");
    cJSON_AddStringToObject(o, "state", oa.state);
    cJSON_AddStringToObject(o, "detail", oa.detail);
    cJSON_AddStringToObject(o, "countries", oa.countries);
    cJSON_AddNumberToObject(o, "airspaces", oa.airspaces);
    cJSON_AddNumberToObject(o, "airfields", oa.airfields);
    cJSON_AddNumberToObject(o, "age_s", oa.age_s);

    adb_status_t adb;
    adb_get_status(&adb);
    cJSON *ad = cJSON_AddObjectToObject(json, "aerodatabox");
    cJSON_AddStringToObject(ad, "state", adb.state);
    cJSON_AddStringToObject(ad, "detail", adb.detail);
    cJSON_AddNumberToObject(ad, "used_today", adb.used_today);
    cJSON_AddNumberToObject(ad, "lookups", adb.lookups);
    return send_json(req, json);
}

static esp_err_t on_scan(httpd_req_t *req)
{
    net_ap_t *aps = calloc(SCAN_MAX, sizeof(*aps));
    if (!aps) return httpd_resp_send_500(req);
    const int n = net_scan(aps, SCAN_MAX); // blocks for a couple of seconds
    if (n < 0) {
        free(aps);
        return send_result(req, ESP_FAIL, "Scan failed, try again");
    }
    cJSON *list = cJSON_CreateArray();
    for (int i = 0; i < n; i++) {
        cJSON *ap = cJSON_CreateObject();
        cJSON_AddStringToObject(ap, "ssid", aps[i].ssid);
        cJSON_AddNumberToObject(ap, "rssi", aps[i].rssi);
        cJSON_AddBoolToObject(ap, "open", aps[i].open);
        cJSON_AddItemToArray(list, ap);
    }
    free(aps);
    cJSON *json = cJSON_CreateObject();
    cJSON_AddBoolToObject(json, "ok", true);
    cJSON_AddItemToObject(json, "networks", list);
    return send_json(req, json);
}

// The setup page: network, password and, optionally, the radar centre;
// without one it is found from the internet address once connected
static esp_err_t on_wifi(httpd_req_t *req)
{
    cJSON *json = read_json(req);
    const cJSON *ssid = cJSON_GetObjectItemCaseSensitive(json, "ssid");
    const cJSON *pass = cJSON_GetObjectItemCaseSensitive(json, "password");
    if (!cJSON_IsString(ssid) || !ssid->valuestring[0] || strlen(ssid->valuestring) > 32 ||
        (pass && (!cJSON_IsString(pass) || strlen(pass->valuestring) > 64))) {
        cJSON_Delete(json);
        return send_result(req, ESP_ERR_INVALID_ARG, "Network name of 1-32 characters, password of up to 64");
    }
    settings_t s;
    settings_copy(&s);
    strlcpy(s.ssid, ssid->valuestring, sizeof(s.ssid));
    strlcpy(s.password, pass ? pass->valuestring : "", sizeof(s.password));
    take_centre(json, &s);
    cJSON_Delete(json);

    if (!settings_valid(&s)) return send_result(req, ESP_ERR_INVALID_ARG, "Latitude or longitude out of range");
    ESP_LOGI(TAG, "network set to '%s'", s.ssid);
    return send_result(req, commit(&s), NULL);
}

// json's key, if there, into out: printable, no spaces; false if not
static bool take_key(const cJSON *json, const char *name, char *out, size_t n)
{
    const cJSON *key = cJSON_GetObjectItemCaseSensitive(json, name);
    if (!key) return true;
    bool ok = cJSON_IsString(key) && strlen(key->valuestring) < n;
    for (const char *c = ok ? key->valuestring : ""; *c; c++) ok = ok && *c > ' ' && *c < 0x7f;
    if (ok) strlcpy(out, key->valuestring, n);
    return ok;
}

// The configuration page; any setting left out keeps its value
static esp_err_t on_settings(httpd_req_t *req)
{
    cJSON *json = read_json(req);
    if (!json) return send_result(req, ESP_ERR_INVALID_ARG, "Expected JSON");
    settings_t s;
    settings_copy(&s);
    take_centre(json, &s);
    take_int(json, "range_idx", &s.range_idx);
    take_int(json, "feed", &s.feed);
    take_int(json, "fetch_s", &s.fetch_s);
    take_bool(json, "show_ground", &s.show_ground);
    take_bool(json, "sweep", &s.sweep);
    take_int(json, "max_labels", &s.max_labels);
    take_bool(json, "batt_auto_off", &s.batt_auto_off);
    take_int(json, "batt_shutdown_mv", &s.batt_shutdown_mv);
    if (BOARD_CAN_ROTATE) take_int(json, "rotation", &s.rotation);
    take_bool(json, "show_airspace", &s.show_airspace);
    take_bool(json, "show_airfields", &s.show_airfields);
    take_int(json, "brightness", &s.brightness);
    take_int(json, "dim_mode", &s.dim_mode);
    take_int(json, "dim_brightness", &s.dim_brightness);
    take_int(json, "dim_from", &s.dim_from);
    take_int(json, "dim_to", &s.dim_to);
    take_int(json, "clock", &s.clock);
    take_int(json, "tag_fields", &s.tag_fields);
    take_int(json, "trail_s", &s.trail_s);
    take_bool(json, "show_military", &s.show_military);
    take_bool(json, "show_route", &s.show_route);
    take_bool(json, "alert", &s.alert);
    take_int(json, "alert_nm10", &s.alert_nm10);
    take_int(json, "alert_ft", &s.alert_ft);
    take_bool(json, "alert_sound", &s.alert_sound);
    take_bool(json, "show_vector", &s.show_vector);
    take_int(json, "filter_min_ft", &s.filter_min_ft);
    take_int(json, "filter_max_ft", &s.filter_max_ft);
    take_int(json, "show_kinds", &s.show_kinds);
    take_bool(json, "emerg_alert", &s.emerg_alert);
    take_bool(json, "show_centrelines", &s.show_centrelines);
    take_bool(json, "sleep", &s.sleep);
    take_int(json, "sleep_from", &s.sleep_from);
    take_int(json, "sleep_to", &s.sleep_to);
    take_int(json, "adb_daily", &s.adb_daily);
    const cJSON *tz = cJSON_GetObjectItemCaseSensitive(json, "tz");
    if (cJSON_IsString(tz)) {
        if (strlen(tz->valuestring) >= sizeof(s.tz)) {
            cJSON_Delete(json);
            return send_result(req, ESP_ERR_INVALID_ARG, "The timezone is up to 47 characters");
        }
        strlcpy(s.tz, tz->valuestring, sizeof(s.tz));
    }
    const cJSON *cc = cJSON_GetObjectItemCaseSensitive(json, "openaip_countries");
    if (cJSON_IsString(cc)) {
        // Normalised to "FR,CH": upper case, commas, no spaces
        char norm[sizeof(s.openaip_countries)] = "";
        size_t n = 0;
        for (const char *c = cc->valuestring; *c && n + 1 < sizeof(norm); c++) {
            if (*c == ' ') continue;
            norm[n++] = (*c >= 'a' && *c <= 'z') ? *c - 32 : *c;
        }
        norm[n] = '\0';
        strlcpy(s.openaip_countries, norm, sizeof(s.openaip_countries));
    }
    // Keys are only sent when typed or cleared, since the page never sees the old ones
    if (!take_key(json, "openaip_key", s.openaip_key, sizeof(s.openaip_key)) ||
        !take_key(json, "adb_key", s.adb_key, sizeof(s.adb_key))) {
        cJSON_Delete(json);
        return send_result(req, ESP_ERR_INVALID_ARG, "An API key is up to 64 characters, no spaces");
    }
    cJSON_Delete(json);

    if (!settings_valid(&s)) {
        return send_result(req, ESP_ERR_INVALID_ARG,
                           "A value is out of range (countries are two-letter codes, up to four; a timezone is a "
                           "POSIX TZ string such as GMT0BST,M3.5.0/1,M10.5.0)");
    }
    // Applied at once, except the orientation, which restarts. Waits until
    // applied, so the page's next look at the state shows it.
    const bool restart = settings_need_restart(&s);
    esp_err_t err = settings_change(&s, !restart);
    if (err != ESP_OK) return send_result(req, err, err == ESP_ERR_TIMEOUT ? "Saved, but still applying" : NULL);
    cJSON *reply = cJSON_CreateObject();
    cJSON_AddBoolToObject(reply, "ok", true);
    cJSON_AddBoolToObject(reply, "restart", restart);
    return send_json(req, reply);
}

#if CONFIG_LV_USE_SNAPSHOT
// For development, in a debug build (-D OVERHEAD_DEBUG=1, which turns LVGL's
// snapshot on; releases have none of this): the screen as raw
// RGB565, X-Size giving width, height and stride, and the settings screen or
// an aircraft's card (?hex=4010ee) opened without a finger; &emerg=1 makes
// that aircraft squawk 7700, &emerg=0 stops it, and &view=radar, big or
// list swipes. How the invisible sliders were found.
#include "lvgl.h"
#include "bench.h"
#include "radar_ui.h"
#include "settings_ui.h"
static esp_err_t on_debug_screen(httpd_req_t *req)
{
    board_lock();
    lv_draw_buf_t *b = lv_snapshot_take(lv_screen_active(), LV_COLOR_FORMAT_RGB565);
    board_unlock();
    if (!b) return httpd_resp_send_500(req);
    char hdr[48];
    snprintf(hdr, sizeof(hdr), "%u %u %u", (unsigned)b->header.w, (unsigned)b->header.h, (unsigned)b->header.stride);
    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "X-Size", hdr);
    esp_err_t err = httpd_resp_send(req, (const char *)b->data, b->header.stride * b->header.h);
    lv_draw_buf_destroy(b);
    return err;
}
static esp_err_t on_debug_settings(httpd_req_t *req)
{
    board_lock();
    settings_ui_open();
    board_unlock();
    return send_result(req, ESP_OK, NULL);
}
// Times the display for ?ms= (5 s) doing one of (?what=):
//   settings  scrolling the settings screen up and down
//   scope     the radar, its whole scope redrawn every frame
//   radar     the radar as it is, with nothing forced
//   big       the big view as it is
//   covered   the settings screen open and still, over the radar
// Then frames a second, the average and longest refresh, the share of the
// time refreshing and of that in the flush and waiting (see bench.h), the
// pixels drawn, the redraw requests, and internal RAM (now, and the least
// since boot).
static esp_err_t on_debug_bench(httpd_req_t *req)
{
    char query[48], ms_s[8] = "5000", what[12] = "settings";
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        httpd_query_key_value(query, "ms", ms_s, sizeof(ms_s));
        httpd_query_key_value(query, "what", what, sizeof(what));
    }
    const int ms = atoi(ms_s) > 0 ? atoi(ms_s) : 5000;
    const bool settings_open = strcmp(what, "settings") == 0 || strcmp(what, "covered") == 0;
    const bool big = strcmp(what, "big") == 0;
    if (!settings_open && !big && strcmp(what, "scope") != 0 && strcmp(what, "radar") != 0) {
        return send_result(req, ESP_ERR_INVALID_ARG, "what: settings, scope, radar, big or covered");
    }
    board_lock();
    // As a touch would: untouched for a minute, the settings screen closes,
    // and the screen goes dark overnight
    lv_display_trigger_activity(NULL);
    if (settings_open) {
        settings_ui_open();
    } else {
        settings_ui_debug_close();
        radar_ui_debug_view(big ? 1 : 0);
    }
    board_unlock();
    vTaskDelay(pdMS_TO_TICKS(500)); // drawn once first
    board_lock();
    if (strcmp(what, "settings") == 0) settings_ui_debug_scroll_start();
    if (strcmp(what, "scope") == 0) radar_ui_debug_stress(true);
    bench_start();
    board_unlock();
    vTaskDelay(pdMS_TO_TICKS(ms));
    bench_t b;
    board_lock();
    bench_stop(&b);
    settings_ui_debug_scroll_stop();
    radar_ui_debug_stress(false);
    if (settings_open) settings_ui_debug_close();
    if (big) radar_ui_debug_view(0);
    board_unlock();

    openaip_status_t aip;
    openaip_get_status(&aip);
    cJSON *reply = cJSON_CreateObject();
    cJSON_AddStringToObject(reply, "what", what);
    cJSON_AddNumberToObject(reply, "frames", b.frames);
    cJSON_AddNumberToObject(reply, "fps", b.frames * 1000.0 / ms);
    cJSON_AddNumberToObject(reply, "avg_ms", b.avg_us / 1000.0);
    cJSON_AddNumberToObject(reply, "max_ms", b.max_us / 1000.0);
    cJSON_AddNumberToObject(reply, "busy_pct", b.busy_pct);
    cJSON_AddNumberToObject(reply, "flush_pct", b.flush_pct);
    cJSON_AddNumberToObject(reply, "wait_pct", b.wait_pct);
    cJSON_AddNumberToObject(reply, "drawn_kpx", b.drawn_kpx);
    cJSON_AddNumberToObject(reply, "requests", b.requests);
    char biggest[48];
    snprintf(biggest, sizeof(biggest), "%dx%d at %d,%d", b.biggest_w, b.biggest_h, b.biggest_x, b.biggest_y);
    cJSON_AddStringToObject(reply, "biggest_request", biggest);
    cJSON_AddNumberToObject(reply, "prep_us", b.prep_us);
    cJSON_AddNumberToObject(reply, "render_us", b.render_us);
    cJSON_AddNumberToObject(reply, "flush_us", b.flush_us);
    cJSON_AddNumberToObject(reply, "flushes_per_frame", b.flushes_x10 / 10.0);
    cJSON_AddNumberToObject(reply, "scope_cb_us", b.scope_cb_us);
    cJSON_AddNumberToObject(reply, "sweep_ticks", b.sweep_ticks);
    cJSON_AddNumberToObject(reply, "rendered", b.rendered);
    cJSON_AddNumberToObject(reply, "internal_free", heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(reply, "internal_min", heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(reply, "internal_largest", heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    cJSON_AddStringToObject(reply, "openaip", aip.state);
    return send_json(req, reply);
}
static esp_err_t on_debug_select(httpd_req_t *req)
{
    char query[64], hex[8] = "", emerg[4] = "", view[8] = "";
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        httpd_query_key_value(query, "hex", hex, sizeof(hex));
        httpd_query_key_value(query, "emerg", emerg, sizeof(emerg));
        httpd_query_key_value(query, "view", view, sizeof(view));
    }
    board_lock();
    if (emerg[0]) radar_ui_debug_emergency(emerg[0] == '1' ? hex : NULL);
    if (view[0]) radar_ui_debug_view(strcmp(view, "big") == 0 ? 1 : strcmp(view, "list") == 0 ? 2 : 0);
    radar_ui_select(hex[0] ? hex : NULL);
    board_unlock();
    return send_result(req, ESP_OK, NULL);
}
#endif

// The timezone list, shared with the settings screen
static esp_err_t on_zones(httpd_req_t *req)
{
    cJSON *list = cJSON_CreateArray();
    for (int i = 0; i < ZONE_COUNT; i++) {
        cJSON *z = cJSON_CreateArray();
        cJSON_AddItemToArray(z, cJSON_CreateString(ZONES[i].iana));
        cJSON_AddItemToArray(z, cJSON_CreateString(ZONES[i].label));
        cJSON_AddItemToArray(z, cJSON_CreateString(ZONES[i].posix));
        cJSON_AddItemToArray(list, z);
    }
    return send_json(req, list);
}

// The brightness slider, shown for a few seconds without saving
static esp_err_t on_brightness(httpd_req_t *req)
{
    cJSON *json = read_json(req);
    int percent = -1;
    take_int(json, "percent", &percent);
    cJSON_Delete(json);
    if (percent < 1 || percent > 100) return send_result(req, ESP_ERR_INVALID_ARG, "Percent from 1 to 100");
    board_lock(); // the display task sets the level too, every second
    backlight_preview(percent);
    board_unlock();
    return send_result(req, ESP_OK, NULL);
}

static esp_err_t on_ota_status(httpd_req_t *req)
{
    ota_status_t st;
    ota_get_status(&st);
    cJSON *json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "state", st.state);
    cJSON_AddStringToObject(json, "detail", st.detail);
    cJSON_AddStringToObject(json, "latest", st.latest);
    cJSON_AddStringToObject(json, "published", st.published);
    cJSON_AddStringToObject(json, "notes_url", st.notes_url);
    cJSON_AddBoolToObject(json, "has_image", st.has_image);
    cJSON_AddNumberToObject(json, "progress", st.progress);
    cJSON_AddStringToObject(json, "running", esp_app_get_description()->version);
    return send_json(req, json);
}

static esp_err_t on_ota_check(httpd_req_t *req)
{
    esp_err_t err = ota_check();
    return send_result(req, err, err == ESP_ERR_INVALID_STATE ? "Already busy" : NULL);
}

static esp_err_t on_ota_install(httpd_req_t *req)
{
    esp_err_t err = ota_install();
    return send_result(req, err, err == ESP_ERR_INVALID_STATE ? "Check for an update first" : NULL);
}

static esp_err_t on_forget(httpd_req_t *req)
{
    settings_t s;
    settings_copy(&s);
    s.ssid[0] = s.password[0] = '\0';
    ESP_LOGW(TAG, "network forgotten");
    return send_result(req, commit(&s), NULL);
}

static esp_err_t on_defaults(httpd_req_t *req)
{
    return send_result(req, commit(NULL), NULL);
}

void web_start(void)
{
    esp_timer_create_args_t timer = {.callback = commit_and_restart, .name = "commit"};
    ESP_ERROR_CHECK(esp_timer_create(&timer, &s_commit_timer));

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
#if CONFIG_LV_USE_SNAPSHOT
    cfg.stack_size = 16384; // a snapshot renders the whole screen in this task
#else
    cfg.stack_size = 6144; // cJSON and the scan records; about 3.3 KB used
#endif
#if CONFIG_SPIRAM && CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM
    cfg.task_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT; // see commit_and_restart()
#endif
    cfg.max_uri_handlers = 20;
    cfg.max_open_sockets = 5;
    // A phone opens several connections at once and leaves them open
    cfg.lru_purge_enable = true;

    httpd_handle_t server;
    esp_err_t err = httpd_start(&server, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "server not started: %s", esp_err_to_name(err));
        return;
    }
    static const httpd_uri_t URIS[] = {
        {.uri = "/", .method = HTTP_GET, .handler = on_root},
        {.uri = "/setup", .method = HTTP_GET, .handler = on_setup_page},
        {.uri = "/api/state", .method = HTTP_GET, .handler = on_state},
        {.uri = "/api/scan", .method = HTTP_GET, .handler = on_scan},
        {.uri = "/api/wifi", .method = HTTP_POST, .handler = on_wifi},
        {.uri = "/api/settings", .method = HTTP_POST, .handler = on_settings},
        {.uri = "/api/forget", .method = HTTP_POST, .handler = on_forget},
        {.uri = "/api/brightness", .method = HTTP_POST, .handler = on_brightness},
        {.uri = "/api/zones", .method = HTTP_GET, .handler = on_zones},
#if CONFIG_LV_USE_SNAPSHOT
        {.uri = "/api/debug/screen", .method = HTTP_GET, .handler = on_debug_screen},
        {.uri = "/api/debug/settings", .method = HTTP_POST, .handler = on_debug_settings},
        {.uri = "/api/debug/select", .method = HTTP_POST, .handler = on_debug_select},
        {.uri = "/api/debug/bench", .method = HTTP_POST, .handler = on_debug_bench},
#endif
        {.uri = "/api/defaults", .method = HTTP_POST, .handler = on_defaults},
        {.uri = "/api/ota", .method = HTTP_GET, .handler = on_ota_status},
        {.uri = "/api/ota/check", .method = HTTP_POST, .handler = on_ota_check},
        {.uri = "/api/ota/install", .method = HTTP_POST, .handler = on_ota_install},
    };
    for (size_t i = 0; i < sizeof(URIS) / sizeof(URIS[0]); i++) {
        httpd_uri_t u = URIS[i];
        if (u.method == HTTP_POST) {
            u.user_ctx = (void *)u.handler;
            u.handler = json_only;
        }
        httpd_register_uri_handler(server, &u);
    }
    httpd_register_err_handler(server, HTTPD_404_NOT_FOUND, on_not_found);
}
