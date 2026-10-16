#pragma once

#include <stdbool.h>

typedef enum {
    BATT_NO_PACK,     // monitor present, no pack fitted
    BATT_CHECKING,    // in range, but not yet long enough to rule out the no-pack cycle
    BATT_EXTERNAL,    // on external power, pack neither charging nor discharging
    BATT_CHARGING,
    BATT_DISCHARGING,
} batt_state_t;

typedef struct {
    batt_state_t state;
    float volts;
    float amps;        // > 0 discharging, < 0 charging
    int percent;       // estimated from voltage, 0-100
    int minutes_left;  // -1 unless discharging steadily
    bool low;          // at or below the warning level
    int shutdown_in_s; // > 0 while counting down to a low-voltage power-off
} battery_status_t;

// Monitor the battery and push its status to the UI. Enables charging.
// Does nothing on boards without a battery monitor.
void battery_start(void);

// The latest status. False on boards without a battery monitor.
bool battery_get(battery_status_t *out);
