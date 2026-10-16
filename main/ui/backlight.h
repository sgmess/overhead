#pragma once

#include <stdbool.h>

// Backlight level from the settings: the day brightness, or the night one
// while dimmed (sunset to sunrise at the radar centre, or between two local
// times), or off while asleep (between the sleep times, once nobody has
// touched the screen for a couple of minutes).

#include <stdint.h>

// Set the level for now. Call about once a second; alert brings it up to the
// day level while something is overhead, unless asleep. idle_ms is how long
// since the screen was last touched. Returns whether it is asleep.
bool backlight_update(bool alert, uint32_t idle_ms);

// Whether the screen is off for the night, as of the last update.
bool backlight_asleep(void);

// Show percent for a few seconds, for the configuration page's slider.
// Safe from any task.
void backlight_preview(int percent);

// Whether it is dimmed for the night right now.
bool backlight_night(void);
