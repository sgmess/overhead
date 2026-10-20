#include "settings.h"

#include <math.h>
#include <string.h>

#include "aircraft.h"
#include "board.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"

static const char *TAG = "settings";

#define NS "overhead"

static settings_t s_settings;
static SemaphoreHandle_t s_store_lock; // see store(); made in settings_load(), before any writer
static bool s_final; // under s_store_lock: a save or reset before a restart was written

// Before anything is saved: no network (so the setup portal opens) and the
// radar centred on Heathrow until, once connected, the internet address says
// where it is (geoloc.c). Everything here can be changed on the
// configuration page.
void settings_defaults(settings_t *out)
{
    *out = (settings_t){
        .lat = 51.4700,
        .lon = -0.4543,
        .range_idx = 2, // 25 NM
        .feed = 1,      // adsb.fi, with adsb.lol as the fallback
        .fetch_s = 5,
        .show_ground = true,
        .sweep = true,
        .max_labels = 40,
        // The Tab5's NP-F550 latches into protection below 6.0 V and has to
        // be refitted before it charges again
        .batt_auto_off = true,
        .batt_shutdown_mv = 6300,
#if defined(OVERHEAD_BOARD_TAB5)
        .rotation = 90, // landscape: scope left, list right
#endif
        .show_airspace = true,
        .show_airfields = true,
        .brightness = 100,
        .dim_mode = 1, // sunset to sunrise
        .dim_brightness = 30,
        .dim_from = 22 * 60,
        .dim_to = 7 * 60,
        .clock = 0, // UTC until a timezone is chosen
        .tag_fields = TAG_CALLSIGN | TAG_ALT,
        .trail_s = 120,
        .show_military = true,
        .show_route = true,
        .alert = false, // near an airport it would never stop
        .alert_nm10 = 20,
        .alert_ft = 5000,
        .alert_sound = true,
        .show_vector = false,
        .filter_min_ft = 0,
        .filter_max_ft = FILTER_NO_CEILING,
        .show_kinds = AC_KIND_ALL,
        .emerg_alert = true,
        .show_centrelines = true,
        .sleep = false,
        .sleep_from = 23 * 60,
        .sleep_to = 7 * 60,
        .adb_daily = 100,
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
           s->alert_ft >= 500 && s->alert_ft <= 20000 &&
           s->filter_min_ft >= 0 && s->filter_max_ft <= FILTER_NO_CEILING &&
           s->filter_min_ft < s->filter_max_ft &&
           s->show_kinds >= 1 && s->show_kinds <= AC_KIND_ALL &&
           s->sleep_from >= 0 && s->sleep_from < 24 * 60 && s->sleep_to >= 0 && s->sleep_to < 24 * 60 &&
           strlen(s->adb_key) < sizeof(s->adb_key) && s->adb_daily >= ADB_DAILY_MIN && s->adb_daily <= ADB_DAILY_MAX;
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
    s_store_lock = xSemaphoreCreateMutex();
    settings_t s;
    settings_defaults(&s);

    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) == ESP_OK) {
        get_str(h, "ssid", s.ssid, sizeof(s.ssid));
        get_str(h, "pass", s.password, sizeof(s.password));
        get_deg(h, "lat", &s.lat);
        get_deg(h, "lon", &s.lon);
        // Saved before this was: a centre was already saved, so keep it
        int32_t lat;
        s.centre_set = nvs_get_i32(h, "lat", &lat) == ESP_OK;
        get_bool(h, "centre", &s.centre_set);
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
        get_bool(h, "vector", &s.show_vector);
        get_int(h, "flt_min", &s.filter_min_ft);
        get_int(h, "flt_max", &s.filter_max_ft);
        get_int(h, "kinds", &s.show_kinds);
        get_bool(h, "emerg", &s.emerg_alert);
        get_bool(h, "clines", &s.show_centrelines);
        get_bool(h, "sleep", &s.sleep);
        get_int(h, "sleep_from", &s.sleep_from);
        get_int(h, "sleep_to", &s.sleep_to);
        get_str(h, "adb_key", s.adb_key, sizeof(s.adb_key));
        get_int(h, "adb_daily", &s.adb_daily);
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

static esp_err_t store_unlocked(const settings_t *s)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    // The empty SSID is stored too: it is what says "forgotten", over a
    // network built in from menuconfig
    err = nvs_set_str(h, "ssid", s->ssid);
    if (err == ESP_OK) err = nvs_set_str(h, "pass", s->password);
    if (err == ESP_OK) err = nvs_set_i32(h, "lat", (int32_t)lround(s->lat * 1e6));
    if (err == ESP_OK) err = nvs_set_i32(h, "lon", (int32_t)lround(s->lon * 1e6));
    if (err == ESP_OK) err = nvs_set_u8(h, "centre", s->centre_set);
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
    if (err == ESP_OK) err = nvs_set_u8(h, "vector", s->show_vector);
    if (err == ESP_OK) err = nvs_set_i32(h, "flt_min", s->filter_min_ft);
    if (err == ESP_OK) err = nvs_set_i32(h, "flt_max", s->filter_max_ft);
    if (err == ESP_OK) err = nvs_set_i32(h, "kinds", s->show_kinds);
    if (err == ESP_OK) err = nvs_set_u8(h, "emerg", s->emerg_alert);
    if (err == ESP_OK) err = nvs_set_u8(h, "clines", s->show_centrelines);
    if (err == ESP_OK) err = nvs_set_u8(h, "sleep", s->sleep);
    if (err == ESP_OK) err = nvs_set_i32(h, "sleep_from", s->sleep_from);
    if (err == ESP_OK) err = nvs_set_i32(h, "sleep_to", s->sleep_to);
    if (err == ESP_OK) err = nvs_set_str(h, "adb_key", s->adb_key);
    if (err == ESP_OK) err = nvs_set_i32(h, "adb_daily", s->adb_daily);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

static esp_err_t erase_unlocked(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_erase_all(h);
        if (err == ESP_OK) err = nvs_commit(h);
        nvs_close(h);
    }
    return err;
}

// One writer at a time: the settings task stores changes, and a new
// network or a reset is written from the web server's restart timer, so
// their keys could interleave and leave a mix in flash. That last write
// before the restart is final: a change still queued would put older
// values back, so it is refused.
static esp_err_t store(const settings_t *s, bool final)
{
    xSemaphoreTake(s_store_lock, portMAX_DELAY);
    esp_err_t err = ESP_ERR_INVALID_STATE;
    if (!s_final) {
        err = s ? store_unlocked(s) : erase_unlocked();
        s_final = final;
    }
    xSemaphoreGive(s_store_lock);
    return err;
}

void settings_copy(settings_t *out)
{
    board_lock();
    *out = s_settings;
    board_unlock();
}

esp_err_t settings_save(const settings_t *s)
{
    if (!settings_valid(s)) return ESP_ERR_INVALID_ARG;
    esp_err_t err = store(s, true);
    if (err == ESP_OK) {
        board_lock(); // the display task reads them all the time
        s_settings = *s;
        board_unlock();
    }
    return err;
}

esp_err_t settings_erase(void)
{
    return store(NULL, true);
}

bool settings_need_restart(const settings_t *s)
{
    return strcmp(s->ssid, s_settings.ssid) != 0 || strcmp(s->password, s_settings.password) != 0 ||
           s->rotation != s_settings.rotation;
}

// ---------------------------------------------------------------- changes
//
// One task stores and applies every change, so the web page and the screen
// can't interleave, and its stack is internal: on a board without XIP a
// flash write takes PSRAM away, and with it any stack there.

typedef struct {
    settings_t s;
    uint32_t seq;      // numbers each change, so a waiter can tell its own from a newer one
    bool keep_running; // a new network already joined: no restart for it
} change_t;

static QueueHandle_t s_queue;          // of change_t, one deep: the newest wins
static SemaphoreHandle_t s_applied;    // given after each change, for waiters
static SemaphoreHandle_t s_wait_lock;  // one waiter at a time
static SemaphoreHandle_t s_send_lock;  // one sender at a time
static SemaphoreHandle_t s_net_lock;   // settings_change_network()'s copy
static settings_listener_t s_listener;
static uint32_t s_seq;                 // the last number handed out
// The waiter's change, and what became of it
typedef enum { W_NONE, W_PENDING, W_DONE, W_REPLACED } wait_state_t;
static uint32_t s_wait_seq;
static volatile wait_state_t s_wait_state;
static volatile esp_err_t s_wait_err;

static void done(uint32_t seq, esp_err_t err)
{
    xSemaphoreTake(s_send_lock, portMAX_DELAY);
    if (s_wait_state == W_PENDING) {
        if (seq == s_wait_seq) {
            s_wait_err = err;
            s_wait_state = W_DONE;
        } else if ((int32_t)(seq - s_wait_seq) > 0) {
            // The queue is one deep: a newer change finished first, so it
            // replaced the waiter's before the task took it
            s_wait_state = W_REPLACED;
        }
    }
    xSemaphoreGive(s_send_lock);
    xSemaphoreGive(s_applied);
}

static void settings_task(void *arg)
{
    QueueHandle_t queue = arg; // s_queue may not be set yet
    static change_t c;  // too big for a 4 KB stack, with old
    static settings_t old;
    for (;;) {
        xQueueReceive(queue, &c, portMAX_DELAY);
        if (!c.keep_running) {
            // Only settings_change_network() changes the network this way;
            // anything else carries the one running when it was copied, which
            // a network Improv has just joined may have replaced since
            strlcpy(c.s.ssid, s_settings.ssid, sizeof(c.s.ssid));
            strlcpy(c.s.password, s_settings.password, sizeof(c.s.password));
        }
        const settings_t *next = &c.s;
        if (memcmp(next, &s_settings, sizeof(*next)) == 0) {
            done(c.seq, ESP_OK);
            continue;
        }
        const bool restart = !c.keep_running && settings_need_restart(next);
        esp_err_t err = store(next, false);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "not stored: %s", esp_err_to_name(err));
            done(c.seq, err);
            continue;
        }
        if (restart) {
            ESP_LOGW(TAG, "network or orientation changed, restarting");
            vTaskDelay(pdMS_TO_TICKS(500)); // lets a web reply get out
            esp_restart();
        }
        // The display task reads them all the time; this keeps it from
        // seeing half of a string
        old = s_settings;
        board_lock();
        s_settings = *next;
        board_unlock();
        ESP_LOGI(TAG, "changed and applied");
        if (s_listener) s_listener(&old, &s_settings);
        done(c.seq, ESP_OK);
    }
}

void settings_start(settings_listener_t listener)
{
    s_listener = listener;
    s_applied = xSemaphoreCreateBinary();
    s_wait_lock = xSemaphoreCreateMutex();
    s_send_lock = xSemaphoreCreateMutex();
    s_net_lock = xSemaphoreCreateMutex();
    QueueHandle_t queue = xQueueCreate(1, sizeof(change_t));
    if (!s_applied || !s_wait_lock || !s_send_lock || !s_net_lock || !queue ||
        xTaskCreate(settings_task, "settings", 4096, queue, 4, NULL) != pdPASS) {
        // Changes are refused, ESP_ERR_INVALID_STATE; this is at boot, so
        // nothing to free for a retry
        ESP_LOGE(TAG, "changes not started: no memory");
        return;
    }
    s_queue = queue; // last: callers take its being set as everything ready
}

// Queue s as the next change, as the waiter's with wait
static void send(const settings_t *s, bool wait, bool keep_running)
{
    static change_t c; // too big for some callers' stacks, so under s_send_lock
    xSemaphoreTake(s_send_lock, portMAX_DELAY);
    c.s = *s;
    c.seq = ++s_seq;
    c.keep_running = keep_running;
    if (wait) {
        s_wait_seq = c.seq;
        s_wait_state = W_PENDING;
    }
    xQueueOverwrite(s_queue, &c);
    xSemaphoreGive(s_send_lock);
}

static esp_err_t change(const settings_t *s, bool wait, bool keep_running)
{
    if (!settings_valid(s)) return ESP_ERR_INVALID_ARG;
    if (!s_queue) return ESP_ERR_INVALID_STATE;
    if (!wait) {
        send(s, false, keep_running);
        return ESP_OK;
    }
    xSemaphoreTake(s_wait_lock, portMAX_DELAY);
    xSemaphoreTake(s_applied, 0); // drop a give left from a change nobody waited for
    const TickType_t until = xTaskGetTickCount() + pdMS_TO_TICKS(3000);
    esp_err_t err = ESP_ERR_TIMEOUT;
    // A change from the screen (which doesn't wait) can replace this one
    // before the task takes it; then send it again, after that one
    for (int tries = 0; tries < 3; tries++) {
        send(s, true, keep_running);
        while (s_wait_state == W_PENDING) {
            const int32_t left = (int32_t)(until - xTaskGetTickCount());
            if (left <= 0 || xSemaphoreTake(s_applied, left) != pdTRUE) break;
        }
        if (s_wait_state != W_REPLACED) break;
    }
    if (s_wait_state == W_DONE) err = s_wait_err;
    xSemaphoreTake(s_send_lock, portMAX_DELAY);
    s_wait_state = W_NONE;
    xSemaphoreGive(s_send_lock);
    xSemaphoreGive(s_wait_lock);
    return err;
}

esp_err_t settings_change(const settings_t *s, bool wait)
{
    return change(s, wait, false);
}

esp_err_t settings_change_network(const char *ssid, const char *password)
{
    static settings_t s; // too big for the Improv task's stack, so under s_net_lock
    // Improv starts early, so this can come before settings_start()
    for (int i = 0; !s_queue && i < 100; i++) vTaskDelay(pdMS_TO_TICKS(100));
    if (!s_queue) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_net_lock, portMAX_DELAY);
    settings_copy(&s);
    strlcpy(s.ssid, ssid, sizeof(s.ssid));
    strlcpy(s.password, password, sizeof(s.password));
    const esp_err_t err = change(&s, true, true);
    xSemaphoreGive(s_net_lock);
    return err;
}
