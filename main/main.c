#include <stdlib.h>

#include "board.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "battery.h"
#include "feed.h"
#include "net.h"
#include "radar_ui.h"

static const char *TAG = "overhead";

#if CONFIG_IDF_TARGET_ESP32P4
#define FETCH_STACK (16 * 1024)
#else
#define FETCH_STACK (10 * 1024) // internal RAM is tight on the S3
#endif

static TaskHandle_t s_fetch_task;
static double s_lat, s_lon;

static void set_status(const char *text)
{
    board_lock();
    radar_ui_set_status(text);
    board_unlock();
}

// Runs in the LVGL task: wake the fetcher so the new range fills in at once.
static void on_range_change(void)
{
    xTaskNotifyGive(s_fetch_task);
}

static void fetch_task(void *arg)
{
    aircraft_t *buf = heap_caps_calloc(AC_MAX, sizeof(aircraft_t), MALLOC_CAP_SPIRAM);
    assert(buf);

    for (;;) {
        if (!net_is_connected()) {
            set_status("Connecting to " CONFIG_OVERHEAD_WIFI_SSID);
            while (!net_wait_connected(5000)) {
            }
            set_status("Loading traffic");
        }

        // Ask for a little beyond the scope edge so aircraft don't pop in at the rim
        int range = radar_ui_range_nm();
        int n = 0;
        esp_err_t err = feed_fetch(s_lat, s_lon, range + range / 10 + 1, buf, AC_MAX, &n);

        board_lock();
        if (err == ESP_OK) {
            radar_ui_set_aircraft(buf, n, feed_name());
            radar_ui_set_status("");
        } else {
            radar_ui_set_status("Feed error, retrying");
        }
        board_unlock();

        // Internal RAM is the constraint on the S3. The IDF low-water mark is
        // not logged: it misreads the DMA reserve pool as once full.
        ESP_LOGI(TAG, "heap: internal %u free, largest %u; psram %u free",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(CONFIG_OVERHEAD_FETCH_INTERVAL_SEC * 1000));
    }
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    s_lat = strtod(CONFIG_OVERHEAD_CENTER_LAT, NULL);
    s_lon = strtod(CONFIG_OVERHEAD_CENTER_LON, NULL);
    ESP_LOGI(TAG, "centre %.4f, %.4f", s_lat, s_lon);

    board_display_start();

    board_lock();
    radar_ui_create(s_lat, s_lon, on_range_change);
    radar_ui_set_status("Starting WiFi");
    board_unlock();

    // Before WiFi: on the Tab5 this also turns charging on
    battery_start();

    board_wifi_power_on();
    net_start();
    // TLS and JSON parsing buffers are in PSRAM; this is just call depth.
    xTaskCreate(fetch_task, "fetch", FETCH_STACK, NULL, 5, &s_fetch_task);
}
