#pragma once

#include <stdbool.h>

#include "esp_err.h"

// User settings. The firmware gives the defaults; anything saved from the
// setup portal, the configuration page or the device's settings screen is
// kept in NVS and wins. Most take effect at once (settings_change()); a
// different network or orientation needs a restart.

#define SETTINGS_RANGE_COUNT 5 // 5, 10, 25, 50, 100 NM

typedef struct {
    char ssid[33];      // empty: no network yet, so open the setup portal
    char password[65];
    double lat, lon;    // radar centre
    bool centre_set;    // chosen, or found from the IP address; until then it is looked up
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
    int brightness;     // backlight percent, 5..100
    int dim_mode;       // 0 never, 1 sunset to sunrise, 2 from dim_from to dim_to
    int dim_brightness; // backlight percent while dimmed, 1..100
    int dim_from, dim_to; // minutes after local midnight, 0..1439
    char tz[48];        // POSIX TZ, "GMT0BST,M3.5.0/1,M10.5.0"; empty: UTC
    int clock;          // 0 UTC, 1 local, 2 both
    int tag_fields;     // TAG_* bits, at least one
    int trail_s;        // trail length in seconds, 0 (off)..600
    bool show_military;
    bool show_route;    // origin and destination in the card, from adsb.im
    bool alert;         // flag aircraft within alert_nm10 and below alert_ft
    int alert_nm10;     // tenths of a NM, 2..100
    int alert_ft;       // 500..20000
    bool alert_sound;   // on boards with a speaker
    bool show_vector;   // a line ahead of each aircraft, where it will be in a minute
    int filter_min_ft;  // hide airborne aircraft below this; 0 shows all
    int filter_max_ft;  // and above this; FILTER_NO_CEILING shows all
    int show_kinds;     // AC_KIND_* bits shown, at least one; emergencies always are
    bool emerg_alert;   // banner and chime for an emergency squawk anywhere in range
    bool show_centrelines; // extended runway centrelines at the major airfields
    bool sleep;         // backlight off from sleep_from to sleep_to, waking on a touch
    int sleep_from, sleep_to; // minutes after local midnight, 0..1439
    char adb_key[65];   // AeroDataBox, direct; empty: no flight times or aircraft details
    int adb_daily;      // AeroDataBox credits to spend a day at most, ADB_DAILY_MIN..ADB_DAILY_MAX
} settings_t;

#define ADB_DAILY_MIN 10
#define ADB_DAILY_MAX 5000

#define FILTER_NO_CEILING 60000

// What a data tag shows. Callsign and type share the first line, altitude and
// speed the second.
#define TAG_CALLSIGN 1
#define TAG_ALT 2
#define TAG_SPEED 4
#define TAG_TYPE 8
#define TAG_ALL 15

// Read NVS over the defaults. Call after nvs_flash_init().
void settings_load(void);

// The settings this boot is running with. Fine for reading a field or two;
// to copy them whole, as a change starts from, use settings_copy().
const settings_t *settings(void);

// A copy of the running settings, taken under the display lock the settings
// task swaps them under, so it can't be half the old and half the new.
void settings_copy(settings_t *out);

// The menuconfig defaults.
void settings_defaults(settings_t *out);

// Every value in range.
bool settings_valid(const settings_t *s);

// Store every field; ESP_ERR_INVALID_ARG, with nothing written, unless
// settings_valid(). It writes flash, so not from a task with its stack in
// PSRAM: the cache, and with it PSRAM, is off during the write. For just
// before a restart: nothing is stored after it, not even settings_change().
esp_err_t settings_save(const settings_t *s);

// Forget everything saved, so the next boot uses the defaults again. Like
// settings_save(), the last write before a restart.
esp_err_t settings_erase(void);

// Whether going from the running settings to s needs a restart: the network
// and the display's orientation are only set up at boot.
bool settings_need_restart(const settings_t *s);

// Called after a change has been stored and made the running settings, from
// the settings task, without the display lock. old is what ran before.
typedef void (*settings_listener_t)(const settings_t *old, const settings_t *now);

// Start the task that stores and applies changes. After settings_load().
void settings_start(settings_listener_t listener);

// Store s and apply it, or with settings_need_restart(s) store it and
// restart. ESP_ERR_INVALID_ARG unless settings_valid(). A newer change
// waiting replaces an older one. With wait, returns once it has been applied
// (up to a couple of seconds). Safe from any task, display lock or not
// unless waiting.
esp_err_t settings_change(const settings_t *s, bool wait);

// A network already joined (Improv over USB): store it and make it the
// running one, through the same task, without the restart a new network
// otherwise means. Waits until done.
esp_err_t settings_change_network(const char *ssid, const char *password);
