#include "display_watch.h"

#include <stdint.h>

#include "board.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "display_watch";

// The display can freeze for good: on the Tab5 the LVGL adapter's flush has
// been seen waiting forever for a frame buffer the panel never gives back,
// holding the display lock, so the screen stops and then everything waiting
// for the lock does too (docs/PERFORMANCE.md). Only a restart recovers it.
// Nothing holds the lock legitimately for more than a second or so.
#define CHECK_MS 5000
#define FROZEN_MS 15000

#define MAGIC 0x64697370 // "disp": the count below is ours, not power-on noise
static RTC_NOINIT_ATTR uint32_t s_magic, s_restarts; // kept through a restart

static void watch_task(void *arg)
{
    int64_t missing_since = 0;
    for (;;) {
        if (board_lock_timeout(CHECK_MS)) {
            board_unlock();
            missing_since = 0;
            vTaskDelay(pdMS_TO_TICKS(CHECK_MS));
            continue;
        }
        const int64_t now = esp_timer_get_time();
        if (!missing_since) missing_since = now - CHECK_MS * 1000LL;
        if (now - missing_since >= FROZEN_MS * 1000LL) {
            ESP_LOGE(TAG, "display frozen for %lld s: restarting", (now - missing_since) / 1000000);
            s_restarts++;
            esp_restart();
        }
    }
}

void display_watch_start(void)
{
    if (s_magic != MAGIC) {
        s_magic = MAGIC;
        s_restarts = 0;
    } else if (s_restarts) {
        ESP_LOGW(TAG, "restarted %lu time(s) since power-on after the display froze", (unsigned long)s_restarts);
    }
    xTaskCreate(watch_task, "display_watch", 2560, NULL, 2, NULL);
}

int display_watch_restarts(void)
{
    return s_magic == MAGIC ? (int)s_restarts : 0;
}
