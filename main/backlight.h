#pragma once

#include <stdbool.h>

// Backlight level from the settings: the day brightness, or the night one
// while dimmed (sunset to sunrise at the radar centre, or between two local
// times).

// Set the level for now. Call about once a second; alert brings it up to the
// day level while something is overhead.
void backlight_update(bool alert);

// Show percent for a few seconds, for the configuration page's slider.
// Safe from any task.
void backlight_preview(int percent);

// Whether it is dimmed for the night right now.
bool backlight_night(void);
