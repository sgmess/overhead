#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

// Firmware updates from the project's GitHub releases. Each release carries
// <project>-ota.bin, the app image for one board (the project name includes
// the board, e.g. overhead-tab5). Checking and installing run in a task of
// their own; the configuration page starts them and polls the status.

// A freshly installed image is only kept once this is called: the
// bootloader rolls back to the previous one if the device restarts first.
// Call once the app has shown it works (here, on reaching the network).
void ota_mark_valid(void);

// Look up the latest release. ESP_ERR_INVALID_STATE while busy.
esp_err_t ota_check(void);

// Download and install the release found by the last check, then restart.
// ESP_ERR_INVALID_STATE without a successful check, or while busy.
esp_err_t ota_install(void);

// An install is downloading or writing; other network work should wait.
bool ota_installing(void);

// Check by itself: once a minute after joining, then daily. It only looks;
// installing is still asked for.
void ota_start_auto_check(void);

// "v0.2.6, up to date", "v0.2.6, v0.2.7 available" and so on: the running
// version and where it stands, for the status lists.
void ota_summary(char *buf, size_t n);

typedef struct {
    const char *state;    // "idle", "checking", "checked", "installing", "error"
    const char *detail;   // what went wrong, for "error"
    const char *latest;   // the latest release's tag, once checked
    const char *published; // its date, YYYY-MM-DD
    const char *notes_url; // its page on GitHub
    bool has_image;       // it has an image for this board
    int progress;         // percent, while installing
    int checked_ago_s;    // since the last successful check; -1 before one
} ota_status_t;

void ota_get_status(ota_status_t *out);
