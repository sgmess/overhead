#pragma once
#include "esp_err.h"
static inline esp_err_t esp_crt_bundle_attach(void *c)
{
    (void)c;
    return ESP_OK;
}
