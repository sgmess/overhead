#pragma once
// A fake client: perform() hands the handler g_http.body and answers
// g_http.status, or fails with g_http.err (after part of the body when
// g_http.err_after_body)
#include <stddef.h>
#include "esp_err.h"
#define ESP_ERR_HTTP_CONNECT 0x7002
#define ESP_ERR_HTTP_FETCH_HEADER 0x7004
typedef enum {
    HTTP_EVENT_ON_CONNECTED,
    HTTP_EVENT_ON_HEADER,
    HTTP_EVENT_ON_DATA,
    HTTP_EVENT_ON_FINISH,
    HTTP_EVENT_DISCONNECTED,
} esp_http_client_event_id_t;
typedef enum { HTTP_METHOD_GET, HTTP_METHOD_POST } esp_http_client_method_t;
typedef struct {
    esp_http_client_event_id_t event_id;
    void *user_data;
    void *data;
    int data_len;
    char *header_key, *header_value;
} esp_http_client_event_t;
typedef esp_err_t (*http_event_handle_cb)(esp_http_client_event_t *);
typedef struct {
    const char *url;
    esp_http_client_method_t method;
    http_event_handle_cb event_handler;
    void *user_data;
    esp_err_t (*crt_bundle_attach)(void *);
    int timeout_ms;
    int buffer_size;
    const char *user_agent;
    int keep_alive_enable;
} esp_http_client_config_t;
typedef struct fake_client *esp_http_client_handle_t;
esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *c);
esp_err_t esp_http_client_set_url(esp_http_client_handle_t c, const char *url);
esp_err_t esp_http_client_set_header(esp_http_client_handle_t c, const char *k, const char *v);
esp_err_t esp_http_client_set_post_field(esp_http_client_handle_t c, const char *data, int len);
esp_err_t esp_http_client_perform(esp_http_client_handle_t c);
int esp_http_client_get_status_code(esp_http_client_handle_t c);
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t c);

struct fake_http {
    const char *body;
    int status;
    esp_err_t err;
    int err_after_body;
    int requests;
    int spent_at_send; // the stored spending when the last request went
    char last_url[256];
};
extern struct fake_http g_http;
