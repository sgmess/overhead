#include "improv.h"

#include "sdkconfig.h"

#if CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG || CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG

#include <stdio.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hal/usb_serial_jtag_ll.h"
#include "net.h"
#include "settings.h"

static const char *TAG = "improv";

#define VERSION 1
#define MAX_DATA 128
#define POLL_MS 10
#define CONNECT_TIMEOUT_MS 40000 // the client gives up at 45 s
#define SCAN_MAX 20

enum { // packet types
    TYPE_STATE = 0x01,
    TYPE_ERROR = 0x02,
    TYPE_RPC = 0x03,
    TYPE_RPC_RESULT = 0x04,
};

enum { // states
    STATE_READY = 0x02,
    STATE_PROVISIONING = 0x03,
    STATE_PROVISIONED = 0x04,
};

enum { // errors
    ERROR_NONE = 0x00,
    ERROR_INVALID_RPC = 0x01,
    ERROR_UNKNOWN_RPC = 0x02,
    ERROR_UNABLE_TO_CONNECT = 0x03,
};

enum { // RPC commands
    RPC_WIFI_SETTINGS = 0x01,
    RPC_CURRENT_STATE = 0x02,
    RPC_DEVICE_INFO = 0x03,
    RPC_SCAN = 0x04,
};

static const uint8_t HEADER[6] = {'I', 'M', 'P', 'R', 'O', 'V'};

// ---------------------------------------------------------------- sending

// In one locked write, so a log line from another task can't split it
static void send(uint8_t type, const uint8_t *data, size_t len)
{
    uint8_t pkt[sizeof(HEADER) + 3 + 255 + 2];
    size_t n = 0;
    memcpy(pkt, HEADER, sizeof(HEADER));
    n += sizeof(HEADER);
    pkt[n++] = VERSION;
    pkt[n++] = type;
    pkt[n++] = len;
    memcpy(pkt + n, data, len);
    n += len;
    uint8_t sum = 0;
    for (size_t i = 0; i < n; i++) sum += pkt[i];
    pkt[n++] = sum;
    pkt[n++] = '\n';
    flockfile(stdout);
    fwrite(pkt, 1, n, stdout);
    fflush(stdout);
    funlockfile(stdout);
}

static void send_byte(uint8_t type, uint8_t value)
{
    send(type, &value, 1);
}

// An RPC result: the command, then strings, each with its length first
typedef struct {
    uint8_t data[255];
    size_t len;
} result_t;

static void result_init(result_t *r, uint8_t command)
{
    r->data[0] = command;
    r->data[1] = 0; // length of what follows, filled in as strings go in
    r->len = 2;
}

static void result_add(result_t *r, const char *s)
{
    size_t n = strlen(s);
    if (r->len + 1 + n > sizeof(r->data)) return;
    r->data[r->len++] = n;
    memcpy(r->data + r->len, s, n);
    r->len += n;
    r->data[1] = r->len - 2;
}

// The configuration page's address, which the flasher links to
static void send_provisioned(uint8_t command)
{
    char ip[16], url[32];
    send_byte(TYPE_STATE, STATE_PROVISIONED);
    if (!net_get_ip(ip, sizeof(ip))) return;
    snprintf(url, sizeof(url), "http://%s", ip);
    result_t r;
    result_init(&r, command);
    result_add(&r, url);
    send(TYPE_RPC_RESULT, r.data, r.len);
}

// ---------------------------------------------------------------- commands

static void wait_for_wifi(void)
{
    while (!net_started()) vTaskDelay(pdMS_TO_TICKS(100));
}

static void on_wifi_settings(const uint8_t *d, size_t len)
{
    // ssid length, ssid, password length, password
    char ssid[33], password[65];
    if (len < 2 || d[0] >= sizeof(ssid) || 1 + d[0] + 1 > len) {
        send_byte(TYPE_ERROR, ERROR_INVALID_RPC);
        return;
    }
    const size_t pw_len = d[1 + d[0]];
    if (pw_len >= sizeof(password) || 2 + d[0] + pw_len > len) {
        send_byte(TYPE_ERROR, ERROR_INVALID_RPC);
        return;
    }
    memcpy(ssid, d + 1, d[0]);
    ssid[d[0]] = '\0';
    memcpy(password, d + 2 + d[0], pw_len);
    password[pw_len] = '\0';

    send_byte(TYPE_STATE, STATE_PROVISIONING);
    wait_for_wifi();
    ESP_LOGI(TAG, "joining '%s'", ssid);
    if (!net_try_network(ssid, password, CONNECT_TIMEOUT_MS)) {
        ESP_LOGW(TAG, "couldn't join '%s'", ssid);
        send_byte(TYPE_ERROR, ERROR_UNABLE_TO_CONNECT);
        send_byte(TYPE_STATE, STATE_READY);
        return;
    }
    // Kept from now on. This writes flash, which is why this task's stack is
    // in internal RAM.
    settings_t s = *settings();
    strlcpy(s.ssid, ssid, sizeof(s.ssid));
    strlcpy(s.password, password, sizeof(s.password));
    if (settings_save(&s) != ESP_OK) ESP_LOGE(TAG, "network joined but not saved");
    send_provisioned(RPC_WIFI_SETTINGS);
}

static void on_device_info(void)
{
    const esp_app_desc_t *app = esp_app_get_description();
    result_t r;
    result_init(&r, RPC_DEVICE_INFO);
    result_add(&r, "Overhead");
    result_add(&r, app->version);
#if CONFIG_IDF_TARGET_ESP32P4
    result_add(&r, "ESP32-P4");
#else
    result_add(&r, CONFIG_IDF_TARGET);
#endif
    result_add(&r, app->project_name); // overhead-<board>
    send(TYPE_RPC_RESULT, r.data, r.len);
}

// One result per network, then an empty one to end the list
static void on_scan(void)
{
    static net_ap_t aps[SCAN_MAX];
    wait_for_wifi();
    int n = net_scan(aps, SCAN_MAX);
    char rssi[8];
    result_t r;
    for (int i = 0; i < n; i++) {
        result_init(&r, RPC_SCAN);
        snprintf(rssi, sizeof(rssi), "%d", aps[i].rssi);
        result_add(&r, aps[i].ssid);
        result_add(&r, rssi);
        result_add(&r, aps[i].open ? "NO" : "YES"); // "needs a password"
        send(TYPE_RPC_RESULT, r.data, r.len);
    }
    result_init(&r, RPC_SCAN);
    send(TYPE_RPC_RESULT, r.data, r.len);
}

static void on_rpc(const uint8_t *d, size_t len)
{
    // command, length, then its data
    if (len < 2 || d[1] + 2u > len) {
        send_byte(TYPE_ERROR, ERROR_INVALID_RPC);
        return;
    }
    send_byte(TYPE_ERROR, ERROR_NONE);
    switch (d[0]) {
    case RPC_WIFI_SETTINGS:
        on_wifi_settings(d + 2, d[1]);
        break;
    case RPC_CURRENT_STATE:
        if (net_is_connected()) {
            send_provisioned(RPC_CURRENT_STATE);
        } else {
            send_byte(TYPE_STATE, STATE_READY);
        }
        break;
    case RPC_DEVICE_INFO:
        on_device_info();
        break;
    case RPC_SCAN:
        on_scan();
        break;
    default:
        send_byte(TYPE_ERROR, ERROR_UNKNOWN_RPC);
    }
}

// ---------------------------------------------------------------- receiving

typedef struct {
    uint8_t buf[sizeof(HEADER) + 3 + MAX_DATA + 1];
    size_t n;
} parser_t;

// Feed one byte; handles a packet when one completes
static void feed(parser_t *p, uint8_t b)
{
    // Until the header matches, slide along: anything else is not for us
    if (p->n < sizeof(HEADER)) {
        if (b == HEADER[p->n]) {
            p->buf[p->n++] = b;
        } else {
            p->n = b == HEADER[0];
            if (p->n) p->buf[0] = b;
        }
        return;
    }
    p->buf[p->n++] = b;
    const size_t at_len = sizeof(HEADER) + 2;
    if (p->n <= at_len) return;
    const size_t len = p->buf[at_len];
    if (len > MAX_DATA) {
        p->n = 0;
        return;
    }
    if (p->n < at_len + 1 + len + 1) return;

    uint8_t sum = 0;
    for (size_t i = 0; i < p->n - 1; i++) sum += p->buf[i];
    const bool ok = sum == p->buf[p->n - 1] && p->buf[sizeof(HEADER)] == VERSION;
    const uint8_t type = p->buf[sizeof(HEADER) + 1];
    p->n = 0;
    if (!ok) {
        send_byte(TYPE_ERROR, ERROR_INVALID_RPC);
        return;
    }
    if (type == TYPE_RPC) on_rpc(p->buf + at_len + 1, len);
}

// Polls the USB Serial/JTAG receive FIFO directly: the console only writes
// to it, so nothing else reads it. USB holds the host's data back while the
// FIFO is full, so polling loses nothing.
static void improv_task(void *arg)
{
    static parser_t parser;
    uint8_t chunk[64];
    for (;;) {
        int n = usb_serial_jtag_ll_rxfifo_data_available() ? usb_serial_jtag_ll_read_rxfifo(chunk, sizeof(chunk)) : 0;
        for (int i = 0; i < n; i++) feed(&parser, chunk[i]);
        if (!n) vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
}

void improv_start(void)
{
    // Internal stack: saving the network writes flash
    xTaskCreate(improv_task, "improv", 6144, NULL, 3, NULL);
}

#else

void improv_start(void)
{
}

#endif
