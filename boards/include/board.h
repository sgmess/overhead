#pragma once

#include <stdbool.h>

// Board bring-up. Each board has one implementation in boards/<board>/board/,
// and CMakeLists.txt puts exactly one of them in the build.
//
// Each also defines one of OVERHEAD_BOARD_P4_34C, OVERHEAD_BOARD_S3_28C or
// OVERHEAD_BOARD_TAB5 for code that lays out differently per board.

// Panel, touch and the LVGL task, with the backlight on. rotation_deg is
// clockwise from the panel's native orientation: 0, 90 or 270 on the Tab5;
// the round boards ignore it.
void board_display_start(int rotation_deg);

// Power the WiFi radio. Call before esp_wifi_init().
void board_wifi_power_on(void);

// LVGL mutex. Every lv_* call outside the LVGL task must hold it.
void board_lock(void);
void board_unlock(void);

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
