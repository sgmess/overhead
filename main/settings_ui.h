#pragma once

// The device's own settings screen: everything that needs no typing, each
// change applied as it is made. A long press on the scope opens it. Both
// functions with the display lock held.

// The dark theme its controls are drawn with. Once, before any widgets, so
// the rest of the screen looks the same before and after it first opens.
void settings_ui_init(void);

void settings_ui_open(void);
