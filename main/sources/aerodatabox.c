#include "aerodatabox.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "airlines.h"
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
#include "nvs.h"
#include "settings.h"

static const char *TAG = "adb";

#define API "https://api.aerodatabox.com"
#define FLIGHTS_N 16
#define AIRCRAFT_N 32
#define RESP_MAX (48 * 1024) // a callsign can have a few flights on the day, ~3 KB each
#define FLIGHT_COST 2        // AeroDataBox's tier 2
#define AIRCRAFT_COST 1      // tier 1
#define RETRY_US (60 * 1000000LL)        // after a failed request
#define REFRESH_US (15 * 60 * 1000000LL) // a flight's times, if it is still being looked at
#define UNKNOWN_US (60 * 60 * 1000000LL) // before asking again about a flight nobody knew
#define LIMIT_US (10 * 60 * 1000000LL)   // after "too many requests"

typedef struct {
    char callsign[10];
    adb_flight_t f;
    bool found;         // f is something
    int64_t fetched_us; // answered, found or not; 0 never
    int64_t asked_us;   // queued or failed: holds off asking again; 0 not
    uint32_t used;      // for evicting the least recently used
} flight_entry_t;

typedef struct {
    char hex[8];
    adb_aircraft_t a;
    bool found, done;
    int64_t asked_us;
    uint32_t used;
} aircraft_entry_t;

typedef struct {
    char data[RESP_MAX];
    size_t len;
    bool overflow;
} resp_t;

// In PSRAM, as route.c's
static flight_entry_t *s_flights;
static aircraft_entry_t *s_aircraft;
static resp_t *s_resp;
static uint32_t s_clock;
static SemaphoreHandle_t s_lock;
static TaskHandle_t s_task;

// The one flight and one aircraft waiting, newest wins: only the picked one matters
static char s_want_cs[10], s_want_cs_hex[8], s_want_hex[8]; // the flight, its aircraft, the aircraft
static double s_want_lat, s_want_lon;

// Under s_lock
static int s_used_today, s_day = -1; // s_day: the year * 1000 + its day
// The day's spending as last stored, so restarts and power cuts can't each
// start the day's credits again. In its own namespace: a reset to the
// defaults doesn't spend them again either.
#define SPENT_NS "adb"
static int32_t s_spent_day = -1, s_spent;
static int s_lookups;
static const char *s_state = "ok";
static char s_detail[64];
static int64_t s_limit_us;    // when the service said to slow down; 0 not
static char s_bad_key[65];    // the key it refused; asking again only with another

// ---------------------------------------------------------------- helpers

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

static const cJSON *obj(const cJSON *o, const char *key)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsObject(v) ? v : NULL;
}

static int64_t days_from_civil(int y, int m, int d)
{
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const int yoe = y - era * 400;
    const int doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    const int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (int64_t)era * 146097 + doe - 719468;
}

// A DateTimeContract's UTC, "2026-10-20 09:42Z", in seconds since 1970; 0 if none
static int64_t utc_s(const cJSON *dt)
{
    int y, mo, d, h, mi;
    if (!dt || sscanf(str(dt, "utc"), "%d-%d-%d %d:%d", &y, &mo, &d, &h, &mi) != 5) return 0;
    if (mo < 1 || mo > 12 || d < 1 || d > 31 || h < 0 || h > 23 || mi < 0 || mi > 59) return 0;
    return days_from_civil(y, mo, d) * 86400 + h * 3600 + mi * 60;
}

// Its local time, "2026-10-20 10:42+01:00", as "10:42"
static void hhmm(const cJSON *dt, char out[6])
{
    const char *l = dt ? str(dt, "local") : "";
    out[0] = '\0';
    if (strlen(l) >= 16 && l[13] == ':') {
        memcpy(out, l + 11, 5);
        out[5] = '\0';
    }
}

// Great-circle distance in nautical miles
static double nm_between(double lat1, double lon1, double lat2, double lon2)
{
    const double r = M_PI / 180.0;
    const double a = pow(sin((lat2 - lat1) * r / 2), 2) +
                     cos(lat1 * r) * cos(lat2 * r) * pow(sin(lon_diff(lon2, lon1) * r / 2), 2);
    return 2 * 3440.065 * asin(sqrt(a));
}

// Whether (lat, lon) is anywhere near the way from a to b
static bool on_the_way(const route_t *r, double lat, double lon)
{
    if (r->stops < 2) return true; // nothing to say otherwise
    const double leg = nm_between(r->lat[0], r->lon[0], r->lat[1], r->lon[1]);
    const double detour = nm_between(r->lat[0], r->lon[0], lat, lon) + nm_between(lat, lon, r->lat[1], r->lon[1]) - leg;
    return detour < 100 + leg / 4;
}

// ---------------------------------------------------------------- parsing

// One end: its airport into the route's slot k, and its times
static void parse_end(const cJSON *mv, bool arrival, route_t *r, int k, adb_end_t *e)
{
    const cJSON *ap = obj(mv, "airport");
    const char *code = str(ap, "iata")[0] ? str(ap, "iata") : str(ap, "icao");
    if (k) strlcat(r->codes, "-", sizeof(r->codes));
    strlcat(r->codes, code[0] ? code : "?", sizeof(r->codes));
    const char *place = str(ap, "municipalityName");
    if (!place[0]) place = str(ap, "shortName");
    if (!place[0]) place = str(ap, "name");
    if (k) strlcat(r->places, "|", sizeof(r->places));
    strlcat(r->places, place, sizeof(r->places));
    const cJSON *loc = obj(ap, "location");
    const cJSON *lat = cJSON_GetObjectItemCaseSensitive(loc, "lat");
    const cJSON *lon = cJSON_GetObjectItemCaseSensitive(loc, "lon");
    if (r->stops == k && cJSON_IsNumber(lat) && cJSON_IsNumber(lon) && isfinite(lat->valuedouble) &&
        isfinite(lon->valuedouble)) {
        r->lat[k] = (float)lat->valuedouble;
        r->lon[k] = (float)lon->valuedouble;
        r->stops++;
    }

    // The gate times airlines publish first, then the runway's, then a guess
    const cJSON *sched = obj(mv, "scheduledTime");
    const cJSON *best = obj(mv, "revisedTime");
    if (!best) best = obj(mv, arrival ? "predictedTime" : "runwayTime");
    if (!best) best = obj(mv, arrival ? "runwayTime" : "predictedTime");
    hhmm(sched, e->sched);
    hhmm(best ? best : sched, e->best);
    e->live = best && e->best[0];
    const int64_t s = utc_s(sched), b = utc_s(best);
    e->delay_min = s && b ? (int)((b - s) / 60) : 0;
    strlcpy(e->terminal, str(mv, "terminal"), sizeof(e->terminal));
    strlcpy(e->gate, str(mv, "gate"), sizeof(e->gate));
}

static void parse_flight(const cJSON *fl, double lat, double lon, adb_flight_t *f)
{
    *f = (adb_flight_t){0};
    strlcpy(f->number, str(fl, "number"), sizeof(f->number));
    static const struct {
        const char *api, *text;
        bool departed;
    } STATUS[] = {
        {"Expected", "Scheduled", false}, {"EnRoute", "En route", true},  {"CheckIn", "Check-in", false},
        {"Boarding", "Boarding", false},  {"GateClosed", "Gate closed", false}, {"Departed", "Departed", true},
        {"Delayed", "Delayed", false},    {"Approaching", "On approach", true}, {"Arrived", "Arrived", true},
        {"Canceled", "Cancelled", false}, {"Diverted", "Diverted", true},
    };
    const char *st = str(fl, "status");
    for (size_t i = 0; i < sizeof(STATUS) / sizeof(STATUS[0]); i++) {
        if (strcmp(st, STATUS[i].api) == 0) {
            strlcpy(f->status, STATUS[i].text, sizeof(f->status));
            f->departed = STATUS[i].departed;
        }
    }
    f->arrived = strcmp(st, "Arrived") == 0;
    f->diverted = strcmp(st, "Diverted") == 0;

    const cJSON *dep = obj(fl, "departure"), *arr = obj(fl, "arrival");
    parse_end(dep, false, &f->route, 0, &f->dep);
    parse_end(arr, true, &f->route, 1, &f->arr);
    if (obj(dep, "runwayTime")) f->departed = true;
    if (f->route.stops < 2) f->route.stops = 0;
    // As adsb.im's: " ?" when it doesn't fit where the aircraft is
    f->route.plausible = on_the_way(&f->route, lat, lon);
    if (!f->route.plausible) strlcat(f->route.codes, " ?", sizeof(f->route.codes));
    if (strcmp(f->route.codes, "?-?") == 0) f->route = (route_t){0};
}

// Whether a, as AeroDataBox writes it ("BAW 21V"), is b, as the feed does ("BAW21V")
static bool same_callsign(const char *a, const char *b)
{
    for (;; a++, b++) {
        while (*a == ' ') a++;
        if (*a != *b) return false;
        if (!*a) return true;
    }
}

// The best of a flight's legs on the day for an aircraft at (lat, lon)
// now: the one in the air that fits where it is, else the nearest in time.
// *off_s is how far now is outside its departure to arrival, give or take
// half an hour: 0 when inside or unknown.
static const cJSON *pick_flight(const cJSON *list, const char *callsign, double lat, double lon, int64_t *off_s)
{
    const time_t now = time(NULL);
    const bool clock_set = now > 1700000000;
    const cJSON *best = NULL, *fl;
    int64_t best_score = INT64_MAX;
    *off_s = 0;
    cJSON_ArrayForEach(fl, list)
    {
        if (!cJSON_IsObject(fl)) continue;
        const cJSON *dep = obj(fl, "departure"), *arr = obj(fl, "arrival");
        const cJSON *d = obj(dep, "revisedTime") ? obj(dep, "revisedTime") : obj(dep, "scheduledTime");
        const cJSON *a = obj(arr, "revisedTime") ? obj(arr, "revisedTime") : obj(arr, "scheduledTime");
        const int64_t dt = utc_s(d), at = utc_s(a);
        int64_t off = 0;
        if (clock_set && dt && now < dt - 1800) off = dt - 1800 - now;
        if (clock_set && at && now > at + 1800) off = now - at - 1800;
        int64_t score = off;
        const char *st = str(fl, "status");
        if (strcmp(st, "EnRoute") == 0 || strcmp(st, "Departed") == 0 || strcmp(st, "Approaching") == 0) {
            score -= 86400;
        }
        adb_flight_t f;
        parse_flight(fl, lat, lon, &f);
        if (!f.route.plausible) score += 43200;
        if (!same_callsign(str(fl, "callSign"), callsign)) score += 3600; // an aircraft flies several a day
        if (score < best_score) {
            best_score = score;
            best = fl;
            *off_s = off;
        }
    }
    return best;
}

static void parse_aircraft(const cJSON *root, adb_aircraft_t *a)
{
    *a = (adb_aircraft_t){0};
    const char *model = str(root, "typeName");
    if (!model[0]) model = str(root, "model");
    strlcpy(a->model, model, sizeof(a->model));
    static const char *const DATES[] = {"firstFlightDate", "deliveryDate", "rolloutDate"};
    for (int i = 0; i < 3 && !a->built; i++) {
        int y;
        if (sscanf(str(root, DATES[i]), "%d", &y) == 1 && y >= 1900 && y <= 2100) a->built = y;
    }
    const cJSON *seats = cJSON_GetObjectItemCaseSensitive(root, "numSeats");
    if (cJSON_IsNumber(seats) && seats->valuedouble > 0 && seats->valuedouble < 1000) a->seats = seats->valueint;
}

// ---------------------------------------------------------------- requests

typedef enum { GOT_SOME, GOT_NONE, NO_ANSWER } result_t;

// A new day starts the count again; with the lock held
static void roll_day(void)
{
    const time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    const int day = tm.tm_year * 1000 + tm.tm_yday;
    if (day == s_day) return;
    s_day = day;
    s_used_today = 0;
    if (s_spent_day == day && s_spent > 0) s_used_today = s_spent;
    if (strcmp(s_state, "limit") == 0 && !s_limit_us) {
        s_state = "ok";
        s_detail[0] = '\0';
    }
}

// Keep the day's spending for the next boot. A flash write, which needs XIP
// from PSRAM: this task's stack is there (as for the OpenAIP cache)
static void store_spent(int32_t day, int32_t used)
{
#if CONFIG_SPIRAM_XIP_FROM_PSRAM
    nvs_handle_t h;
    esp_err_t err = nvs_open(SPENT_NS, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_i32(h, "day", day);
        if (err == ESP_OK) err = nvs_set_i32(h, "spent", used);
        if (err == ESP_OK) err = nvs_commit(h);
        nvs_close(h);
    }
    if (err != ESP_OK) ESP_LOGW(TAG, "spending not stored: %s", esp_err_to_name(err));
#else
    (void)day;
    (void)used;
#endif
}

// The credits for one more request, if today has them; with the lock held
static bool can_spend(int cost, int daily)
{
    roll_day();
    if (s_used_today + cost <= daily) return true;
    s_state = "limit";
    snprintf(s_detail, sizeof(s_detail), "Today's %d credits are spent", daily);
    return false;
}

// GET path into s_resp, parsed into *root when there is something; what
// came of it. cost is charged for any answer from the service.
static result_t get(const settings_t *s, const char *path, int cost, cJSON **root)
{
    *root = NULL;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    // Not before the clock is set: the day's flights and credits need the date
    bool go = strcmp(s->adb_key, s_bad_key) != 0 && time(NULL) > 1700000000;
    if (go && s_limit_us && esp_timer_get_time() - s_limit_us < LIMIT_US) go = false;
    if (go) go = can_spend(cost, s->adb_daily);
    xSemaphoreGive(s_lock);
    if (!go) return NO_ANSWER;

    char url[192];
    snprintf(url, sizeof(url), API "%s", path);
    s_resp->len = 0;
    s_resp->overflow = false;
    s_resp->data[0] = '\0';
    esp_http_client_config_t cfg = {
        .url = url,
        .event_handler = on_http_event,
        .user_data = s_resp,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 15000,
        .user_agent = "overhead-radar/1.0 (ESP32-P4)",
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return NO_ANSWER;
    esp_http_client_set_header(c, "X-Api-Key", s->adb_key);
    esp_http_client_set_header(c, "Accept", "application/json");
    const esp_err_t err = esp_http_client_perform(c);
    const int status = esp_http_client_get_status_code(c);
    esp_http_client_cleanup(c);

    xSemaphoreTake(s_lock, portMAX_DELAY);
    result_t res = NO_ANSWER;
    bool spent = false;
    if (err != ESP_OK) {
        s_state = "error";
        snprintf(s_detail, sizeof(s_detail), "No answer (%s)", esp_err_to_name(err));
    } else if (status == 401 || status == 403) {
        s_state = "error";
        strlcpy(s_detail, "The key was refused", sizeof(s_detail));
        strlcpy(s_bad_key, s->adb_key, sizeof(s_bad_key));
    } else if (status == 429) {
        s_state = "limit";
        strlcpy(s_detail, "AeroDataBox asked to slow down, or the credits ran out", sizeof(s_detail));
        s_limit_us = esp_timer_get_time();
    } else if (status == 200 || status == 204 || status == 404) {
        s_used_today += cost;
        s_spent_day = s_day;
        s_spent = s_used_today;
        spent = true;
        s_lookups++;
        s_state = "ok";
        s_detail[0] = '\0';
        s_limit_us = 0;
        res = GOT_NONE;
        if (status == 200 && !s_resp->overflow) {
            *root = cJSON_ParseWithLength(s_resp->data, s_resp->len);
            if (*root) res = GOT_SOME;
        }
    } else {
        s_state = "error";
        snprintf(s_detail, sizeof(s_detail), "HTTP %d", status);
    }
    const int32_t day = s_spent_day, used = s_spent;
    xSemaphoreGive(s_lock);
    if (spent) store_spent(day, used);
    ESP_LOGI(TAG, "%s: HTTP %d, %u bytes%s", path, status, (unsigned)s_resp->len, s_resp->overflow ? ", too big" : "");
    if (status >= 400) ESP_LOGW(TAG, "%.200s", s_resp->data); // the service's reason
    return res;
}

static flight_entry_t *find_flight(const char *callsign)
{
    for (int i = 0; i < FLIGHTS_N; i++) {
        if (s_flights[i].callsign[0] && strcmp(s_flights[i].callsign, callsign) == 0) return &s_flights[i];
    }
    return NULL;
}

static flight_entry_t *add_flight(const char *callsign)
{
    flight_entry_t *e = find_flight(callsign);
    if (e) return e;
    e = &s_flights[0];
    for (int i = 1; i < FLIGHTS_N; i++) {
        if (s_flights[i].used < e->used) e = &s_flights[i];
    }
    *e = (flight_entry_t){0};
    strlcpy(e->callsign, callsign, sizeof(e->callsign));
    return e;
}

static aircraft_entry_t *find_aircraft(const char *hex)
{
    for (int i = 0; i < AIRCRAFT_N; i++) {
        if (s_aircraft[i].hex[0] && strcmp(s_aircraft[i].hex, hex) == 0) return &s_aircraft[i];
    }
    return NULL;
}

static aircraft_entry_t *add_aircraft(const char *hex)
{
    aircraft_entry_t *e = find_aircraft(hex);
    if (e) return e;
    e = &s_aircraft[0];
    for (int i = 1; i < AIRCRAFT_N; i++) {
        if (s_aircraft[i].used < e->used) e = &s_aircraft[i];
    }
    *e = (aircraft_entry_t){0};
    strlcpy(e->hex, hex, sizeof(e->hex));
    return e;
}

// Today's flights by by ("icao24", "number" or "callsign") = param, departing or
// arriving: without a date it answers for the nearest day with one, which
// can be yesterday's. The one for an aircraft flying callsign at (lat, lon)
// now into *f; *found false when none is. Only once the clock is set.
static result_t ask_flights(const settings_t *s, const char *by, const char *param, const char *callsign, double lat,
                            double lon, adb_flight_t *f, bool *found)
{
    char path[128];
    const time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    snprintf(path, sizeof(path),
             "/flights/%s/%s/%04d-%02d-%02d?dateLocalRole=Both&withAircraftImage=false&withLocation=false", by, param,
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
    cJSON *root;
    const result_t res = get(s, path, FLIGHT_COST, &root);
    *f = (adb_flight_t){0};
    *found = false;
    if (res == GOT_SOME) {
        int64_t off_s;
        const cJSON *fl = cJSON_IsArray(root) ? pick_flight(root, callsign, lat, lon, &off_s) : NULL;
        // Hours from now: some other day's, or another flight
        if (fl && off_s > 3 * 3600) {
            ESP_LOGI(TAG, "%s %s: nothing near now (%lld min off)", by, param, (long long)(off_s / 60));
            fl = NULL;
        }
        if (fl) {
            parse_flight(fl, lat, lon, f);
            *found = true;
            ESP_LOGI(TAG, "%s %s: %s %s, %s, dep %s (%+d) arr %s (%+d)", by, param, f->number, f->route.codes,
                     f->status, f->dep.best, f->dep.delay_min, f->arr.best, f->arr.delay_min);
        }
    }
    cJSON_Delete(root);
    return res;
}

// By the aircraft first, which finds the flight once AeroDataBox has seen
// it flying; then by the flight number the callsign is (AAL91 is AA91),
// which the schedules are filed under, or else by the callsign, which is
// only linked to a flight once seen (BAW21V)
static void lookup_flight(const settings_t *s, const char *callsign, const char *hex, double lat, double lon)
{
    adb_flight_t f;
    bool found = false;
    result_t res = NO_ANSWER;
    if (hex[0]) res = ask_flights(s, "icao24", hex, callsign, lat, lon, &f, &found);
    if (!found && (!hex[0] || res != NO_ANSWER)) {
        char number[12];
        if (airline_flight_number(callsign, number, sizeof(number))) {
            res = ask_flights(s, "number", number, callsign, lat, lon, &f, &found);
        } else {
            res = ask_flights(s, "callsign", callsign, callsign, lat, lon, &f, &found);
        }
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    flight_entry_t *e = add_flight(callsign);
    e->used = ++s_clock;
    if (res == NO_ANSWER) {
        e->asked_us = esp_timer_get_time();
    } else {
        e->f = f;
        e->found = found;
        e->fetched_us = esp_timer_get_time();
        e->asked_us = 0;
    }
    xSemaphoreGive(s_lock);
}

static void lookup_aircraft(const settings_t *s, const char *hex)
{
    char path[48];
    snprintf(path, sizeof(path), "/aircrafts/icao24/%s", hex);
    cJSON *root;
    const result_t res = get(s, path, AIRCRAFT_COST, &root);
    adb_aircraft_t a = {0};
    if (res == GOT_SOME && cJSON_IsObject(root)) {
        parse_aircraft(root, &a);
        ESP_LOGI(TAG, "%s: %s, built %d, %d seats", hex, a.model, a.built, a.seats);
    }
    cJSON_Delete(root);

    xSemaphoreTake(s_lock, portMAX_DELAY);
    aircraft_entry_t *e = add_aircraft(hex);
    e->used = ++s_clock;
    if (res == NO_ANSWER) {
        e->asked_us = esp_timer_get_time();
    } else {
        e->a = a;
        e->found = a.model[0] || a.built || a.seats;
        e->done = true;
    }
    xSemaphoreGive(s_lock);
}

static void adb_task(void *arg)
{
    static settings_t s; // one task, and too big for its stack
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        char callsign[10], cs_hex[8], hex[8];
        double lat, lon;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        strlcpy(callsign, s_want_cs, sizeof(callsign));
        strlcpy(cs_hex, s_want_cs_hex, sizeof(cs_hex));
        strlcpy(hex, s_want_hex, sizeof(hex));
        lat = s_want_lat;
        lon = s_want_lon;
        s_want_cs[0] = s_want_hex[0] = '\0';
        xSemaphoreGive(s_lock);
        settings_copy(&s);
        if (!s.adb_key[0]) continue;
        if (callsign[0]) lookup_flight(&s, callsign, cs_hex, lat, lon);
        if (hex[0]) lookup_aircraft(&s, hex);
    }
}

// ---------------------------------------------------------------- the rest

void adb_start(void)
{
    // From app_main, whose stack is internal, so reading flash is fine
    nvs_handle_t h;
    if (nvs_open(SPENT_NS, NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_i32(h, "day", &s_spent_day) != ESP_OK || nvs_get_i32(h, "spent", &s_spent) != ESP_OK) {
            s_spent_day = -1;
            s_spent = 0;
        }
        nvs_close(h);
    }
    s_flights = heap_caps_calloc(FLIGHTS_N, sizeof(flight_entry_t), MALLOC_CAP_SPIRAM);
    s_aircraft = heap_caps_calloc(AIRCRAFT_N, sizeof(aircraft_entry_t), MALLOC_CAP_SPIRAM);
    s_resp = heap_caps_malloc(sizeof(resp_t), MALLOC_CAP_SPIRAM);
    SemaphoreHandle_t lock = xSemaphoreCreateMutex();
    // TLS and the request are on the heap; this is call depth, as for route.c
    if (s_flights && s_aircraft && s_resp && lock &&
        (xTaskCreateWithCaps(adb_task, "adb", 8192, NULL, 3, &s_task, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) == pdPASS ||
         xTaskCreate(adb_task, "adb", 8192, NULL, 3, &s_task) == pdPASS)) {
        s_lock = lock; // last: lookups take it as everything ready; the task waits for one first
        return;
    }
    ESP_LOGE(TAG, "not started: no memory");
    if (lock) vSemaphoreDelete(lock);
    heap_caps_free(s_flights);
    heap_caps_free(s_aircraft);
    heap_caps_free(s_resp);
    s_flights = NULL;
    s_aircraft = NULL;
    s_resp = NULL;
}

// Letters and digits only: it goes into the URL as it is
static bool plain(const char *s, size_t max)
{
    size_t n = 0;
    for (; s[n]; n++) {
        const char c = s[n];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))) return false;
    }
    return n > 0 && n < max;
}

// Whether to ask about something last asked at asked_us; with the lock held
static bool may_ask(int64_t asked_us)
{
    return !asked_us || esp_timer_get_time() - asked_us > RETRY_US;
}

bool adb_flight(const char *callsign, const char *hex, double lat, double lon, bool ask, adb_flight_t *out)
{
    *out = (adb_flight_t){0};
    if (!s_lock || !settings()->adb_key[0] || !plain(callsign, sizeof(s_want_cs))) return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    flight_entry_t *e = find_flight(callsign);
    if (e) e->used = ++s_clock;
    bool found = false;
    if (e && e->fetched_us) {
        found = e->found;
        if (found) *out = e->f;
    }
    const int64_t age = e && e->fetched_us ? esp_timer_get_time() - e->fetched_us : INT64_MAX;
    const bool stale = !e || !e->fetched_us || age > (found ? REFRESH_US : UNKNOWN_US);
    if (ask && stale && !(found && out->arrived) && may_ask(e ? e->asked_us : 0) && strcmp(s_want_cs, callsign) != 0) {
        // One waiting at a time: the one this replaces was never asked for,
        // so let it ask again at once
        flight_entry_t *gone = s_want_cs[0] ? find_flight(s_want_cs) : NULL;
        if (gone) gone->asked_us = 0;
        strlcpy(s_want_cs, callsign, sizeof(s_want_cs));
        strlcpy(s_want_cs_hex, plain(hex, sizeof(s_want_cs_hex)) ? hex : "", sizeof(s_want_cs_hex));
        s_want_lat = lat;
        s_want_lon = lon;
        if (!e) e = add_flight(callsign);
        e->used = ++s_clock;
        e->asked_us = esp_timer_get_time(); // not again every second while it is out
        xTaskNotifyGive(s_task);
    }
    xSemaphoreGive(s_lock);
    return found;
}

bool adb_aircraft(const char *hex, bool ask, adb_aircraft_t *out)
{
    *out = (adb_aircraft_t){0};
    if (!s_lock || !settings()->adb_key[0] || !plain(hex, sizeof(s_want_hex))) return false; // "~": not an ICAO address
    xSemaphoreTake(s_lock, portMAX_DELAY);
    aircraft_entry_t *e = find_aircraft(hex);
    if (e) e->used = ++s_clock;
    const bool found = e && e->done && e->found;
    if (found) *out = e->a;
    if (ask && !(e && e->done) && may_ask(e ? e->asked_us : 0) && strcmp(s_want_hex, hex) != 0) {
        aircraft_entry_t *gone = s_want_hex[0] ? find_aircraft(s_want_hex) : NULL;
        if (gone && !gone->done) gone->asked_us = 0;
        strlcpy(s_want_hex, hex, sizeof(s_want_hex));
        if (!e) e = add_aircraft(hex);
        e->used = ++s_clock;
        e->asked_us = esp_timer_get_time();
        xTaskNotifyGive(s_task);
    }
    xSemaphoreGive(s_lock);
    return found;
}

void adb_get_status(adb_status_t *out)
{
    *out = (adb_status_t){.state = "off"};
    if (!s_lock || !settings()->adb_key[0]) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    roll_day();
    out->state = s_state;
    strlcpy(out->detail, s_detail, sizeof(out->detail));
    out->used_today = s_used_today;
    out->lookups = s_lookups;
    if (strcmp(settings()->adb_key, s_bad_key) == 0) {
        out->state = "error";
        strlcpy(out->detail, "The key was refused", sizeof(out->detail));
    }
    xSemaphoreGive(s_lock);
}
