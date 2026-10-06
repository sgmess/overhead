#include "battery.h"

#include <math.h>

#include "board.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "radar_ui.h"

static const char *TAG = "battery";

#define POLL_MS 500
#define UI_EVERY_MS 2000
#define LOG_EVERY_MS 30000
#define SMOOTHING 0.05f   // EMA weight per sample: ~10 s time constant at 500 ms
#define STATE_AMPS 0.05f  // below this either way, the pack is idle

// With no pack fitted the charger's output doesn't sit at 0 V: it alternates
// ~11 s at 8.38 V and ~5.5 s at 4.2 V (measured on this Tab5, 2026-10-06; the
// same effect is noted in yejun/tab5-fancy-clock). So one reading outside a
// pack's range means no pack, and a pack is only believed once it has stayed
// in range for longer than a whole cycle. tab5-fancy-clock waits 8 s, but the
// high phase alone is 11 s here and would pass for a pack.
#define PACK_MIN_VOLTS 5.5f // the pack cuts out at 6.0 V, so it never reads lower
#define PACK_MAX_VOLTS 9.0f
#define PACK_CONFIRM_MS 20000

// Tab5 pack: NP-F550, 2S Li-ion, 7.4 V 2000 mAh (14.8 Wh)
#define CELLS 2
#define PACK_WH 14.8f
// Assumed internal resistance, to undo the sag under load (and the rise
// while charging) before reading the charge level off the voltage curve
#define PACK_OHMS 0.12f

#define LOW_PERCENT 15
#define CRITICAL_HOLD_MS 30000 // below the shutdown voltage this long, then...
#define COUNTDOWN_MS 10000     // ...warn on screen for this long, then power off

// Resting voltage per cell against charge, for a typical Li-ion cell. M5Stack
// give 8.23 V (4.115 V/cell) as full on the Tab5.
static const struct {
    float volts;
    int percent;
} CURVE[] = {
    {3.30f, 0},  {3.50f, 5},  {3.61f, 10}, {3.69f, 20}, {3.73f, 30},  {3.77f, 40},
    {3.81f, 50}, {3.86f, 60}, {3.92f, 70}, {3.98f, 80}, {4.04f, 90}, {4.11f, 100},
};
#define CURVE_N ((int)(sizeof(CURVE) / sizeof(CURVE[0])))

static int percent_from_cell(float v)
{
    if (v <= CURVE[0].volts) return 0;
    for (int i = 1; i < CURVE_N; i++) {
        if (v < CURVE[i].volts) {
            float t = (v - CURVE[i - 1].volts) / (CURVE[i].volts - CURVE[i - 1].volts);
            return (int)lroundf(CURVE[i - 1].percent + t * (CURVE[i].percent - CURVE[i - 1].percent));
        }
    }
    return 100;
}

static void battery_task(void *arg)
{
    static const char *const NAMES[] = {"no pack", "checking", "external power", "charging", "discharging"};
    float volts = 0, amps = 0;
    bool primed = false, pack = false;
    int64_t in_range_ms = -1; // how long the voltage has stayed in a pack's range
    int discharging_ms = 0, since_ui = UI_EVERY_MS, since_log = LOG_EVERY_MS;
    batt_state_t last_state = BATT_CHECKING;
#ifdef CONFIG_OVERHEAD_BATTERY_AUTO_OFF
    const float shutdown_volts = CONFIG_OVERHEAD_BATTERY_SHUTDOWN_MV / 1000.0f;
    int critical_ms = 0, countdown_ms = -1; // -1: no countdown running
#endif

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
        board_battery_t raw;
        if (!board_battery_read(&raw)) continue;

        // Presence, from raw readings: smoothing would average the no-pack
        // cycle into a believable voltage
        if (raw.volts < PACK_MIN_VOLTS || raw.volts > PACK_MAX_VOLTS) {
            in_range_ms = -1;
            primed = false;
            if (pack) ESP_LOGW(TAG, "no pack (%.2f V)", raw.volts);
            pack = false;
        } else {
            in_range_ms = in_range_ms < 0 ? 0 : in_range_ms + POLL_MS;
            if (!pack && in_range_ms >= PACK_CONFIRM_MS) {
                pack = true;
                ESP_LOGI(TAG, "pack confirmed at %.2f V", raw.volts);
            }
        }

        if (!primed) {
            volts = raw.volts;
            amps = raw.amps;
            primed = true;
        }
        volts += SMOOTHING * (raw.volts - volts);
        amps += SMOOTHING * (raw.amps - amps);

        battery_status_t st = {.volts = volts, .amps = amps, .minutes_left = -1};
        if (!pack) {
            // A dip since boot (or since the last one) means none is fitted;
            // in range but not yet for a whole cycle means we can't tell yet
            st.state = in_range_ms < 0 ? BATT_NO_PACK : BATT_CHECKING;
        } else if (amps > STATE_AMPS) {
            st.state = BATT_DISCHARGING;
        } else if (amps < -STATE_AMPS) {
            st.state = BATT_CHARGING;
        } else {
            st.state = BATT_EXTERNAL;
        }
        // Once a pack is out, keep saying so through the high phase of the
        // cycle rather than flipping back to "checking" every 5 s
        if (st.state == BATT_CHECKING && last_state == BATT_NO_PACK) st.state = BATT_NO_PACK;

        if (pack) {
            st.percent = percent_from_cell((volts + amps * PACK_OHMS) / CELLS);
            st.low = st.percent <= LOW_PERCENT;
        }

        // Time left only once the load has settled for a minute
        discharging_ms = st.state == BATT_DISCHARGING ? discharging_ms + POLL_MS : 0;
        float watts = volts * amps;
        if (discharging_ms >= 60000 && watts > 0.3f) {
            st.minutes_left = (int)(st.percent / 100.0f * PACK_WH / watts * 60.0f);
        }

        bool power_off = false;
#ifdef CONFIG_OVERHEAD_BATTERY_AUTO_OFF
        // Below 6.0 V the pack latches into protection and has to be taken out
        // and refitted, so power off cleanly before it gets there. Only with a
        // confirmed pack; plugging in during the countdown cancels it.
        if (st.state == BATT_DISCHARGING && volts < shutdown_volts) {
            critical_ms += POLL_MS;
        } else {
            critical_ms = 0;
            countdown_ms = -1;
        }
        if (critical_ms >= CRITICAL_HOLD_MS && countdown_ms < 0) {
            countdown_ms = COUNTDOWN_MS;
            ESP_LOGW(TAG, "pack at %.2f V, under %.2f V for %d s: powering off in %d s", volts, shutdown_volts,
                     CRITICAL_HOLD_MS / 1000, COUNTDOWN_MS / 1000);
        }
        if (countdown_ms >= 0) {
            st.shutdown_in_s = countdown_ms > 0 ? (countdown_ms + 999) / 1000 : 1;
            power_off = countdown_ms == 0;
            countdown_ms = countdown_ms > POLL_MS ? countdown_ms - POLL_MS : 0;
        }
#endif

        // Redraw every 2 s, or at once when the state changes
        since_ui += POLL_MS;
        if (since_ui >= UI_EVERY_MS || st.state != last_state || power_off) {
            board_lock();
            radar_ui_set_battery(&st);
            board_unlock();
            since_ui = 0;
        }
        last_state = st.state;

#ifdef CONFIG_OVERHEAD_BATTERY_AUTO_OFF
        if (power_off) {
            board_power_off();
            critical_ms = 0; // still running: start over
            countdown_ms = -1;
        }
#endif

        since_log += POLL_MS;
        if (since_log >= LOG_EVERY_MS) {
            if (pack) {
                ESP_LOGI(TAG, "%.2f V %+.2f A, %s, ~%d%%", volts, amps, NAMES[st.state], st.percent);
            } else {
                ESP_LOGI(TAG, "%.2f V, %s", raw.volts, NAMES[st.state]);
            }
            since_log = 0;
        }
    }
}

void battery_start(void)
{
    if (!board_battery_init()) return;
    xTaskCreate(battery_task, "battery", 4096, NULL, 3, NULL);
}
