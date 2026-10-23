#pragma once
#include "esp_err.h"
typedef void (*shutdown_handler_t)(void);
extern shutdown_handler_t g_shutdown;
static inline esp_err_t esp_register_shutdown_handler(shutdown_handler_t h)
{
    g_shutdown = h;
    return ESP_OK;
}
