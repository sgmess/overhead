#pragma once

// Restarts the device if the display stays frozen. Start once the display
// is running.
void display_watch_start(void);

// How many times it has done that since the device was powered on.
int display_watch_restarts(void);
