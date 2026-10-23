#pragma once

#include "sdkconfig.h"

// The device's own settings screen: everything that needs no typing, each
// change applied as it is made. A long press on the scope opens it. Both
// functions with the display lock held.

// The dark theme its controls are drawn with. Once, before any widgets, so
// the rest of the screen looks the same before and after it first opens.
void settings_ui_init(void);

void settings_ui_open(void);

// Whether it is open, covering the radar. With the display lock held.
bool settings_ui_is_open(void);

#if CONFIG_LV_USE_SNAPSHOT
// For development: scroll the open settings screen up and down without a
// finger until stopped (main/ui/bench.h counts the frames)
void settings_ui_debug_scroll_start(void);
void settings_ui_debug_scroll_stop(void);
// Close it, as its Done button does
void settings_ui_debug_close(void);
// Scroll it to y pixels down, for a look at what is further down
void settings_ui_debug_scroll_to(int y);
#endif
