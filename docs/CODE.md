[← Overhead](../README.md)

# The code

What each source file does, how settings are stored and saved, and how the
radar is drawn.

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
| `docs/` | These documents: building, data sources, alerts and statistics, the Tab5, the code, and performance |
| `web/flash/`, `.github/workflows/` | Web flasher page and the build, release and Pages workflow |
| `boards/<board>/board/` | Per-board bring-up: display start, WiFi power, LVGL lock; the BSP is its dependency |
| `boards/tab5/esp_driver_ppa/` | IDF's PPA driver with the rotation-hang fix, for the Tab5 only |

The P4 3.4C and Tab5 BSPs come from the component registry
(`waveshare/esp32_p4_wifi6_touch_lcd_xc` 3.0.1, `espressif/m5stack_tab5_noglib` 1.3.2),
each fetched only for its own board.

## Settings

The page and the screen both save through one task (`settings_change()` in
`main/device/settings.c`) with its stack in internal RAM, which stores the
change in NVS, makes it the running settings under the display lock, then
applies what isn't read afresh: the timezone, the centre (the radar
re-projects and the airspace is fetched again), the sweep, trails, starting
range, feed and the OpenAIP layers.

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

## Drawing

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
