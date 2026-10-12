# Overhead

A small, single-screen ADS-B radar for ESP32 touch displays. It joins
WiFi, polls a free online ADS-B feed, and plots the aircraft around a fixed
point on a radar scope.

| Board | `OVERHEAD_BOARD` | Panel |
| --- | --- | --- |
| ESP32-P4-WIFI6-Touch-LCD-3.4C | `p4_34c` (default) | 800x800 MIPI DSI, WiFi via ESP32-C6 |
| ESP32-S3-Touch-LCD-2.8C | `s3_28c` | 480x480 ST7701 RGB, native WiFi |
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
  [adsb.im](https://adsb.im)'s route database (the one tar1090 uses).
- An overhead alert: an aircraft within a set distance and below a set
  altitude blinks amber, a banner names it, and the Tab5 chimes.
- Brightness, with dimming from sunset to sunrise at the radar centre or
  between set times.
- Positions are moved forward between polls using ground speed and track, so
  traffic moves smoothly instead of jumping every few seconds.
- Emergency squawks (7500/7600/7700) are drawn red and circled.
- UTC or local clock (or both), rotating sweep, compass rose, range rings.
  On the Tab5 the clock, range, traffic count and battery sit in the corners
  around the scope; the round panels have no corners, so they keep them along
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
| An aircraft | Opens its card: registration, type, route, altitude, V/S, speed, track, squawk, range and bearing |
| The alert banner | Opens the card of the nearest aircraft that set it off |
| Empty scope, card open | Closes the card |
| Empty scope | Next range: 5, 10, 25, 50, 100 NM |
| A list row (Tab5) | Selects that aircraft; tap it again to clear |
| Long press on the scope | Opens the settings screen |

## Build and flash

Needs ESP-IDF 6.1 (the Tab5; the parked boards' lock files are still from
5.5). Each board has its own build directory, `sdkconfig.<board>`
and `dependencies.lock.<board>`, so building one never touches the other.
There is nothing to configure first: WiFi and every radar setting are set on
the device (see [Setup and settings](#setup-and-settings)).

```bash
. ~/esp/esp-idf/export.sh

# P4 3.4C
idf.py build flash monitor

# S3 2.8C
idf.py -B build_s3 -D OVERHEAD_BOARD=s3_28c build flash monitor

# M5Stack Tab5
idf.py -B build_tab5 -D OVERHEAD_BOARD=tab5 build flash monitor
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
Release builds start from the `sdkconfig.defaults*` files, never a local
`sdkconfig`, so they contain no WiFi network and open the setup portal on
first boot. Only the Tab5 is built while the other boards are parked.

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
overhead alert, brightness and night dimming, the clock and timezone, primary
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
`main/settings.c`) with its stack in internal RAM, which stores the change in
NVS, makes it the running settings under the display lock, then applies what
isn't read afresh: the timezone, the centre (the radar re-projects and the
airspace is fetched again), the sweep, trails, starting range, feed and the
OpenAIP layers.

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

### Clock, dimming and alerts

The timezone is a POSIX TZ string, because that is what newlib understands.
The page lists common zones, suggests the browser's own when none is saved,
and takes any other rule typed in (`GMT0BST,M3.5.0/1,M10.5.0`). Night
dimming either follows the sun's elevation at the radar centre (below
-0.833°, which is sunset and sunrise with refraction) or local clock times.
Moving a brightness slider on the page shows that level for 8 seconds without
saving. While an overhead alert is active the screen comes back to full
brightness. Each aircraft chimes once and again only after it has been out of
the alert for two minutes. The chime is the Tab5's ES8388 and speaker, opened
for each chime so the amplifier is off between them.

Routes are looked up only for the aircraft whose card is open, one request
per callsign per session, and sent with its position so adsb.im can say
whether the route fits (a "?" after it when not).

Settings are kept in NVS (namespace `overhead`). Until something is saved
the firmware's defaults apply (`settings_defaults()` in `main/settings.c`):
no network, so the setup portal opens, and the centre on Heathrow. Once
connected, a device whose centre has never been set asks where its internet
connection is (ipinfo.io, then ipwho.is) and centres there, usually within a
few km; the timezone comes from the same answer if none was chosen. That
happens once: it never moves a centre set on the setup or configuration page.
"Reset all settings to defaults" on the page erases the saved ones, network
included, so the next connection looks the centre up again.

The web server's task stack is in PSRAM. PSRAM is unreachable while flash is
written, so a save is written to NVS from the esp_timer task just before the
restart, not from the request handler.

### S3 2.8C notes

The panel has no frame memory: the S3 streams the frame out of PSRAM
continuously, and anything that starves that bus shifts or tears the picture.
The settings in `sdkconfig.defaults.s3_28c` come from traffic-display, where
they ran stable with WiFi up. This app also runs TLS, so mbedTLS is moved to
PSRAM to keep internal RAM free. It needs the Touch variant of the board: the
non-touch 2.8C aborts at boot because its GT911 never answers.

### Tab5 notes

Landscape by default: the scope on the left, and the nearest aircraft listed
on the right (15 rows), with the details card above the list while something
is selected. Portrait, the panel's native orientation, puts the scope in the
top 720x720 and the list below it; it is set on the configuration page or the
settings screen.
In landscape LVGL renders 1280x720 and the P4's PPA rotates each strip into
the portrait frame buffer, so the CPU never rotates pixels. Measured against
portrait: about 65 ms more per once-a-second redraw (mostly from the smaller
24-line strips, about 15 ms the rotation itself), and about 33 KB more
internal RAM free, because its three draw buffers are smaller than
portrait's two. The BSP (`espressif/m5stack_tab5`) detects the
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

LVGL's heap is in PSRAM too (`main/lvgl_mem.c`, `LV_USE_CUSTOM_MALLOC`).
With the C library's malloc, everything LVGL allocates of 1 KB or less went
to internal RAM: the radar's objects took about 48 KB of it, and opening the
settings screen another 36 KB in small pieces, after which esp_hosted
couldn't get a 3.5 KB receive buffer and the network stopped (v0.2.5). Now
about 120 KB of internal RAM stays free while running, with the settings
screen open or not.

**Code runs from PSRAM** (`SPIRAM_XIP_FROM_PSRAM`), as traffic-display found
it must on the P4. Otherwise IDF disables the cache for every flash write and
erase, both cores lose PSRAM and the frame buffer with it, and the panel
flashed through an update and at every settings save. It costs about 10 KB
of static internal RAM.

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
The P4 3.4C is still on 1.4; if its C6 is updated, it needs the same move.

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
500 ms so a dip can't be missed. The P4 3.4C has no battery. The 2.8C has a battery voltage divider whose ratio
nobody has measured, so it reports none.

## Layout

| File | Purpose |
| --- | --- |
| `main/main.c` | Starts the display, WiFi and the fetch task |
| `main/settings.c` | Settings in NVS over the firmware defaults |
| `main/net.c` | WiFi (via the ESP32-C6 on the P4 boards, native on the S3), setup portal, mDNS, SNTP |
| `main/dns_server.c` | The portal's DNS: every name answers with the device |
| `main/web.c`, `main/web/` | Web server, JSON API, and the setup and configuration pages |
| `main/feed.c` | HTTPS poll and readsb JSON parsing (buffers in PSRAM) |
| `main/battery.c` | Battery state, charge estimate, time left, low-voltage power-off |
| `main/backlight.c` | Brightness, night dimming by sun elevation or clock, the page's preview |
| `main/trails.c` | Position history per aircraft for the trails |
| `main/route.c` | Origin and destination by callsign from adsb.im, cached |
| `main/settings_ui.c` | The on-device settings screen |
| `main/splash.c` | The start-up screen |
| `main/zones.c` | Timezones and their POSIX rules, for the page and the screen |
| `main/ota.c` | Updates from GitHub releases, image check, rollback confirmation |
| `web/flash/`, `.github/workflows/` | Web flasher page and the build, release and Pages workflow |
| `main/openaip.c` | OpenAIP airspace and airfields: fetch by country, trim to range, project to NM |
| `main/radar_ui.c` | LVGL scope, aircraft rendering, touch, detail card, Tab5 list; per-board sizes at the top |
| `boards/<board>/board/` | Per-board bring-up: display start, WiFi power, LVGL lock; the BSP is its dependency |
| `boards/s3_28c/waveshare__esp32_s3_touch_lcd_28c` | 2.8C BSP, vendored from traffic-display (Apache-2.0) |

The P4 3.4C and Tab5 BSPs come from the component registry
(`waveshare/esp32_p4_wifi6_touch_lcd_xc` 3.0.1, `espressif/m5stack_tab5` 1.3.1),
each fetched only for its own board.

## Licence

Apache License 2.0; see [LICENSE](LICENSE). The vendored 2.8C BSP keeps its
own Apache-2.0 terms. The licence covers this code only: aircraft data from
adsb.fi and adsb.lol, routes from adsb.im, airspace from OpenAIP and the
IP-location answers from ipinfo.io and ipwho.is are under those services'
own terms.
