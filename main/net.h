#pragma once

#include <stdbool.h>

// Start WiFi station mode (reconnects forever) and SNTP.
void net_start(void);

bool net_is_connected(void);

// Block until an IP address is assigned or timeout_ms passes.
bool net_wait_connected(int timeout_ms);
