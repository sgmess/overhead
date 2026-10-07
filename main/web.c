#include "web.h"

#include <stdlib.h>
#include <string.h>

#include "battery.h"
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

static const char *TAG = "web";

#define BODY_MAX 1024
#define SCAN_MAX 24
#define RESTART_DELAY_MS 1500 // lets the reply reach the browser first

extern const char setup_html_start[] asm("_binary_setup_html_start");
extern const char setup_html_end[] asm("_binary_setup_html_end");
extern const char config_html_start[] asm("_binary_config_html_start");
extern const char config_html_end[] asm("_binary_config_html_end");

#if defined(OVERHEAD_BOARD_TAB5)
#define BOARD_NAME "M5Stack Tab5"
#define CAN_ROTATE true
#elif defined(OVERHEAD_BOARD_P4_34C)
#define BOARD_NAME "Waveshare ESP32-P4-WIFI6-Touch-LCD-3.4C"
#define CAN_ROTATE false
#else
#define BOARD_NAME "Waveshare ESP32-S3-Touch-LCD-2.8C"
#define CAN_ROTATE false
#endif

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

// The request body as JSON, or NULL
static cJSON *read_json(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len > BODY_MAX) return NULL;
    char *body = malloc(req->content_len + 1);
    if (!body) return NULL;
    int got = 0;
    while (got < req->content_len) {
        int n = httpd_req_recv(req, body + got, req->content_len - got);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) continue;
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

static void take_bool(const cJSON *json, const char *key, bool *v)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(json, key);
    if (cJSON_IsBool(item)) *v = cJSON_IsTrue(item);
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
    const settings_t *s = settings();
    const esp_app_desc_t *app = esp_app_get_description();
    cJSON *json = cJSON_CreateObject();

    cJSON_AddStringToObject(json, "board", BOARD_NAME);
    cJSON_AddStringToObject(json, "firmware", app->version);
    cJSON_AddStringToObject(json, "project", app->project_name);
    cJSON_AddStringToObject(json, "update_repo", CONFIG_OVERHEAD_OTA_REPO);
    cJSON_AddBoolToObject(json, "can_rotate", CAN_ROTATE);
    cJSON_AddNumberToObject(json, "uptime_s", (double)(esp_timer_get_time() / 1000000));
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

    openaip_status_t oa;
    openaip_get_status(&oa);
    cJSON *o = cJSON_AddObjectToObject(json, "openaip");
    cJSON_AddStringToObject(o, "state", oa.state);
    cJSON_AddStringToObject(o, "detail", oa.detail);
    cJSON_AddStringToObject(o, "countries", oa.countries);
    cJSON_AddNumberToObject(o, "airspaces", oa.airspaces);
    cJSON_AddNumberToObject(o, "airfields", oa.airfields);
    cJSON_AddNumberToObject(o, "age_s", oa.age_s);
    return send_json(req, json);
}

static int cmp_rssi(const void *a, const void *b)
{
    return ((const wifi_ap_record_t *)b)->rssi - ((const wifi_ap_record_t *)a)->rssi;
}

static esp_err_t on_scan(httpd_req_t *req)
{
    // Blocks for a couple of seconds; fails while the station is mid-connect
    esp_err_t err = esp_wifi_scan_start(NULL, true);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "scan: %s", esp_err_to_name(err));
        return send_result(req, err, "Scan failed, try again");
    }
    uint16_t n = SCAN_MAX;
    wifi_ap_record_t *aps = calloc(SCAN_MAX, sizeof(*aps));
    if (!aps) return httpd_resp_send_500(req);
    esp_wifi_scan_get_ap_records(&n, aps);
    qsort(aps, n, sizeof(*aps), cmp_rssi);

    // Strongest first, each name once (mesh and multi-band APs repeat it)
    cJSON *list = cJSON_CreateArray();
    for (int i = 0; i < n; i++) {
        const char *ssid = (const char *)aps[i].ssid;
        if (!ssid[0]) continue;
        bool seen = false;
        for (int j = 0; j < i && !seen; j++) seen = strcmp(ssid, (const char *)aps[j].ssid) == 0;
        if (seen) continue;
        cJSON *ap = cJSON_CreateObject();
        cJSON_AddStringToObject(ap, "ssid", ssid);
        cJSON_AddNumberToObject(ap, "rssi", aps[i].rssi);
        cJSON_AddBoolToObject(ap, "open", aps[i].authmode == WIFI_AUTH_OPEN);
        cJSON_AddItemToArray(list, ap);
    }
    free(aps);
    cJSON *json = cJSON_CreateObject();
    cJSON_AddBoolToObject(json, "ok", true);
    cJSON_AddItemToObject(json, "networks", list);
    return send_json(req, json);
}

// The setup page: network, password and, optionally, the radar centre
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
    settings_t s = *settings();
    strlcpy(s.ssid, ssid->valuestring, sizeof(s.ssid));
    strlcpy(s.password, pass ? pass->valuestring : "", sizeof(s.password));
    take_double(json, "lat", &s.lat);
    take_double(json, "lon", &s.lon);
    cJSON_Delete(json);

    if (!settings_valid(&s)) return send_result(req, ESP_ERR_INVALID_ARG, "Latitude or longitude out of range");
    ESP_LOGI(TAG, "network set to '%s'", s.ssid);
    return send_result(req, commit(&s), NULL);
}

// The configuration page; any setting left out keeps its value
static esp_err_t on_settings(httpd_req_t *req)
{
    cJSON *json = read_json(req);
    if (!json) return send_result(req, ESP_ERR_INVALID_ARG, "Expected JSON");
    settings_t s = *settings();
    take_double(json, "lat", &s.lat);
    take_double(json, "lon", &s.lon);
    take_int(json, "range_idx", &s.range_idx);
    take_int(json, "feed", &s.feed);
    take_int(json, "fetch_s", &s.fetch_s);
    take_bool(json, "show_ground", &s.show_ground);
    take_bool(json, "sweep", &s.sweep);
    take_int(json, "max_labels", &s.max_labels);
    take_bool(json, "batt_auto_off", &s.batt_auto_off);
    take_int(json, "batt_shutdown_mv", &s.batt_shutdown_mv);
    if (CAN_ROTATE) take_int(json, "rotation", &s.rotation);
    take_bool(json, "show_airspace", &s.show_airspace);
    take_bool(json, "show_airfields", &s.show_airfields);
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
    // Only sent when typed or cleared, since the page never sees the old one
    const cJSON *key = cJSON_GetObjectItemCaseSensitive(json, "openaip_key");
    if (key) {
        bool ok = cJSON_IsString(key) && strlen(key->valuestring) < sizeof(s.openaip_key);
        for (const char *c = ok ? key->valuestring : ""; *c; c++) ok = ok && *c > ' ' && *c < 0x7f;
        if (!ok) {
            cJSON_Delete(json);
            return send_result(req, ESP_ERR_INVALID_ARG, "The API key is up to 64 characters, no spaces");
        }
        strlcpy(s.openaip_key, key->valuestring, sizeof(s.openaip_key));
    }
    cJSON_Delete(json);

    if (!settings_valid(&s)) {
        return send_result(req, ESP_ERR_INVALID_ARG, "A value is out of range (countries are two-letter codes, up to four)");
    }
    return send_result(req, commit(&s), NULL);
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
    settings_t s = *settings();
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
    cfg.stack_size = 6144; // cJSON and the scan records; about 3.3 KB used
#if CONFIG_SPIRAM && CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM
    cfg.task_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT; // see commit_and_restart()
#endif
    cfg.max_uri_handlers = 14;
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
        {.uri = "/api/defaults", .method = HTTP_POST, .handler = on_defaults},
        {.uri = "/api/ota", .method = HTTP_GET, .handler = on_ota_status},
        {.uri = "/api/ota/check", .method = HTTP_POST, .handler = on_ota_check},
        {.uri = "/api/ota/install", .method = HTTP_POST, .handler = on_ota_install},
    };
    for (size_t i = 0; i < sizeof(URIS) / sizeof(URIS[0]); i++) httpd_register_uri_handler(server, &URIS[i]);
    httpd_register_err_handler(server, HTTPD_404_NOT_FOUND, on_not_found);
}
