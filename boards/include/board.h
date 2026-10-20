#pragma once

#include <stdbool.h>

// Board bring-up. Each board has one implementation in boards/<board>/board/,
// and CMakeLists.txt puts exactly one of them in the build.
//
// Each also defines OVERHEAD_BOARD_P4_34C or OVERHEAD_BOARD_TAB5 for code
// that lays out differently per board.

// What the board has, for the configuration page and the settings screen
#if defined(OVERHEAD_BOARD_TAB5)
#define BOARD_NAME "M5Stack Tab5"
#define BOARD_CAN_ROTATE 1
#define BOARD_HAS_SPEAKER 1
#elif defined(OVERHEAD_BOARD_P4_34C)
#define BOARD_NAME "Waveshare ESP32-P4-WIFI6-Touch-LCD-3.4C"
#define BOARD_CAN_ROTATE 0
#define BOARD_HAS_SPEAKER 1 // ES8311, on the speaker header
#else
#error "Unknown OVERHEAD_BOARD"
#endif

// Panel, touch and the LVGL task, with the backlight on. rotation_deg is
// clockwise from the panel's native orientation: 0, 90 or 270 on the Tab5;
// the round 3.4C ignores it.
void board_display_start(int rotation_deg);

// Backlight, 0..100 percent. Safe from any task.
void board_set_brightness(int percent);

// Play a short two-tone chime without blocking, on boards with a speaker
// (both have one).
void board_chime(void);

// Power the WiFi radio. Call before esp_wifi_init().
void board_wifi_power_on(void);

// LVGL mutex. Every lv_* call outside the LVGL task must hold it.
void board_lock(void);
void board_unlock(void);
// The same, waiting at most timeout_ms: false, without it, if not had in time.
// A negative timeout waits for ever, as board_lock() does.
bool board_lock_timeout(int timeout_ms);

// Battery, on boards with a monitor (only the Tab5 so far).
typedef struct {
    float volts; // pack voltage
    float amps;  // > 0 discharging, < 0 charging
} board_battery_t;

// Set up the monitor and enable charging. False when the board has none.
bool board_battery_init(void);

// One reading. False on boards without a monitor or on a bus error.
bool board_battery_read(board_battery_t *out);

// Cut the power, on boards that can. Returns if the power stayed on.
void board_power_off(void);
