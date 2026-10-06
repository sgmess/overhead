// Waveshare ESP32-P4-WIFI6-Touch-LCD-3.4C
#include "board.h"

#include "bsp/esp-bsp.h"

void board_display_start(int rotation_deg)
{
    (void)rotation_deg;
    bsp_display_cfg_t cfg = {
        .lv_adapter_cfg = ESP_LV_ADAPTER_DEFAULT_CONFIG(),
        .rotation = ESP_LV_ADAPTER_ROTATE_0,
        .tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_TRIPLE_PARTIAL,
    };
    bsp_display_start_with_config(&cfg);
    bsp_display_backlight_on();
}

void board_wifi_power_on(void)
{
    // The C6 is always powered; esp_hosted resets it over GPIO 54.
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
    return false; // no battery monitor on this board
}

bool board_battery_read(board_battery_t *out)
{
    return false;
}

void board_power_off(void)
{
}
