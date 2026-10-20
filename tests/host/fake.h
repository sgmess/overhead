#pragma once
// What the host tests share: the fake clock, settings, HTTP and NVS state
// behind the headers in stub/, and CHECK.
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

#include "settings.h"

extern int64_t g_now_us; // uptime
extern time_t g_now;     // the wall clock; fake_time() answers it
time_t fake_time(time_t *t);

extern settings_t g_set; // what settings() and settings_copy() give
extern int g_notified;   // task notifications sent
extern void (*g_notify)(void); // and run at each, when set

extern int g_fails;
#define CHECK(cond, ...)                                                                                              \
    do {                                                                                                              \
        const bool ok_ = (cond);                                                                                      \
        printf("%s ", ok_ ? "  ok  " : "  FAIL");                                                                     \
        printf(__VA_ARGS__);                                                                                          \
        printf("\n");                                                                                                 \
        if (!ok_) g_fails++;                                                                                          \
    } while (0)

// 0 when every CHECK passed
int fake_done(void);
