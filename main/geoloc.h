#pragma once

#include <stdbool.h>

// Until a centre has been chosen, where the internet connection is: a city,
// a few km out, which on a 25 NM scope is close enough to start with. Also
// the timezone, when none has been chosen either.

// Look it up and apply it through settings_change(). True once nothing is
// left to do: found, or a centre has been chosen meanwhile. Connected, from a
// task with room for TLS, not holding the display lock.
bool geoloc_locate(void);
