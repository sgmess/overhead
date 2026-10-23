# Overhead

<p align="center">
  <a href="https://sgmess.github.io/overhead/"><img alt="Flash in Browser" src="https://img.shields.io/badge/Flash%20in%20Browser-FF6D00?logo=googlechrome&logoColor=white"></a>
  <img alt="Boards" src="https://img.shields.io/badge/Boards-ESP32--P4%20%C2%B7%20M5Stack--Tab5-E7352C?logo=espressif&logoColor=white">
  <a href="https://github.com/sgmess/overhead/releases"><img alt="GitHub Release" src="https://img.shields.io/github/v/release/sgmess/overhead?color=7B42BC"></a>
  <a href="LICENSE"><img alt="GitHub License" src="https://img.shields.io/github/license/sgmess/overhead"></a>
  <img alt="GitHub top language" src="https://img.shields.io/github/languages/top/sgmess/overhead">
</p>

A small, single-screen ADS-B radar for ESP32-P4 touch displays. It joins
WiFi, polls a free online ADS-B feed, and plots the aircraft around a fixed
point on a radar scope.

| Board | `OVERHEAD_BOARD` | Panel |
| --- | --- | --- |
| ESP32-P4-WIFI6-Touch-LCD-3.4C | `p4_34c` (default) | 800x800 MIPI DSI, WiFi via ESP32-C6 |
| M5Stack Tab5 | `tab5` | 720x1280 MIPI DSI portrait, WiFi via ESP32-C6 |

- Aircraft drawn as arrowheads along their track, coloured by altitude (amber
  low, lime, cyan, blue, violet high), with an optional line to where each
  will be in a minute.
- Radar-style data tags: callsign, altitude in hundreds of feet, and a climb or
  descent arrow, with type and ground speed as options. The nearest aircraft
  get labelled first.
- Trails behind the nearest 50 aircraft, fading with age (1 to 10 minutes).
- Helicopters drawn with a rotor and tail boom; military aircraft (the feed's
  `dbFlags`) in a pink diamond.
- Origin and destination in the aircraft card, from
  [adsb.im](https://adsb.im)'s route database (the one tar1090 uses), with
  the flight number, the airline's name and logo, and the flag of the
  country the aircraft is registered in, all worked out on the device; how
  far along its route it is ("LHR > JFK • 23%"); its registered owner,
  when that isn't the airline, and for US registrations the year it was
  built; and in the big view a photo from [planespotters.net](https://www.planespotters.net).
- With an [AeroDataBox](https://aerodatabox.com) key (optional): for the
  aircraft you pick, when its flight left and gets in and how late it is
  ("Dep 14:23  Arr 00:24 • on time"), the flight number it's sold under,
  today's route, the terminal and gate it arrives at, and the aircraft's
  full type, age and seats ("Airbus A380-800", "built 2019, 7 years old •
  516 seats").
- An overhead alert: an aircraft within a set distance and below a set
  altitude blinks amber, a banner names it, and the Tab5 or 3.4C chimes.
- An emergency alert: any aircraft in range squawking 7700, 7600 or 7500, or
  reporting an emergency over ADS-B (medical, low fuel), gets a red banner
  ("7700 EMERGENCY BAW123 12.3 NM") ahead of any overhead alert, and a chime.
- A watchlist: types, registrations, callsigns or addresses ("A388, G-XLEA,
  RRR*"), and if wanted every military aircraft or any type never seen here
  before. One in range gets a violet ring and banner, and a chime.
- Where to look: the card and the big view give the compass point and the
  height above the horizon ("13.8 NM SE ▲5°"), allowing for the Earth's curve;
  a setting shows the bearing in both instead, as before.
- The weather at the airfield nearest the centre, or one chosen, from its
  METAR (wind, temperature, pressure), and the runway in use there, from
  which way the traffic low over it is landing and taking off.
- A filter by kind of aircraft (airliners, light aircraft, helicopters,
  other, from the transponder's emitter category) and altitude band. The
  count then reads "12 of 30 aircraft"; emergencies always show.
- Brightness, with dimming from sunset to sunrise at the radar centre or
  between set times, and the screen fully off overnight if wanted: a touch
  wakes it (and does nothing else), and it goes off again two minutes later.
- Traffic stats on the configuration page: today's aircraft seen, military,
  the busiest hour, the highest and fastest, the top airlines and types (by
  name, "Boeing 787-8 Dreamliner"), the rarest types here and a bar chart by
  hour; every aircraft that passed
  overhead, at its closest, for today, the last week or month or all kept
  (about 16,000), with a CSV download; and over the days, the average day of
  the last week, the most in a day and the running total, kept through restarts.
- Positions are moved forward between polls using ground speed and track, so
  traffic moves smoothly instead of jumping every few seconds.
- Emergency squawks (7500/7600/7700) are drawn red and circled.
- Extended runway centrelines at the airports, a dash a mile out to 9.5 NM,
  where arrivals line up and departures climb out.
- UTC or local clock (or both), a sweep turning every 8 s, compass rose,
  range rings.
  On the Tab5 the clock, range, traffic count and battery sit in the corners
  around the scope; the round 3.4C has no corners, so it keeps them along
  the top and bottom of the scope.
- Battery level, charging state and time left on the Tab5, with charging
  enabled and a clean power-off before the pack runs flat.

Data comes from [adsb.fi](https://adsb.fi), with an automatic switch to
[adsb.lol](https://adsb.lol) if a request fails; either can be made the
primary in the settings. Neither needs an API key.

## Touch

At power-up a start-up screen shows the radar in miniature, what start-up is
doing (joining WiFi, then receiving traffic) and the firmware version, with an
arc counting down 10 s. A tap dismisses it sooner, and does nothing else. It
gives way at once to the WiFi setup portal.

| Tap | Does |
| --- | --- |
| An aircraft | Opens its card: flag, flight number, airline and logo, registration, type, route, altitude, V/S, speed, track, squawk, range and bearing |
| The alert banner | Opens the card of the nearest aircraft that set it off |
| The card | Opens that aircraft in the big view |
| Empty scope, card open | Closes the card |
| Swipe up or down on the scope | Up or down a range: 5, 10, 25, 50, 100 NM |
| A list row (Tab5) | Selects that aircraft (in the big view too, with the big view up); tap it again to clear |
| Long press on the scope | Opens the settings screen |
| Swipe left or right | The next view, or the one before: scope, big view, and on the round 3.4C a list |
| The big view | Selects the next nearest aircraft, back round to the nearest |
| A row of the 3.4C's list | Selects that aircraft and opens it in the big view |

The big view shows one aircraft in large type, for across the room: flag and
logo, callsign, flight number and airline, altitude in its scope colour with
which way it is going, speed with an arrow along its track (north up, as on
the scope), then distance, bearing and squawk, the route with the airports'
towns ("SDF > CGN", "Louisville > Cologne") and how far along it is,
registration and type, the owner and age, and a photo. The card
only shows with the scope, and whatever the big view or a list picks stays
selected there when you swipe back. An aircraft that has just started an
emergency or come overhead takes the big view over until it stops or a tap
moves on; otherwise it shows the selection, or with none the nearest airborne
(not counting aircraft sitting on the ground without saying so: no altitude
and under 50 kt). The nearest stays shown until another is half a mile
nearer, so a near tie doesn't flicker. On the Tab5 it fills
the scope's square and the list stays beside it. Its route is looked up as
the card's is, once per callsign.

The round 3.4C has no room for the Tab5's list beside the scope, so
it gets it as a third view: the nearest 10 in the middle of
the circle, with the same columns, colours and count.

## Build and flash

Needs ESP-IDF 6.1.0 exactly for the Tab5: it builds a patched copy of IDF's
PPA driver, which stops the build on any other version until it is checked
against it (`boards/tab5/esp_driver_ppa/README.md`). Each board has its own build directory, `sdkconfig.<board>`
and `dependencies.lock.<board>`, so building one never changes the other's
settings or component versions. `managed_components/` is shared, though, so
the first build after switching boards re-fetches the components it needs.
There is nothing to configure first: WiFi and every radar setting are set on
the device (see [Setup and settings](#setup-and-settings)).

```bash
. ~/esp/esp-idf/export.sh

# P4 3.4C
idf.py build flash monitor

# M5Stack Tab5
idf.py -B build_tab5 -D OVERHEAD_BOARD=tab5 build flash monitor
```

The logic that is easiest to get wrong in a rare case (AeroDataBox's
credits and flight cache, feed numbers, trails across boot and the 180th
meridian, the watchlist, METARs and the runway in use, the overhead log in
flash) also has host tests: built for the PC with stand-ins for ESP-IDF
and the address and undefined-behaviour sanitizers on. They need only gcc,
and CI runs them before a release:

```bash
tests/host/run.sh
```

## Releases, web flasher and updates

`.github/workflows/firmware.yml` builds the firmware on every push to main.
A version tag publishes it:

```bash
git tag v0.2.0
```

```bash
git push origin v0.2.0
```

That makes a GitHub release holding, for each board,
`overhead-<board>-full.bin` (the whole flash image, at `0x0`) and
`overhead-<board>-ota.bin` (the app alone). It also deploys the web flasher
(`web/flash/`, using [ESP Web Tools](https://esphome.github.io/esp-web-tools/))
to GitHub Pages at https://sgmess.github.io/overhead/, serving that release.
ESP Web Tools is built from a pinned commit in that job: its latest release
bundles esptool-js 0.6, which can't flash a P4 of revision v3.1 or later,
as the 3.4C's is (its flash starts powered off). The update to esptool-js
0.7.0 is merged but not yet released.
Release builds start from the `sdkconfig.defaults*` files, never a local
`sdkconfig`, so they contain no WiFi network and open the setup portal on
first boot.

The device also checks by itself, a minute after joining WiFi and then
daily, and shows the result first in the status on the configuration page
and the settings screen ("v0.2.6, up to date", "v0.2.6, v0.2.7 available").
It never installs on its own.

Updates install over WiFi from the configuration page: *Firmware > Check
for updates* reads the latest release of `sgmess/overhead` (the one option
left in menuconfig, `OVERHEAD_OTA_REPO`, for forks) and installs `<project>-ota.bin`. The project name
includes the board (`overhead-tab5`), and an image whose name doesn't match
the running one is refused before anything is written. Flash has two app
slots (`partitions.csv`), and rollback is on. A new image is only kept once
it reaches the network. If it restarts before then, the bootloader goes back
to the previous one.

Moving from the old single-app layout needs one USB flash
(`idf.py -B build_tab5 flash`). The NVS partition stays where it was, so
saved settings survive.

## Setup and settings

With no network configured, the first boot opens a setup portal: an open
access point named `Overhead-XXXX` (the last four hex digits of the MAC).
The radar shows its name and a QR code that joins it. Phones then open the
setup page by themselves (DNS answers every name with the device, and DHCP
option 114 names the page); otherwise browse to `http://192.168.4.1`. The page
lists nearby networks and takes the password and the radar centre. Saving
restarts the device onto that network. If a saved network hasn't connected
30 s after boot, the portal opens as well. The saved network is retried every
15 s meanwhile, and the portal closes once it connects.

Once connected, the configuration page is at `http://overhead.local` (mDNS),
or at the address shown on the radar until the first traffic arrives. It
covers the centre (with a map), starting range, labels and what the data tags
show, trails, sweep, ground traffic, military highlighting, routes, the
filter, the overhead and emergency alerts, map layers, brightness and night dimming, the clock and timezone, primary
feed, poll interval, the Tab5's orientation and battery cut-off, and changing
or forgetting the network. Saving applies at once; only a new orientation or
network restarts the device. The pages are unauthenticated, so anyone on the
same network can change the settings.

A long press on the scope opens the same settings on the device itself,
with the status (network, address, uptime, memory, battery, map layers) at
the top and firmware updates (check, then install) at the bottom:
everything that needs no typing (so not the centre, the WiFi password, the
OpenAIP key or a typed-in timezone, which the screen sends you to the page
for). Each change applies as it is made, and the screen closes after a minute
untouched. Both save through one task (`settings_change()` in
`main/device/settings.c`) with its stack in internal RAM, which stores the change in
NVS, makes it the running settings under the display lock, then applies what
isn't read afresh: the timezone, the centre (the radar re-projects and the
airspace is fetched again), the sweep, trails, starting range, feed and the
OpenAIP layers.

### Flight details

With an [AeroDataBox](https://aerodatabox.com) API key entered on the
configuration page, the aircraft you tap gets its flight's times and the
aircraft's details. The key is for AeroDataBox's own direct API
(`api.aerodatabox.com`, header `X-Api-Key`), not RapidAPI or API.Market,
and can run on the credits AeroDataBox gives for feeding it ADS-B.

Every answer costs credits, so only the aircraft someone picked is looked
up, never the lists or the big view stepping to the nearest:

- The flight: today's flights of that aircraft (by ICAO address), 2
  credits. That finds the flight once AeroDataBox has seen it flying. If
  none of them is about now, 2 more for today's flights by the flight
  number the callsign is (AAL91 is AA91), which the schedules are filed
  under, or for a callsign that isn't one (BAW21V), by the callsign, which
  AeroDataBox only links to a flight once it has seen it. With no date it
  answers for the nearest day with one, which can be yesterday's, so the
  date is always given. Of what comes back, only flights within three hours
  of now count; of those, the one in the air that fits where the aircraft
  is wins. An answer is kept for that aircraft and that day only, since a
  callsign is flown again the next day, often by another aircraft. Its
  times are asked for again after 15 minutes if it is still being looked
  at, or 3 hours once it has landed. "Departed" comes from the flight's
  status, not its runway time, which can be an estimate.
- The aircraft, 1 credit, kept until a restart.

The configuration page sets the most to spend a day (100 credits unless
changed) and shows what has been spent. The count is stored in flash (NVS,
its own namespace), so restarts, power cuts and a reset to the defaults
don't start the day again. Each request's cost is stored before it is
sent; if it can't be stored, nothing is asked. It is given back only when
the request can't have been charged: no connection could be made, or the
answer refused it (401, 403, 429). An answer cut off part way, a timeout
once sent or a server error may have been charged, so those stay counted. One HTTPS connection serves a tap's requests, kept until 10 s pass
without one, which saves about 1.2 s on each after the first (see
[docs/PERFORMANCE.md](docs/PERFORMANCE.md)). Nothing is looked up before the clock is set, a
refused key isn't tried again until another is entered, and "too many
requests" holds off for 10 minutes. Without a key, adsb.im's routes and the
feed's details are shown as before.

### Weather

`main/sources/weather.c` fetches the METAR from
[aviationweather.gov](https://aviationweather.gov)'s API every 15 minutes,
and at once when the station or the centre changes: the station set on the
configuration page, or else the one nearest the centre among those within
about 24 NM, then 72 NM. The Tab5 shows it in the radar's bottom-left
corner, the 3.4C under the title of its list view, and the page shows the
METAR itself. The runway in use comes from the traffic: aircraft within
6 NM of the station and below 3,000 ft above it, at 80 to 250 kt, climbing
or descending at 300 fpm or more, one sample each over the last 20 minutes.
When at least two agree closely, their mean track gives the runway number.
It is a true track, where runways are named by magnetic heading, so it can
be one out where the variation is large.

### Airspace and airfields

With an [OpenAIP](https://www.openaip.net) API key entered on the
configuration page, the scope also shows airspace outlines and airfields
within 107 NM of the centre, under the traffic. The key is free: create one
on openaip.net under your profile's API clients. The airspace shown is CTR,
ATZ and MATZ (blue), TMA and CTA (darker blue), restricted and prohibited
areas (red), danger areas (orange), and RMZ and TMZ (purple). FIRs, airways
and sectors are left out. Airports are always shown, labelled at 50 NM and
below. Small airfields, glider and microlight sites appear at 25 NM and
below, labelled at 10 NM. Each is drawn as a ring crossed by its main runway.
At 50 NM and below, each airport's main runway also gets an extended
centreline both ways, a dash a mile from 1 to 9.5 NM. OpenAIP gives the
runway's heading but not its thresholds, so the line runs through the
airport's reference point: at Heathrow, between the two parallel runways.
Either layer can be turned off.

The data is fetched a country at a time and trimmed to the range, because
OpenAIP's area queries time out (HTTP 408) unless they fill a whole page.
Measured 2026-10-07: GB is 800 airspaces and 415 airfields in 6 pages, which
takes about 75 s with the 10 s gaps its rate limiter needs. It runs in its
own task, so the traffic keeps updating, and again daily. The country is the
nearest airfield's unless the page lists some (for a centre near a border,
say `FR,CH`).

So that this minute and a half isn't spent at every boot, each fetched map
is kept in the `aipcache` partition (185 KB for GB at 107 NM) and drawn from
there 4.6 s after power-on, before WiFi is even up. It is fetched again in
the background once it is a day old, or at once when the centre or the
countries change. The partition was added after release, in flash the app
slots leave free; an update over WiFi can't add a partition, so a device
only ever updated that way fetches at every boot as before, until one USB
flash of the partition table (`idf.py -B build_tab5 partition-table-flash`,
which keeps the saved settings). The web flasher's full image has it.

Everything under the traffic (disc, rings, compass rose, airspace and
airfields) is drawn once per range into a PSRAM image and copied each frame.
Drawing about 4,000 outline segments every second took a frame from about
100 ms to 400 ms at 100 NM; with the image, frames are 25–65 ms.
Nothing of the radar is redrawn while the settings screen covers it or the
screen is off overnight, and labels and colours are only set when they
change, since LVGL redraws an object whenever either is set. On the Tab5 the
radar keeps the display busy about 12–15% of the time with the sweep off
and about 25–29% with it on (a turn every 8 s, drawn in four segments);
with the settings screen open, next to none.

`/api/debug/bench?what=` (debug builds: `idf.py -B build_tab5_debug -D
OVERHEAD_BOARD=tab5 -D OVERHEAD_DEBUG=1 build`, which CI builds too) times the display for five seconds: `settings` scrolls the
settings screen, `scope` redraws the whole scope every frame, `radar` leaves
it as it is, `big` the big view, and `covered` holds the settings screen
still over it. It gives frames a second, the average and longest frame, the
share of time the display was busy and how much of that went on handing
frames to the panel, the pixels drawn, and internal RAM free now and at its
lowest since boot; `tools/bench.sh` runs every mode.
[docs/PERFORMANCE.md](docs/PERFORMANCE.md) records every display and memory
experiment so far, with the results and why each was kept or dropped.

### Clock, dimming and alerts

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

The watchlist (`main/core/watch.c`) matches each entry against an
aircraft's type, registration (with or without its dash), callsign and
address, in any case; a trailing `*` matches the start. It can also take
every military aircraft, and any type first seen here today: the statistics
keep every type ever seen, with the days it was seen on, and a type only
counts as new once that list is three days old, so a new device doesn't
alert for everything. A watched aircraft shows whatever the display filter
says, as an emergency does; its banner comes after an emergency's and an
overhead alert's, since it can stay in range for many minutes.

With "Screen off overnight", the backlight goes off between the set times
once nobody has touched the screen for two minutes (LVGL's own idle time).
While it is off, a transparent layer over everything takes the waking touch,
so it can't also select an aircraft or step the range. Alerts neither wake
it nor chime meanwhile. The configuration page's sliders still preview.

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

The overhead list (`main/core/passes.c`) uses the overhead alert's distance
and ceiling, whether the alert is on or not. Each pass is written to flash
as it ends, a 32-byte record (CRC-16 checked) in a ring of 4 KB sectors, the
oldest sector erased as the ring comes round to it, and those overhead as
the device restarts are written then; at boot the newest record says where
it got to, and today's are read back. It has a `passes` partition of its
own (512 KB, 16,384 passes), added in v1.0.11 after `stats` without moving
anything, so only on devices flashed over USB or from the web flasher since.
A device updated over WiFi keeps it in the end of `aipcache` instead
(384 KB, 12,288 passes), which the OpenAIP map, at most 640 KB, leaves
free; without either, today's are in RAM alone. The page shows today's, the
last 7 or 30 days' or all of them (the newest 3,000), and
`/api/passes.csv` downloads every one, oldest first, then those overhead
now as far as they have got (a pass ends a minute after the aircraft leaves
the circle, and is only logged then).

The chime is the Tab5's ES8388 and speaker, or
the 3.4C's ES8311 and a speaker on its header, opened for each chime so the
amplifier is off between them.

Routes are looked up only for the aircraft whose card is open, one request
per callsign per session, and sent with its position so adsb.im can say
whether the route fits (a "?" after it when not). How far along it is
comes from the airports' positions in the same answer: the share of the leg
it is on that it has flown, great circle, and in the big view the miles to
go.

The owner is the feed's `ownOp`, which only US registrations have (from the
FAA's register, with the `year` it was built), or else
[adsbdb.com](https://www.adsbdb.com)'s registered owner, looked up by ICAO
address for the aircraft on show. The photo in the big view is the first
of planespotters.net's photos of the registration, credited to its
photographer as their terms ask, fetched once and scaled to 150 pixels high.
planespotters.net serves progressive JPEGs, which neither the P4's hardware
decoder nor LVGL's TJPGD can read, so `main/third_party/stb_image.h` decodes
them. Both are looked up one aircraft at a time and the last 16 are kept.

The flight number is shown only for a callsign of an airline's code and
digits, BAW189 as BA189, which is usually the flight number; alphanumeric
callsigns (BAW22B, EZY45GT) are assigned to avoid confusion on the radio and
aren't, so they get none. The airline comes from the callsign's first three letters (`main/core/airlines.c`, the
carriers most often seen over Europe and the large ones elsewhere), and the
country from the block of ICAO addresses each state is allocated
(`main/core/countries.c`); an aircraft that isn't an airliner gets its country's
name in place of the airline. The flags are Twemoji's, halved to 36x26 and
kept in `main/ui/flags.bin`, which `tools/make_flags.py` rebuilds after a
country is added. The airline logos are pics.avs.io's, drawn on white,
trimmed and fitted to 138x40, and shown on a white badge because many are
dark and would vanish on the card; `tools/make_logos.py` rebuilds
`main/ui/logos.bin` after an airline is added (about 1 MB for 133).

Settings are kept in NVS (namespace `overhead`). Until something is saved
the firmware's defaults apply (`settings_defaults()` in `main/device/settings.c`):
no network, so the setup portal opens, and the centre on Heathrow. Once
connected, a device whose centre has never been set asks where its internet
connection is (ipinfo.io, then ipwho.is) and centres there, usually within a
few km; the timezone comes from the same answer if none was chosen. That
happens once: it never moves a centre set on the setup or configuration page.
"Reset all settings to defaults" on the page erases the saved ones, network
included, so the next connection looks the centre up again.

The web server's task stack is in PSRAM. PSRAM is unreachable while flash is
written, so a new network or a reset is written to NVS from the esp_timer
task just before the restart, not from the request handler. Other changes go
through `settings_change()`, as above. The device's settings screen sends only
the settings changed on it, so a change made on the page while it is open is
kept.

### Tab5 notes

Landscape by default: the scope on the left, and the nearest aircraft listed
on the right (15 rows), with the details card above the list while something
is selected. Portrait, the panel's native orientation, puts the scope in the
top 720x720 and the list below it; it is set on the configuration page or the
settings screen.
The display goes through `esp_lvgl_adapter`, as on the 3.4C, not the BSP's
`esp_lvgl_port`: the board code creates the panel and touch with the BSP and
registers them with the adapter. It draws into one internal buffer and, in
landscape, the P4's PPA rotates that into one of three PSRAM frame buffers
while another is on the panel. Scrolling the settings screen
(`/api/debug/bench`, debug builds) runs at 20.8 fps in landscape against
esp_lvgl_port's 12, which rotated 24-line strips one at a time; portrait
went from 27 to 20.5, and about 40 KB more internal RAM is free. LVGL draws
with one thread on the Tab5, not two: two scrolled at 16.8 fps. LVGL's PPA
renderer was slower than drawing in software. The BSP is the variant without
its own LVGL layer (`m5stack_tab5_noglib`).
The P4's PPA, which does the rotation, can hang for good on some rotations
(a hardware bug), freezing the screen; IDF 6.1's workaround didn't cover the
Tab5's flushes. `boards/tab5/esp_driver_ppa/` is IDF's PPA driver with the
fix esp_lvgl_adapter ships as an IDF patch, and replaces IDF's own in the
Tab5 build. As a safety net on both boards, `main/device/display_watch.c`
restarts the device if the display stays frozen for 15 s, and `/api/state`
counts those restarts since power-on (`display_restarts`). The adapter
draws rotated but passes touches on as the panel reports them, portrait, so
the board code sets the touch driver's swap and mirror to match. The BSP (`espressif/m5stack_tab5_noglib`) detects the
three panel revisions at boot by which touch controller answers on I2C. The
board code releases the panel and touch resets and waits for that controller
first, because without it detection failed on a revision-3 (ST7121) unit. The
Tab5's P4 is a rev v1.x chip, so its build uses the pre-v3 settings. The
ESP32-C6's power is switched through an I/O expander before WiFi starts.

**Internal RAM at boot.** esp_hosted sets up its SDIO buffers in a
constructor, before the scheduler starts, while only about 150 KB of internal
RAM is in the heap. Its default 20-deep queues took 125 KB of that. v0.2.0
carried 10 KB more static data than v0.1.1 and boot-looped: the timer task's
2 KB stack no longer fitted. The queues are now 12 deep, as on the 3.4C and
in traffic-display on this Tab5, which takes about 99 KB. Big buffers live in
PSRAM, and the workflow fails a Tab5 build whose static D/IRAM passes
140,000 bytes (132,880 at v0.2.4; 302 KB of internal RAM free at `app_main`).
esp_hosted's buffers can't go to PSRAM (`ESP_HOSTED_MEMPOOL_PREFER_SPIRAM`):
the first small transmit after joining failed every boot and restarted it.

LVGL's heap is in PSRAM too (`main/ui/lvgl_mem.c`, `LV_USE_CUSTOM_MALLOC`).
With the C library's malloc, everything LVGL allocates of 1 KB or less went
to internal RAM: the radar's objects took about 48 KB of it, and opening the
settings screen another 36 KB in small pieces, after which esp_hosted
couldn't get a 3.5 KB receive buffer and the network stopped (v0.2.5). Then
about 120 KB of internal RAM stayed free while running, with the settings
screen open or not; since the display adapter and one drawing thread
(v1.0.2, v1.0.3), about 175 KB.

**Code runs from PSRAM** (`SPIRAM_XIP_FROM_PSRAM`), as traffic-display found
it must on the P4. Otherwise IDF disables the cache for every flash write and
erase, both cores lose PSRAM and the frame buffer with it, and the panel
flashed through an update and at every settings save. It costs about 10 KB
of static internal RAM. Both P4 boards have it; the 3.4C got it in October
2026, after reading the saved OpenAIP map from a task with its stack in
PSRAM boot-looped it.

Updates over WiFi write each 4 KB chunk to flash from an internal buffer.
From PSRAM, IDF copies it 32 bytes at a time with the caches paused for each,
which starved the C6's link until the download died (v0.1.1's updater). A
2 MB update now takes about 12 s.

**ESP-IDF 6.1.** Moved from 5.5.5 on 12 October 2026. What it took:
cJSON from the registry (`espressif/cjson`, IDF dropped its own); certificate
checks in software (`MBEDTLS_HARDWARE_ECDSA_VERIFY=n`), because 6.1's PSA
driver uses the ECDSA peripheral this rev v1.x P4 lacks and every
ECDSA-signed chain failed; and GlobalSign Root CA added back to the bundle
(`certs/extra_roots.pem`), which 6.1 dropped but adsb.fi and OpenAIP still
chain to. Static internal RAM fell 13 KB (FreeRTOS out of IRAM) and 317 KB
is free at `app_main`. The C library is now picolibc; the local clock's POSIX
timezone works with it.

**C6 firmware.** The host's esp_hosted major version must match the one on
the C6; the boot log prints the C6's (`net: co-processor esp_hosted
firmware ...`). This Tab5's C6 runs 2.12.6, so its build uses esp_hosted 2.12
and esp_wifi_remote 1.6. A 1.4 host against it asserts in `netif_add` when
connecting. Two Tab5 settings changed with 2.x. The SDIO pins come from
esp_hosted's own Tab5 board option, because 2.x ignores the pin options set
directly. The C6 reset is set to active high: 1.4 always drove that pin high
whatever the setting (a bug), and with "active low" honoured, 2.x holds the
C6 in reset. TLS buffers moved to PSRAM because 2.x uses more internal RAM.
The P4 3.4C made the same move in October 2026, when its C6 was updated to
2.x. Its pins are esp_hosted's defaults, so it needs no board option, and its
TLS buffers are in PSRAM too. Its Waveshare BSP pins esp_lvgl_adapter to
0.6, which includes LVGL 9.6's deprecated `src/` headers; their `#warning`
is an error under IDF 6.1, so the top-level CMakeLists keeps it a warning
in that component alone. The Tab5 uses the adapter's 0.7, which doesn't.

**Battery.** The Tab5 only charges while firmware holds the charger's enable
line (CHG_EN, on the second I/O expander) high, so the firmware turns it on at
boot, with QC fast charge as M5Stack's firmware does. An INA226 on the pack
(5 mOhm shunt) gives voltage and current. The current is negative while
charging, measured on this hardware. Level and charging state show in the
scope's top-right corner. The percentage is read off a Li-ion voltage curve,
so it's an estimate. Below 6.0 V the pack latches into a protection mode
and has to be refitted, so after 30 s under 6.3 V on battery the screen counts
down and the firmware powers off (configuration page, Battery). With no pack fitted the charger's output doesn't
read 0 V. It alternates about 11 s at 8.38 V and 5.5 s at 4.2 V (measured on
this unit; also noted in yejun/tab5-fancy-clock). So one reading outside
5.5 to 9.0 V means no pack, and a pack is only shown once it has stayed in
range for 20 s, longer than one whole cycle. The voltage is sampled every
500 ms so a dip can't be missed. The P4 3.4C has no battery.

## Layout

`main/` is split by what each part does; headers are included by name alone,
so a file can move between folders without its users changing.

| File | Purpose |
| --- | --- |
| `main/main.c` | Starts the display, WiFi and the fetch task |
| `main/aircraft.h` | An aircraft as the feed gives it |
| **`main/core/`** | **The logic, without the network or the screen** |
| `stats.c` | Today's counts and the days before, and every type seen, for the configuration page, saved to flash |
| `passes.c` | The aircraft overhead, each at its closest, logged in flash |
| `watch.c` | Which aircraft a watchlist matches |
| `types.c` | Aircraft types' names by ICAO designator, made by `tools/make_types.py` |
| `trails.c` | Position history per aircraft for the trails |
| `airlines.c`, `countries.c` | Airline and flight number by callsign; country by ICAO address |
| **`main/sources/`** | **What is fetched** |
| `feed.c` | HTTPS poll and readsb JSON parsing (buffers in PSRAM) |
| `route.c` | Origin and destination by callsign from adsb.im, cached, and how far along it is |
| `weather.c` | The METAR near the centre from aviationweather.gov, and the runway in use from the traffic |
| `details.c` | The registered owner from adsbdb.com and a photo from planespotters.net, by ICAO address |
| `aerodatabox.c` | Optional flight times and aircraft details from AeroDataBox, for the picked aircraft only, within a daily credit limit |
| `openaip.c` | OpenAIP airspace and airfields: fetch by country, trim to range, project to NM |
| `geoloc.c` | A new device's centre from its IP address |
| `zones.c` | Timezones and their POSIX rules, for the page and the screen |
| **`main/net/`** | **WiFi, the web pages, updates** |
| `net.c` | WiFi (via the ESP32-C6), setup portal, mDNS, SNTP |
| `dns_server.c`, `improv.c` | The portal's DNS, where every name answers with the device; WiFi setup from the web flasher |
| `web.c`, `web/` | Web server, JSON API, and the setup and configuration pages |
| `ota.c` | Updates from GitHub releases, image check, rollback confirmation |
| **`main/ui/`** | **The screen** |
| `radar_ui.c` | The radar screen's shared state, overlay (clock, status, alert banner), views and touch, and its API |
| `radar_priv.h` | What the radar screen's parts share: per-board sizes, colours, state and functions |
| `scope.c` | Aircraft projected onto the scope, airspace and airfields, trails, the drawing and the sweep |
| `card.c`, `big_view.c`, `lists.c` | The card, the big view, and the Tab5's list and the 3.4C's list view |
| `describe.c` | Text about an aircraft for all of those: route, airline, owner and age, altitude |
| `settings_ui.c` | The on-device settings screen |
| `splash.c` | The start-up screen |
| `backlight.c` | Brightness, night dimming by sun elevation or clock, sleep, the page's preview |
| `flags.c`, `logos.c`, `flags.bin`, `logos.bin` | Country flags and airline logos, built by `tools/` |
| `lvgl_mem.c` | LVGL's heap, in PSRAM |
| **`main/device/`** | **The device itself** |
| `settings.c` | Settings in NVS over the firmware defaults |
| `battery.c` | Battery state, charge estimate, time left, low-voltage power-off |
| `display_watch.c` | Restarts the device if the display stays frozen |
| `main/third_party/` | stb_image, the JPEG decoder for the photos |
| `tools/` | `make_flags.py` and `make_logos.py`, which build `main/ui/flags.bin` and `main/ui/logos.bin`; `make_types.py`, which builds `main/core/types.c`; `bench.sh`, which times the display |
| `tests/host/` | Host tests of the firmware's logic, with stand-ins for ESP-IDF (`run.sh`) |
| `docs/PERFORMANCE.md` | The benchmarks run, their results, and what was decided |
| `web/flash/`, `.github/workflows/` | Web flasher page and the build, release and Pages workflow |
| `boards/<board>/board/` | Per-board bring-up: display start, WiFi power, LVGL lock; the BSP is its dependency |
| `boards/tab5/esp_driver_ppa/` | IDF's PPA driver with the rotation-hang fix, for the Tab5 only |

The P4 3.4C and Tab5 BSPs come from the component registry
(`waveshare/esp32_p4_wifi6_touch_lcd_xc` 3.0.1, `espressif/m5stack_tab5_noglib` 1.3.2),
each fetched only for its own board.

## Licence

Apache License 2.0; see [LICENSE](LICENSE). `boards/tab5/esp_driver_ppa/`
is ESP-IDF's, Apache-2.0, changed as its README says. `main/third_party/stb_image.h` is Sean Barrett's,
public domain or MIT. The flag images in `main/ui/flags.bin` are from
[Twemoji](https://github.com/jdecked/twemoji), Copyright 2019 Twitter, Inc
and other contributors, licensed under
[CC-BY 4.0](https://creativecommons.org/licenses/by/4.0/); they were scaled
down for the screen. The airline logos in `main/ui/logos.bin` are trademarks of
their airlines and are not covered by this licence. The licence covers this code only: aircraft data from
adsb.fi and adsb.lol, routes from adsb.im, owners from adsbdb.com, photos
from planespotters.net (each its photographer's), flight and aircraft details
from AeroDataBox, weather from aviationweather.gov, aircraft type names from
ICAO Doc 8643 by way of [tar1090-db](https://github.com/wiedehopf/tar1090-db),
airspace from OpenAIP and the
IP-location answers from ipinfo.io and ipwho.is are under those services'
own terms.
