#include "dns_server.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

static const char *TAG = "dns";

#define DNS_PORT 53
#define DNS_MAX_LEN 512
#define HEADER_LEN 12
#define ANSWER_LEN 16 // name pointer, type, class, TTL, length, IPv4 address
#define TYPE_A 1
#define TYPE_ANY 255

static volatile bool s_run;
static TaskHandle_t s_task;

static uint16_t get16(const uint8_t *p)
{
    return (uint16_t)(p[0] << 8 | p[1]);
}

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = v >> 8;
    p[1] = v & 0xff;
}

// Turn the query in buf into its answer in place; returns the reply length,
// or 0 to stay silent.
static int answer(uint8_t *buf, int len, uint32_t ip)
{
    if (len < HEADER_LEN || (buf[2] & 0x80) || get16(buf + 4) != 1) return 0; // a query, one question

    // Skip the name: length-prefixed labels up to a zero
    int p = HEADER_LEN;
    while (p < len && buf[p]) {
        if (buf[p] & 0xc0) return 0; // no compression in a question
        p += buf[p] + 1;
    }
    p++;
    if (p + 4 > len) return 0;
    const uint16_t type = get16(buf + p);
    p += 4; // type and class
    const bool a = type == TYPE_A || type == TYPE_ANY;
    if (a && p + ANSWER_LEN > DNS_MAX_LEN) return 0;

    // Header: response, recursion as asked and available, no error. Other
    // record types (AAAA mostly) get an empty answer, which makes the phone
    // fall back to the A record rather than wait.
    buf[2] = 0x80 | (buf[2] & 0x01);
    buf[3] = 0x80;
    put16(buf + 6, a ? 1 : 0);
    put16(buf + 8, 0);
    put16(buf + 10, 0);
    if (!a) return p; // drops anything after the question

    uint8_t *r = buf + p;
    put16(r, 0xc000 | HEADER_LEN); // the name is the question's
    put16(r + 2, TYPE_A);
    put16(r + 4, 1); // IN
    put16(r + 6, 0);
    put16(r + 8, 60); // TTL, seconds
    put16(r + 10, 4);
    memcpy(r + 12, &ip, 4);
    return p + ANSWER_LEN;
}

static void dns_task(void *arg)
{
    const uint32_t ip = (uint32_t)(uintptr_t)arg;
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(DNS_PORT),
        .sin_addr.s_addr = ip,
    };
    if (sock < 0 || bind(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        ESP_LOGE(TAG, "can't listen on port %d", DNS_PORT);
        if (sock >= 0) close(sock);
        s_task = NULL;
        vTaskDelete(NULL);
    }
    // Wake up now and then to notice dns_server_stop()
    struct timeval tv = {.tv_sec = 1};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    static uint8_t buf[DNS_MAX_LEN];
    while (s_run) {
        struct sockaddr_in from;
        socklen_t from_len = sizeof(from);
        int n = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr *)&from, &from_len);
        if (n <= 0) continue;
        n = answer(buf, n, ip);
        if (n > 0) sendto(sock, buf, n, 0, (struct sockaddr *)&from, from_len);
    }
    close(sock);
    s_task = NULL;
    vTaskDelete(NULL);
}

void dns_server_start(uint32_t ip)
{
    if (s_task) return;
    s_run = true;
    xTaskCreate(dns_task, "dns", 3072, (void *)(uintptr_t)ip, 4, &s_task);
}

void dns_server_stop(void)
{
    s_run = false; // the task closes its socket within a second
}
