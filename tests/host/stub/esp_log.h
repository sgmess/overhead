#pragma once
#include <stdio.h>
// Shown only with VERBOSE=1, so the checks stay readable
extern int g_verbose;
#define ESP_LOG_(l, t, ...) (g_verbose ? (printf("    %s %s: ", l, t), printf(__VA_ARGS__), printf("\n")) : 0)
#define ESP_LOGE(t, ...) ESP_LOG_("E", t, __VA_ARGS__)
#define ESP_LOGW(t, ...) ESP_LOG_("W", t, __VA_ARGS__)
#define ESP_LOGI(t, ...) ESP_LOG_("I", t, __VA_ARGS__)
#define ESP_LOGD(t, ...) ((void)0)
