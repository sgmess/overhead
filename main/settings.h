#pragma once

#include <stdbool.h>

#include "esp_err.h"

// User settings. menuconfig gives the defaults; anything saved from the setup
// portal or the configuration page is kept in NVS and wins. They are read
// once at boot, and a change takes effect on the restart that follows saving.

#define SETTINGS_RANGE_COUNT 5 // 5, 10, 25, 50, 100 NM

typedef struct {
    char ssid[33];      // empty: no network yet, so open the setup portal
    char password[65];
    double lat, lon;    // radar centre
    int range_idx;      // starting range, 0..SETTINGS_RANGE_COUNT-1
    int feed;           // primary feed: 0 adsb.lol, 1 adsb.fi
    int fetch_s;        // seconds between feed requests, 2..60
    bool show_ground;
    bool sweep;
    int max_labels;     // 0..200
    bool batt_auto_off;
    int batt_shutdown_mv;
    int rotation;       // degrees, 0, 90 or 270; only the Tab5 can rotate
    char openaip_key[65]; // empty: no airspace or airfields
    char openaip_countries[12]; // ISO codes, "FR,CH"; empty: the nearest airfield's
    bool show_airspace;
    bool show_airfields;
} settings_t;

// Read NVS over the defaults. Call after nvs_flash_init().
void settings_load(void);

// The settings this boot is running with.
const settings_t *settings(void);

// The menuconfig defaults.
void settings_defaults(settings_t *out);

// Every value in range.
bool settings_valid(const settings_t *s);

// Store every field; ESP_ERR_INVALID_ARG, with nothing written, unless
// settings_valid(). It writes flash, so not from a task with its stack in
// PSRAM: the cache, and with it PSRAM, is off during the write.
esp_err_t settings_save(const settings_t *s);

// Forget everything saved, so the next boot uses the defaults again.
esp_err_t settings_erase(void);
