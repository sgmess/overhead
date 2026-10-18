#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "board.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "battery.h"
#include "feed.h"
#include "geoloc.h"
#include "improv.h"
#include "net.h"
#include "openaip.h"
#include "ota.h"
#include "radar_ui.h"
#include "details.h"
#include "route.h"
#include "settings.h"
#include "stats.h"

static const char *TAG = "overhead";

// Woken early (a range tap, a settings change), still no sooner than this
// after the last request: two 70 ms apart drew HTTP 429 from adsb.fi
#define FETCH_GAP_MS 2000

#define FETCH_STACK (16 * 1024)

#define GEOLOC_RETRY_US (10 * 60 * 1000000LL)

static TaskHandle_t s_fetch_task;

// Runs in the LVGL task: wake the fetcher so the new range fills in at once.
static void on_range_change(void)
{
    // A swipe in the first seconds after boot comes before the task
    if (s_fetch_task) xTaskNotifyGive(s_fetch_task);
}

static void apply_tz(const char *tz)
{
    setenv("TZ", tz[0] ? tz : "UTC0", 1);
    tzset();
}

// A change from the configuration page or the settings screen, already the
// running settings: apply whatever isn't read afresh each time it's used
static void on_settings_changed(const settings_t *old, const settings_t *now)
{
    if (strcmp(now->tz, old->tz) != 0) apply_tz(now->tz);
    const bool moved = now->lat != old->lat || now->lon != old->lon;

    board_lock();
    if (moved) radar_ui_set_centre(now->lat, now->lon);
    radar_ui_apply_settings(old);
    board_unlock();

    if (moved || strcmp(now->openaip_key, old->openaip_key) != 0 ||
        strcmp(now->openaip_countries, old->openaip_countries) != 0 ||
        now->show_airspace != old->show_airspace || now->show_airfields != old->show_airfields) {
        openaip_restart();
    }
    if (now->feed != old->feed) feed_reset();
    // A new centre, feed or interval: fetch now rather than at the old pace
    if (moved || now->feed != old->feed || now->fetch_s != old->fetch_s) xTaskNotifyGive(s_fetch_task);
}

// Until connected: say what we're joining, and how to reach the setup
// portal while it's open
static void wait_for_network(void)
{
    char text[64];
    bool shown = false, setup = false;
    do {
        const char *ssid, *url;
        bool now = net_setup_open(&ssid, &url);
        if (!shown || now != setup) {
            board_lock();
            if (now) {
                radar_ui_show_setup(ssid, url);
                radar_ui_set_status("WiFi setup");
            } else {
                snprintf(text, sizeof(text), "Connecting to %s", settings()->ssid);
                radar_ui_set_status(text);
            }
            board_unlock();
            shown = true;
            setup = now;
        }
    } while (!net_wait_connected(1000));
    // On the network, so a just-installed update works well enough to keep
    ota_mark_valid();

    char ip[16] = "";
    net_get_ip(ip, sizeof(ip));
    // Until the first traffic arrives, which is usually a second or two
    snprintf(text, sizeof(text), "Settings at http://%s", ip);
    board_lock();
    radar_ui_show_setup(NULL, NULL);
    radar_ui_set_status(text);
    board_unlock();
}

static void fetch_task(void *arg)
{
    aircraft_t *buf = heap_caps_calloc(AC_MAX, sizeof(aircraft_t), MALLOC_CAP_SPIRAM);
    assert(buf);
    int64_t last_us = 0;
    bool located = false;
    int64_t locate_us = 0;

    for (;;) {
        if (!net_is_connected()) {
            wait_for_network();
        }
        // An update needs the link and the internal RAM to itself
        if (ota_installing()) {
            board_lock();
            radar_ui_set_status("Installing update");
            board_unlock();
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        // A new device: centred on where the connection is before its first
        // fetch, rather than on Heathrow
        if (!located && esp_timer_get_time() >= locate_us) {
            located = geoloc_locate();
            locate_us = esp_timer_get_time() + GEOLOC_RETRY_US;
            // The new centre woke this task, which is about to fetch anyway
            ulTaskNotifyTake(pdTRUE, 0);
        }

        const int64_t since_ms = (esp_timer_get_time() - last_us) / 1000;
        if (last_us && since_ms < FETCH_GAP_MS) vTaskDelay(pdMS_TO_TICKS(FETCH_GAP_MS - since_ms));
        last_us = esp_timer_get_time();

        // Around the radar's own centre, so the answer is drawn where it was asked for
        double lat, lon;
        board_lock();
        radar_ui_get_centre(&lat, &lon);
        const int range = radar_ui_range_nm();
        board_unlock();
        // Ask for a little beyond the scope edge so aircraft don't pop in at the rim
        int n = 0;
        esp_err_t err = feed_fetch(lat, lon, range + range / 10 + 1, buf, AC_MAX, &n);

        board_lock();
        // A new centre while it was out: these belong to the old one, and the
        // change woke this task to fetch again
        double now_lat, now_lon;
        radar_ui_get_centre(&now_lat, &now_lon);
        const bool moved = now_lat != lat || now_lon != lon;
        if (moved) {
            ESP_LOGI(TAG, "centre moved during the fetch, dropping it");
        } else if (err == ESP_OK) {
            radar_ui_set_aircraft(buf, n, feed_name());
            radar_ui_set_status("");
        } else {
            radar_ui_set_status("Feed error, retrying");
        }
        board_unlock();
        if (err == ESP_OK && !moved) stats_update(buf, n, lat, lon, range);

        // Internal RAM is the constraint, the Tab5's above all. The IDF low-water mark is
        // not logged: it misreads the DMA reserve pool as once full.
        ESP_LOGI(TAG, "heap: internal %u free, largest %u; psram %u free",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(settings()->fetch_s * 1000));
    }
}

void app_main(void)
{
    // The headroom left after esp_hosted's start-up allocations; see
    // radar_ui_create() for why it matters
    ESP_LOGI(TAG, "internal RAM at start: %u free, largest %u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    settings_load();
    // Local time for the clock and the dimming hours
    apply_tz(settings()->tz);
    // Early, so the web flasher finds it within its wait after installing
    improv_start();

    board_display_start(settings()->rotation);

    board_lock();
    radar_ui_create(settings()->lat, settings()->lon, on_range_change);
    radar_ui_set_status("Starting WiFi");
    board_unlock();

    // Before WiFi, which takes seconds to come up: the airspace kept from the
    // last fetch needs no network
    openaip_start();

    // Before WiFi: on the Tab5 this also turns charging on
    battery_start();

    board_wifi_power_on();
    feed_init();
    net_start();
    route_start();
    details_start();
    ota_start_auto_check();
    // TLS and JSON parsing buffers are in PSRAM; this is just call depth.
    xTaskCreate(fetch_task, "fetch", FETCH_STACK, NULL, 5, &s_fetch_task);
    // Last: a change can wake any of the above
    settings_start(on_settings_changed);
}
