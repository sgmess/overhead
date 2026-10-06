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

// Block until an IP address is assigned or timeout_ms passes.
bool net_wait_connected(int timeout_ms);

// The setup portal's state. When open, *ssid is its network name and *url
// its page; both are valid for the rest of the boot.
bool net_setup_open(const char **ssid, const char **url);

// The station's address, as text; false when not connected.
bool net_get_ip(char *buf, size_t n);

// Signal strength of the joined network, in dBm; 0 when not connected.
int net_rssi(void);
