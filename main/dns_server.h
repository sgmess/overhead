#pragma once

#include <stdint.h>

// Answer every A query with ip (network byte order), so a phone that joins
// the setup portal finds the setup page whatever name it looks up.
void dns_server_start(uint32_t ip);
void dns_server_stop(void);
