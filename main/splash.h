#pragma once

// The start-up screen, over everything else at power-up: the emblem, the
// name, what it is doing (joining WiFi, waiting for traffic), the firmware
// version, and an arc counting down. A tap dismisses it, and is used up
// doing so; otherwise it goes after SPLASH_MS. With the display lock held.

void splash_show(void);

// What start-up is doing, under the title. Ignored once it has gone.
void splash_set_status(const char *text);

// Gone now: the setup portal's QR code has to be seen.
void splash_hide(void);
