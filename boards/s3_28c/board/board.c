// Waveshare ESP32-S3-Touch-LCD-2.8C
#include "board.h"

#include "bsp/esp-bsp.h"

void board_display_start(void)
{
    // The BSP's defaults: one PSRAM frame buffer, since every extra one is
    // more PSRAM bandwidth competing with the panel scanout.
    bsp_display_start();
    bsp_display_backlight_on();
}

void board_wifi_power_on(void)
{
    // Native WiFi, nothing to power.
}

void board_lock(void)
{
    bsp_display_lock(-1);
}

void board_unlock(void)
{
    bsp_display_unlock();
}

bool board_battery_init(void)
{
    // GPIO 4 carries a divided battery voltage, but the divider ratio is
    // undocumented and unmeasured; traffic-display reports "no battery" on
    // this board for the same reason (its HARDWARE.md). A guessed constant
    // would show a percentage that only looks right.
    return false;
}

bool board_battery_read(board_battery_t *out)
{
    return false;
}

void board_power_off(void)
{
}
