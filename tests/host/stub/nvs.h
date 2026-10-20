#pragma once
// A fake NVS of two numbers, which the tests can make fail
#include <stdint.h>
#include "esp_err.h"
typedef int nvs_handle_t;
#define NVS_READWRITE 1
#define NVS_READONLY 0
extern int g_nvs_fail, g_nvs_writes;
extern int32_t g_nvs_day, g_nvs_spent;
static inline esp_err_t nvs_open(const char *ns, int mode, nvs_handle_t *h)
{
    (void)ns;
    (void)mode;
    *h = 1;
    return ESP_OK;
}
static inline esp_err_t nvs_set_i32(nvs_handle_t h, const char *k, int32_t v)
{
    (void)h;
    if (k[0] == 'd') g_nvs_day = v;
    else g_nvs_spent = v;
    return ESP_OK;
}
static inline esp_err_t nvs_get_i32(nvs_handle_t h, const char *k, int32_t *v)
{
    (void)h;
    *v = k[0] == 'd' ? g_nvs_day : g_nvs_spent;
    return ESP_OK;
}
static inline esp_err_t nvs_commit(nvs_handle_t h)
{
    (void)h;
    g_nvs_writes++;
    return g_nvs_fail ? ESP_FAIL : ESP_OK;
}
static inline void nvs_close(nvs_handle_t h)
{
    (void)h;
}
