[← Overhead](../README.md)

# Building, releases and tests

How to build and flash Overhead, how releases and the web flasher are made,
the host tests, and the debug builds with their benchmark. The detail of every
performance experiment is in [PERFORMANCE.md](PERFORMANCE.md).

## Build and flash

Needs ESP-IDF 6.1.0 exactly for the Tab5: it builds a patched copy of IDF's
PPA driver, which stops the build on any other version until it is checked
against it (`boards/tab5/esp_driver_ppa/README.md`). Each board has its own build directory, `sdkconfig.<board>`
and `dependencies.lock.<board>`, so building one never changes the other's
settings or component versions. `managed_components/` is shared, though, so
the first build after switching boards re-fetches the components it needs.
There is nothing to configure first: WiFi and every radar setting are set on
the device (see [Setup and settings](../README.md#setup-and-settings)).

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
and the address and undefined-behaviour sanitizers on. They need Bash and a
GCC with those sanitizers (not ESP-IDF), and curl the first time on a fresh
checkout, to fetch cJSON 1.7.19 when there is no ESP-IDF build's copy in
`managed_components/`. CI runs them before a release:

```bash
tests/host/run.sh
```

## Releases, web flasher and updates

`.github/workflows/firmware.yml` builds the firmware for both boards on
every push to main, with the debug builds (`OVERHEAD_DEBUG`, so the debug
endpoints can't break unnoticed) and the host tests (`tests/host/`). A
version tag builds it again with the host tests, then publishes it, the
release first and the web flasher only once that is out:

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

## Debug builds and the display benchmark

`/api/debug/bench?what=` (debug builds: `idf.py -B build_tab5_debug -D
OVERHEAD_BOARD=tab5 -D OVERHEAD_DEBUG=1 build`, which CI builds on pushes to main) times the display for five seconds: `settings` scrolls the
settings screen, `scope` redraws the whole scope every frame, `radar` leaves
it as it is, `big` the big view, and `covered` holds the settings screen
still over it. It gives frames a second, the average and longest frame, the
share of time the display was busy and how much of that went on handing
frames to the panel, the pixels drawn, and internal RAM free now and at its
lowest since boot; `tools/bench.sh` runs every mode.
[PERFORMANCE.md](PERFORMANCE.md) records every display and memory
experiment so far, with the results and why each was kept or dropped.
