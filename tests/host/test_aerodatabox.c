// main/sources/aerodatabox.c on the PC: choosing the flight, "departed",
// the flight cache, credit accounting, and the debug timing run. No
// network: the fake client answers with the fixtures here.

#include "fake.h"
#define time(x) fake_time(x)
#define CONFIG_SPIRAM_XIP_FROM_PSRAM 1 // so the spending is stored
#define CONFIG_LV_USE_SNAPSHOT 1       // and the debug timing is built
#include "aerodatabox.c"

#include <stdlib.h>

// "2026-10-20 13:20Z", or as the airport's local time, for now + d seconds
static const char *at(int64_t d, bool utc)
{
    static char buf[8][32];
    static int k;
    char *b = buf[k++ % 8];
    const time_t t = g_now + d;
    struct tm tm;
    gmtime_r(&t, &tm);
    strftime(b, 32, utc ? "%Y-%m-%d %H:%MZ" : "%Y-%m-%d %H:%M+00:00", &tm);
    return b;
}

// One flight, LHR to JFK, leaving and arriving dep and arr seconds from now;
// extra goes into its departure. Freed by the caller.
static char *flight(const char *status, const char *callsign, int64_t dep, int64_t arr, const char *extra)
{
    char *b = malloc(1024);
    snprintf(b, 1024,
             "{\"number\":\"XX 1\",\"callSign\":\"%s\",\"status\":\"%s\","
             "\"departure\":{\"airport\":{\"iata\":\"LHR\",\"municipalityName\":\"London\","
             "\"location\":{\"lat\":51.47,\"lon\":-0.45}},\"scheduledTime\":{\"utc\":\"%s\",\"local\":\"%s\"}%s},"
             "\"arrival\":{\"airport\":{\"iata\":\"JFK\",\"municipalityName\":\"New York\","
             "\"location\":{\"lat\":40.64,\"lon\":-73.78}},\"scheduledTime\":{\"utc\":\"%s\",\"local\":\"%s\"}}}",
             callsign, status, at(dep, true), at(dep, false), extra, at(arr, true), at(arr, false));
    return b;
}

static char *runway(int64_t d)
{
    static char b[128];
    snprintf(b, sizeof(b), ",\"runwayTime\":{\"utc\":\"%s\",\"local\":\"%s\"}", at(d, true), at(d, false));
    return b;
}

static void pick(void)
{
    printf("Choosing the flight\n");
    char *old = flight("EnRoute", "BAW1", -10 * 3600, -8 * 3600, "");
    char *cur = flight("Expected", "BAW1", -20 * 60, 7 * 3600, "");
    char list[2400];
    snprintf(list, sizeof(list), "[%s,%s]", old, cur);
    cJSON *root = cJSON_Parse(list);
    int64_t nearest;
    CHECK(pick_flight(root, "BAW1", 51.4, -0.5, &nearest) == cJSON_GetArrayItem(root, 1),
          "a stale en-route leg doesn't hide the current scheduled one");
    cJSON_Delete(root);
    snprintf(list, sizeof(list), "[%s]", old);
    root = cJSON_Parse(list);
    CHECK(!pick_flight(root, "BAW1", 51.4, -0.5, &nearest), "only a stale leg: none (%lld min off)",
          (long long)nearest / 60);
    cJSON_Delete(root);
    free(old);
    free(cur);
}

static void departed(void)
{
    printf("\"Departed\"\n");
    adb_flight_t f;
    char *j = flight("Expected", "BAW1", 3600, 9 * 3600, runway(3600));
    cJSON *root = cJSON_Parse(j);
    parse_flight(root, 51.4, -0.5, &f);
    CHECK(!f.departed, "scheduled, with a runway estimate ahead: not departed");
    cJSON_Delete(root);
    free(j);

    j = flight("", "BAW1", -1200, 7 * 3600, runway(-600));
    root = cJSON_Parse(j);
    parse_flight(root, 51.4, -0.5, &f);
    CHECK(f.departed, "no status, a runway time past: departed");
    cJSON_Delete(root);
    free(j);

    j = flight("EnRoute", "BAW1", -1200, 7 * 3600, "");
    root = cJSON_Parse(j);
    parse_flight(root, 51.4, -0.5, &f);
    CHECK(f.departed && strcmp(f.route.codes, "LHR-JFK") == 0, "en route: departed, %s", f.route.codes);
    cJSON_Delete(root);
    free(j);
}

static void cache(void)
{
    printf("The flight cache\n");
    settings_t s = g_set;
    char *landed = flight("Arrived", "BAW1", -6 * 3600, -1800, "");
    char list[1200];
    snprintf(list, sizeof(list), "[%s]", landed);
    g_http = (struct fake_http){.body = list, .status = 200};
    lookup_flight(&s, "BAW1", "400aaa", 51.4, -0.5);
    adb_flight_t f;
    CHECK(adb_flight("BAW1", "400aaa", 51.4, -0.5, true, &f) && f.arrived, "the landed flight, just after");

    int n = g_notified;
    g_now += 86400;
    g_now_us += 86400 * 1000000LL;
    CHECK(!adb_flight("BAW1", "400aaa", 51.4, -0.5, true, &f) && g_notified == n + 1,
          "a day later: not shown, and asked again");
    g_now -= 86400;
    g_now_us -= 86400 * 1000000LL;
    s_want_cs[0] = '\0';
    find_flight("BAW1")->asked_us = 0;

    n = g_notified;
    CHECK(!adb_flight("BAW1", "400bbb", 51.4, -0.5, true, &f) && g_notified == n + 1,
          "another aircraft on the callsign: not given it, and asks");
    s_want_cs[0] = '\0';
    find_flight("BAW1")->asked_us = 0;

    lookup_flight(&s, "BAW1", "400aaa", 51.4, -0.5);
    g_now_us += LANDED_US + 1000000;
    n = g_notified;
    adb_flight("BAW1", "400aaa", 51.4, -0.5, true, &f);
    CHECK(g_notified == n + 1, "landed: asked again after %d h", (int)(LANDED_US / 3600000000LL));
    s_want_cs[0] = '\0';
    free(landed);
}

// The day's spending back to used, as stored
static void spent(int used)
{
    s_used_today = s_spent = g_nvs_spent = used;
}

static void credits(void)
{
    printf("Credits\n");
    settings_t s = g_set;
    s_day = -1;
    s_spent_day = -1;
    g_nvs_day = -1;
    spent(0);
    cJSON *root;
    g_http = (struct fake_http){.body = "[]", .status = 200};

    get(&s, "/x", 2, &root);
    cJSON_Delete(root);
    CHECK(g_http.requests == 1 && g_http.spent_at_send == 2, "the cost is stored before the request goes (%d)",
          g_http.spent_at_send);

    g_nvs_fail = 1;
    CHECK(get(&s, "/x", 2, &root) == NO_ANSWER && g_http.requests == 1 && s_used_today == 2,
          "it can't be stored: nothing is asked (%d used)", s_used_today);
    g_nvs_fail = 0;

    g_http.err = ESP_ERR_HTTP_CONNECT;
    get(&s, "/x", 2, &root);
    CHECK(s_used_today == 2 && g_nvs_spent == 2, "no connection: given back (%d used, %d stored)", s_used_today,
          g_nvs_spent);
    g_http.err = ESP_ERR_HTTP_FETCH_HEADER;
    get(&s, "/x", 2, &root);
    CHECK(s_used_today == 4 && g_nvs_spent == 4, "sent, then no answer: kept (%d used)", s_used_today);
    g_http.err = ESP_FAIL;
    g_http.err_after_body = 1;
    get(&s, "/x", 2, &root);
    CHECK(s_used_today == 6 && g_nvs_spent == 6, "200, then cut off part way: kept (%d used)", s_used_today);
    g_http = (struct fake_http){.body = "{}", .status = 500};
    get(&s, "/x", 2, &root);
    CHECK(s_used_today == 8, "a server error: kept (%d used)", s_used_today);
    g_http.status = 429;
    get(&s, "/x", 2, &root);
    CHECK(s_used_today == 8, "429: given back (%d used)", s_used_today);
    s_limit_us = 0;
    g_http.status = 401;
    get(&s, "/x", 2, &root);
    CHECK(s_used_today == 8, "401: given back (%d used)", s_used_today);
    s_bad_key[0] = '\0';

    spent(2);
    g_http = (struct fake_http){.body = "[]", .status = 200, .err = ESP_FAIL, .err_after_body = 1};
    for (int i = 0; i < 6; i++) get(&s, "/x", 2, &root);
    CHECK(g_http.requests == 4 && s_used_today == 10, "six cut off, limit 10: %d sent, %d recorded",
          g_http.requests, s_used_today);

    g_http = (struct fake_http){.body = "[]", .status = 200};
    spent(2);
    int asked = 0;
    for (int boot = 0; boot < 3; boot++) {
        s_day = -1;
        s_used_today = 0;
        s_spent_day = -1;
        s_spent = 0;
        adb_start(); // reads back what was stored
        for (int i = 0; i < 10; i++) {
            const int before = g_http.requests;
            get(&s, "/x", 2, &root);
            cJSON_Delete(root);
            asked += (g_http.requests - before) * 2;
        }
    }
    CHECK(asked == 8, "three boots under a limit of 10, 2 already used: %d more", asked);
}

static settings_t s_served;
static void serve_now(void)
{
    serve(&s_served);
}

static void timing(void)
{
    printf("The debug timing run\n");
    g_http = (struct fake_http){.body = "{}", .status = 200};
    s_served = g_set;
    char *out = malloc(512);
    const esp_err_t e = adb_debug_timing(3, true, out, 512); // nothing serves it, so it times out
    free(out); // as the web handler does
    CHECK(e == ESP_ERR_TIMEOUT, "the caller gave up");
    char other[512] = "";
    CHECK(adb_debug_timing(3, true, other, sizeof(other)) == ESP_ERR_INVALID_STATE,
          "another run is refused while it goes on");
    serve(&s_served); // it finishes, into its own buffer: AddressSanitizer would catch the freed one
    CHECK(!s_bench_running && strstr(s_bench_out, "\"ms\":["), "it finished into its own buffer");
    CHECK(adb_debug_timing(2, false, other, sizeof(other)) == ESP_ERR_TIMEOUT && !other[0],
          "its late \"done\" doesn't answer the next run");
    serve(&s_served);
    g_notify = serve_now;
    CHECK(adb_debug_timing(2, false, other, sizeof(other)) == ESP_OK && strstr(other, "\"keep\":false"),
          "a run served while waiting returns its result");
    g_notify = NULL;
}

int main(void)
{
    setenv("TZ", "UTC0", 1);
    tzset();
    strcpy(g_set.adb_key, "key");
    g_set.adb_daily = 10;
    adb_start();
    pick();
    departed();
    cache();
    credits();
    timing();
    return fake_done();
}
