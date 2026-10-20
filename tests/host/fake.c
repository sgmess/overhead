#include "fake.h"

#include <stdlib.h>
#include <string.h>

#include "esp_http_client.h"
#include "nvs.h"

#ifndef VERBOSE_DEFAULT
#define VERBOSE_DEFAULT 0
#endif
int g_verbose = VERBOSE_DEFAULT;
int64_t g_now_us = 1000000000LL;
time_t g_now = 1792500000; // 20 Oct 2026, 13:20 UTC
settings_t g_set;
int g_notified;
void (*g_notify)(void);
int g_fails;
int g_nvs_fail, g_nvs_writes;
int32_t g_nvs_day = -1, g_nvs_spent;
struct fake_http g_http = {.status = 200};

time_t fake_time(time_t *t)
{
    if (t) *t = g_now;
    return g_now;
}

const settings_t *settings(void)
{
    return &g_set;
}

void settings_copy(settings_t *out)
{
    *out = g_set;
}

void notify_hook(void)
{
    g_notified++;
    if (g_notify) g_notify();
}

struct fake_client {
    esp_http_client_config_t cfg;
};

esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *c)
{
    struct fake_client *k = calloc(1, sizeof(*k));
    k->cfg = *c;
    strncpy(g_http.last_url, c->url, sizeof(g_http.last_url) - 1);
    return k;
}

esp_err_t esp_http_client_set_url(esp_http_client_handle_t c, const char *url)
{
    (void)c;
    strncpy(g_http.last_url, url, sizeof(g_http.last_url) - 1);
    return ESP_OK;
}

esp_err_t esp_http_client_set_header(esp_http_client_handle_t c, const char *k, const char *v)
{
    (void)c;
    (void)k;
    (void)v;
    return ESP_OK;
}

esp_err_t esp_http_client_set_post_field(esp_http_client_handle_t c, const char *data, int len)
{
    (void)c;
    (void)data;
    (void)len;
    return ESP_OK;
}

static void deliver(struct fake_client *c, int len)
{
    esp_http_client_event_t e = {.event_id = HTTP_EVENT_ON_DATA, .user_data = c->cfg.user_data,
                                 .data = (void *)g_http.body, .data_len = len};
    c->cfg.event_handler(&e);
}

esp_err_t esp_http_client_perform(esp_http_client_handle_t c)
{
    g_http.requests++;
    g_http.spent_at_send = g_nvs_spent;
    if (g_http.err) {
        if (g_http.err_after_body && g_http.body) deliver(c, (int)strlen(g_http.body) / 2);
        return g_http.err;
    }
    if (g_http.body) deliver(c, (int)strlen(g_http.body));
    return ESP_OK;
}

int esp_http_client_get_status_code(esp_http_client_handle_t c)
{
    (void)c;
    return g_http.err && !g_http.err_after_body ? 0 : g_http.status;
}

esp_err_t esp_http_client_cleanup(esp_http_client_handle_t c)
{
    free(c);
    return ESP_OK;
}

int fake_done(void)
{
    if (g_fails) printf("\n%d FAILED\n", g_fails);
    else printf("  all passed\n");
    return g_fails != 0;
}
