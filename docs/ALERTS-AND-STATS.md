# Clock, dimming, alerts and statistics

The clock and night dimming, the screen off overnight, the overhead,
emergency and watchlist alerts and the chime, and the statistics and
overhead log on the configuration page.

## Clock and dimming

The timezone is a POSIX TZ string, because that is what newlib understands.
The page lists common zones, suggests the browser's own when none is saved,
and takes any other rule typed in (`GMT0BST,M3.5.0/1,M10.5.0`). Night
dimming either follows the sun's elevation at the radar centre (below
-0.833°, which is sunset and sunrise with refraction) or local clock times.
Moving a brightness slider on the page shows that level for 8 seconds without
saving. While an overhead or emergency alert is active the screen comes back
to full brightness. Each aircraft chimes once and again only after it has been
out of the alert for two minutes; overhead, emergency and watchlist alerts
keep separate lists, so one doesn't silence another.

## The watchlist

The watchlist (`main/core/watch.c`) matches each entry against an
aircraft's type, registration (with or without its dash), callsign and
address, in any case; a trailing `*` matches the start. It can also take
every military aircraft, and any type first seen here today: the statistics
keep every type ever seen, with the days it was seen on, and a type only
counts as new once that list is three days old, so a new device doesn't
alert for everything. A watched aircraft shows whatever the display filter
says, as an emergency does; its banner comes after an emergency's and an
overhead alert's, since it can stay in range for many minutes.

## Screen off overnight

With "Screen off overnight", the backlight goes off between the set times
once nobody has touched the screen for two minutes (LVGL's own idle time).
While it is off, a transparent layer over everything takes the waking touch,
so it can't also select an aircraft or step the range. Alerts neither wake
it nor chime meanwhile. The configuration page's sliders still preview.

## Statistics

The day's counts (`main/core/stats.c`) take each aircraft once by ICAO address,
in a table in PSRAM, if it comes within the radar's range; an hour's bar is
the aircraft seen in that hour. Altitudes over 60,000 ft and speeds over
750 kt are taken as glitches. An airline's count is its aircraft, by the
three letters their callsigns start with, once each when the callsign first
shows; airlines that fly as two codes (BAW and SHT) are added together on
the page. At local midnight the day goes into a history of the last 30, from
which the page shows the average of the last week's whole days (counted from
midnight to midnight, so a day the device was off for part of doesn't pull
it down), the most in a day and the running total. Every 10 minutes while
they change, at least hourly, and as the device restarts, the counts and the
history are saved to a `stats` partition (eight 32 KB slots written in turn,
to spread the wear), so a restart or an update carries on with the day; a
day that ended while the device was off is added to the history at the next
start. A device that has only ever been updated over WiFi has no such
partition and starts again at each restart. Since v1.0.11 each save also
holds today's aircraft by type and the book of every type seen (320 at
most), after the packed aircraft (save version 3); a version 2 save still
loads, without them.

## The overhead log

The overhead list (`main/core/passes.c`) uses the overhead alert's distance
and ceiling, whether the alert is on or not. Each pass is written to flash
as it ends, a 32-byte record (CRC-16 checked; registrations up to 8
characters) in a ring of 4 KB sectors, the oldest sector erased as the ring
comes round to it. Those overhead as the device restarts, or before the
low-battery power-off, are written then (as the statistics are saved). At
boot the newest record says where it got to. Reading it back looks at every
slot and sorts by time, because a pass is logged as it ends but timed at
its closest, and a power cut can leave a gap; a write that fails leaves its
slot behind and the pass waits, to be written with the next or within a
minute. It has a `passes` partition of its
own (512 KB, 16,384 passes), added in v1.0.11 after `stats` without moving
anything, so only on devices flashed over USB or from the web flasher since.
A device updated over WiFi keeps it in the end of `aipcache` instead
(384 KB, 12,288 passes), which the OpenAIP map, at most 640 KB, leaves
free; without either, today's are in RAM alone. The page shows today's, the
last 7 or 30 days' or all of them (the newest 3,000), and
`/api/passes.csv` downloads every one, oldest first, then those overhead
now as far as they have got (a pass ends a minute after the aircraft leaves
the circle, and is only logged then).

## The chime

The chime is the Tab5's ES8388 and speaker, or
the 3.4C's ES8311 and a speaker on its header, opened for each chime so the
amplifier is off between them.
