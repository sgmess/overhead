#pragma once

#include <stdbool.h>
#include <stddef.h>

// WiFi with the network from settings(), SNTP, the web server and mDNS
// (http://overhead.local). With no network saved, or when it hasn't
// connected NET_SETUP_AFTER_S after boot, the setup portal opens: an open
// access point, NET_SETUP_SSID_PREFIX plus four hex digits of the MAC, whose
// DNS sends every name to the setup page. Retries of the saved network
// continue meanwhile, and the portal closes once it connects.
#define NET_SETUP_SSID_PREFIX "Overhead-"
#define NET_SETUP_AFTER_S 30
#define NET_HOSTNAME "overhead"

void net_start(void);

bool net_is_connected(void);

// WiFi is up (net_start() has run), so scans and net_try_network() work.
bool net_started(void);

// Join ssid now, without a restart: true once it has an address. After
// three failed attempts, or timeout_ms, it goes back to the saved network
// (or none). The caller stores the new one if it wants to keep it.
bool net_try_network(const char *ssid, const char *password, int timeout_ms);

typedef struct {
    char ssid[33];
    int rssi;
    bool open;
} net_ap_t;

// Networks in range, strongest first and each name once (mesh and
// dual-band access points repeat it). Blocks for a few seconds. The count,
// or -1 if the radio is busy (mid-connect, say).
int net_scan(net_ap_t *out, int max);

// Block until an IP address is assigned or timeout_ms passes.
bool net_wait_connected(int timeout_ms);

// The setup portal's state. When open, *ssid is its network name and *url
// its page; both are valid for the rest of the boot.
bool net_setup_open(const char **ssid, const char **url);

// The station's address, as text; false when not connected.
bool net_get_ip(char *buf, size_t n);

// Signal strength of the joined network, in dBm; 0 when not connected.
int net_rssi(void);
