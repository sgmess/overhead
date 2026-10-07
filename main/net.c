#include "net.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dns_server.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "lwip/inet.h"
#include "mdns.h"
#include "settings.h"
#include "web.h"

#if CONFIG_ESP_HOSTED_ENABLED
#include "esp_hosted.h"
#endif

static const char *TAG = "net";

#define BIT_CONNECTED BIT0
#define BIT_TRY_FAILED BIT1
#define TRY_ATTEMPTS 3

// While the portal is open each retry of the saved network scans every
// channel, which takes the access point off its own for a moment; often
// enough and phones on it give up.
#define RETRY_DURING_SETUP_MS 15000

// Timer callbacks only post these, so every WiFi call happens in the event
// loop task: in order, and never stalling esp_timer behind an RPC to the C6.
ESP_EVENT_DEFINE_BASE(NET_EVENT);
enum {
    NET_EV_OPEN_SETUP,
    NET_EV_RETRY,
    NET_EV_TRY,     // join the network in the event data, a net_creds_t
    NET_EV_RESTORE, // give up on that and go back to the saved one
};

typedef struct {
    char ssid[33];
    char password[65];
} net_creds_t;

static EventGroupHandle_t s_events;
static esp_netif_t *s_sta, *s_ap;
static bool s_have_network, s_ever_connected;
static volatile bool s_started;
static bool s_trying; // net_try_network() in progress
static int s_try_fails;
static volatile bool s_setup_open;
static char s_setup_ssid[24], s_setup_url[32];
static esp_timer_handle_t s_setup_timer, s_retry_timer;

static void set_station(const char *ssid, const char *password)
{
    wifi_config_t sta = {0};
    strlcpy((char *)sta.sta.ssid, ssid, sizeof(sta.sta.ssid));
    strlcpy((char *)sta.sta.password, password, sizeof(sta.sta.password));
    sta.sta.threshold.authmode = password[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    sta.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    esp_wifi_set_config(WIFI_IF_STA, &sta);
}

// Back to the saved network after a failed net_try_network()
static void restore_network(void)
{
    const settings_t *s = settings();
    s_trying = false;
    s_have_network = s->ssid[0] != '\0';
    if (s_have_network) {
        ESP_LOGW(TAG, "back to '%s'", s->ssid);
        set_station(s->ssid, s->password);
        esp_wifi_connect();
    }
}

static void post_from_timer(void *arg)
{
    esp_event_post(NET_EVENT, (int32_t)(intptr_t)arg, NULL, 0, 0);
}

static esp_timer_handle_t make_timer(const char *name, int32_t event)
{
    esp_timer_create_args_t args = {
        .callback = post_from_timer,
        .arg = (void *)(intptr_t)event,
        .name = name,
    };
    esp_timer_handle_t t;
    ESP_ERROR_CHECK(esp_timer_create(&args, &t));
    return t;
}

static void open_setup(void)
{
    if (s_setup_open) return;
    ESP_LOGW(TAG, "%s: opening the setup portal on '%s'",
             s_have_network ? "saved network not reached" : "no network saved", s_setup_ssid);

    // AP and station even with no network to join: scanning, for the setup
    // page's list, needs the station
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    wifi_config_t ap = {
        .ap = {
            .channel = 1,
            .authmode = WIFI_AUTH_OPEN,
            .max_connection = 4,
        },
    };
    strlcpy((char *)ap.ap.ssid, s_setup_ssid, sizeof(ap.ap.ssid));
    ap.ap.ssid_len = strlen(s_setup_ssid);
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));

    esp_netif_ip_info_t ip;
    esp_netif_get_ip_info(s_ap, &ip);
    char addr[16];
    inet_ntoa_r(ip.ip.addr, addr, sizeof(addr));
    snprintf(s_setup_url, sizeof(s_setup_url), "http://%s", addr);

    // DHCP option 114 (RFC 8910) names the portal outright; phones that
    // ignore it still find it through the DNS answers and their probe URLs
    esp_netif_dhcps_stop(s_ap);
    if (esp_netif_dhcps_option(s_ap, ESP_NETIF_OP_SET, ESP_NETIF_CAPTIVEPORTAL_URI, s_setup_url,
                               strlen(s_setup_url)) != ESP_OK) {
        ESP_LOGW(TAG, "DHCP captive portal option not set");
    }
    esp_netif_dhcps_start(s_ap);

    dns_server_start(ip.ip.addr);
    s_setup_open = true;
}

static void close_setup(void)
{
    if (!s_setup_open) return;
    ESP_LOGI(TAG, "connected, closing the setup portal");
    dns_server_stop();
    esp_wifi_set_mode(WIFI_MODE_STA);
    s_setup_open = false;
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (s_have_network) esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *ev = data;
        xEventGroupClearBits(s_events, BIT_CONNECTED);
        if (s_trying) {
            // ASSOC_LEAVE is net_try_network() leaving the old network
            if (ev->reason == WIFI_REASON_ASSOC_LEAVE) return;
            ESP_LOGW(TAG, "new network: attempt %d failed (reason %d)", s_try_fails + 1, ev->reason);
            if (++s_try_fails < TRY_ATTEMPTS) {
                esp_wifi_connect();
            } else {
                restore_network();
                xEventGroupSetBits(s_events, BIT_TRY_FAILED);
            }
        } else if (s_setup_open) {
            esp_timer_start_once(s_retry_timer, RETRY_DURING_SETUP_MS * 1000LL);
        } else {
            ESP_LOGW(TAG, "disconnected (reason %d), retrying", ev->reason);
            // Each attempt scans first, so this does not spin even when the AP is down.
            esp_wifi_connect();
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STACONNECTED) {
        const wifi_event_ap_staconnected_t *ev = data;
        ESP_LOGI(TAG, "setup portal: " MACSTR " joined", MAC2STR(ev->mac));
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *ev = data;
        ESP_LOGI(TAG, "got IP " IPSTR ", settings at http://" IPSTR " or http://" NET_HOSTNAME ".local",
                 IP2STR(&ev->ip_info.ip), IP2STR(&ev->ip_info.ip));
        s_ever_connected = true;
        s_trying = false;
        esp_timer_stop(s_setup_timer);
        close_setup();
        xEventGroupSetBits(s_events, BIT_CONNECTED);
    } else if (base == NET_EVENT && id == NET_EV_OPEN_SETUP) {
        // Only if this boot never connected: a router restarting later
        // shouldn't put an open access point up
        if (!s_ever_connected) open_setup();
    } else if (base == NET_EVENT && id == NET_EV_RETRY) {
        if (!net_is_connected() && !s_trying) esp_wifi_connect();
    } else if (base == NET_EVENT && id == NET_EV_TRY) {
        const net_creds_t *c = data;
        ESP_LOGI(TAG, "trying '%s'", c->ssid);
        s_trying = true;
        s_try_fails = 0;
        s_have_network = true;
        esp_timer_stop(s_retry_timer);
        esp_wifi_disconnect();
        set_station(c->ssid, c->password);
        esp_wifi_connect();
    } else if (base == NET_EVENT && id == NET_EV_RESTORE) {
        if (s_trying) restore_network();
    }
}

static void start_mdns(void)
{
    esp_err_t err = mdns_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mDNS off: %s", esp_err_to_name(err));
        return;
    }
    mdns_hostname_set(NET_HOSTNAME);
    mdns_instance_name_set("Overhead radar");
    mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
}

void net_start(void)
{
    const settings_t *cfg = settings();
    s_have_network = cfg->ssid[0] != '\0';

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_BASE);
    snprintf(s_setup_ssid, sizeof(s_setup_ssid), NET_SETUP_SSID_PREFIX "%02X%02X", mac[4], mac[5]);

    s_events = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_sta = esp_netif_create_default_wifi_sta();
    s_ap = esp_netif_create_default_wifi_ap();
    esp_netif_set_hostname(s_sta, NET_HOSTNAME);

    // Calls go to the ESP32-C6 over SDIO via esp_wifi_remote / esp_hosted.
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    // Credentials live in our own NVS namespace, not the driver's copy
    esp_wifi_set_storage(WIFI_STORAGE_RAM);

#if CONFIG_ESP_HOSTED_ENABLED
    // The host library and the C6's firmware must speak the same esp_hosted
    // protocol; log both so a mismatch after a C6 update is easy to spot.
    esp_hosted_coprocessor_fwver_t fw = {0};
    if (esp_hosted_get_coprocessor_fwversion(&fw) == ESP_OK) {
        ESP_LOGI(TAG, "co-processor esp_hosted firmware %" PRIu32 ".%" PRIu32 ".%" PRIu32,
                 fw.major1, fw.minor1, fw.patch1);
    } else {
        ESP_LOGW(TAG, "co-processor firmware version unavailable");
    }
#endif
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(NET_EVENT, ESP_EVENT_ANY_ID, on_event, NULL));
    s_setup_timer = make_timer("setup", NET_EV_OPEN_SETUP);
    s_retry_timer = make_timer("retry", NET_EV_RETRY);

    if (s_have_network) {
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        set_station(cfg->ssid, cfg->password);
        ESP_ERROR_CHECK(esp_wifi_start());
        ESP_LOGI(TAG, "connecting to '%s'", cfg->ssid);
        esp_timer_start_once(s_setup_timer, NET_SETUP_AFTER_S * 1000000LL);
    } else {
        ESP_ERROR_CHECK(esp_wifi_start());
        esp_event_post(NET_EVENT, NET_EV_OPEN_SETUP, NULL, 0, portMAX_DELAY);
    }

    s_started = true;
    web_start();
    start_mdns();

    esp_sntp_config_t sntp = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    esp_netif_sntp_init(&sntp);
}

bool net_is_connected(void)
{
    return xEventGroupGetBits(s_events) & BIT_CONNECTED;
}

bool net_started(void)
{
    return s_started;
}

bool net_try_network(const char *ssid, const char *password, int timeout_ms)
{
    if (!s_started) return false;
    net_creds_t c;
    strlcpy(c.ssid, ssid, sizeof(c.ssid));
    strlcpy(c.password, password, sizeof(c.password));
    xEventGroupClearBits(s_events, BIT_CONNECTED | BIT_TRY_FAILED);
    esp_event_post(NET_EVENT, NET_EV_TRY, &c, sizeof(c), portMAX_DELAY);
    EventBits_t bits = xEventGroupWaitBits(s_events, BIT_CONNECTED | BIT_TRY_FAILED, pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(timeout_ms));
    if (bits & BIT_CONNECTED) return true;
    if (!(bits & BIT_TRY_FAILED)) esp_event_post(NET_EVENT, NET_EV_RESTORE, NULL, 0, portMAX_DELAY);
    return false;
}

static int cmp_rssi(const void *a, const void *b)
{
    return ((const wifi_ap_record_t *)b)->rssi - ((const wifi_ap_record_t *)a)->rssi;
}

int net_scan(net_ap_t *out, int max)
{
    if (!s_started || esp_wifi_scan_start(NULL, true) != ESP_OK) return -1;
    uint16_t n = 32;
    wifi_ap_record_t *aps = calloc(n, sizeof(*aps));
    if (!aps) {
        esp_wifi_clear_ap_list();
        return -1;
    }
    esp_wifi_scan_get_ap_records(&n, aps);
    qsort(aps, n, sizeof(*aps), cmp_rssi);
    int count = 0;
    for (int i = 0; i < n && count < max; i++) {
        const char *ssid = (const char *)aps[i].ssid;
        if (!ssid[0]) continue;
        bool seen = false;
        for (int j = 0; j < count && !seen; j++) seen = strcmp(ssid, out[j].ssid) == 0;
        if (seen) continue;
        strlcpy(out[count].ssid, ssid, sizeof(out[count].ssid));
        out[count].rssi = aps[i].rssi;
        out[count].open = aps[i].authmode == WIFI_AUTH_OPEN;
        count++;
    }
    free(aps);
    return count;
}

bool net_wait_connected(int timeout_ms)
{
    return xEventGroupWaitBits(s_events, BIT_CONNECTED, pdFALSE, pdTRUE,
                               pdMS_TO_TICKS(timeout_ms)) & BIT_CONNECTED;
}

bool net_setup_open(const char **ssid, const char **url)
{
    if (!s_setup_open) return false;
    if (ssid) *ssid = s_setup_ssid;
    if (url) *url = s_setup_url;
    return true;
}

bool net_get_ip(char *buf, size_t n)
{
    esp_netif_ip_info_t ip;
    if (!net_is_connected() || esp_netif_get_ip_info(s_sta, &ip) != ESP_OK) return false;
    inet_ntoa_r(ip.ip.addr, buf, n);
    return true;
}

int net_rssi(void)
{
    wifi_ap_record_t ap;
    if (!net_is_connected() || esp_wifi_sta_get_ap_info(&ap) != ESP_OK) return 0;
    return ap.rssi;
}
