# Overhead

A small, single-screen ADS-B radar for Waveshare round touch displays. It joins
WiFi, polls a free online ADS-B feed, and plots the aircraft around a fixed
point on a radar scope.

| Board | `OVERHEAD_BOARD` | Panel |
| --- | --- | --- |
| ESP32-P4-WIFI6-Touch-LCD-3.4C | `p4_34c` (default) | 800x800 MIPI DSI, WiFi via ESP32-C6 |
| ESP32-S3-Touch-LCD-2.8C | `s3_28c` | 480x480 ST7701 RGB, native WiFi |

- Aircraft drawn as arrowheads along their track with a one-minute velocity
  vector, coloured by altitude (amber low, lime, cyan, blue, violet high).
- Radar-style data tags: callsign, altitude in hundreds of feet, and a climb or
  descent arrow. The nearest aircraft get labelled first.
- Positions are moved forward between polls using ground speed and track, so
  traffic moves smoothly instead of jumping every few seconds.
- Emergency squawks (7500/7600/7700) are drawn red and circled.
- UTC clock, rotating sweep, compass rose, range rings.

Data comes from [adsb.lol](https://adsb.lol), with an automatic switch to
[adsb.fi](https://adsb.fi) if a request fails. Neither needs an API key.

## Touch

| Tap | Does |
| --- | --- |
| An aircraft | Opens its card: registration, type, altitude, V/S, speed, track, squawk, range and bearing |
| Empty scope, card open | Closes the card |
| Empty scope | Next range: 5, 10, 25, 50, 100 NM |

## Build and flash

Needs ESP-IDF 5.5. Each board has its own build directory, `sdkconfig.<board>`
and `dependencies.lock.<board>`, so building one never touches the other.

```bash
. ~/esp/esp-idf/export.sh

# P4 3.4C
idf.py menuconfig                  # Overhead radar -> WiFi, centre lat/lon
idf.py build flash monitor

# S3 2.8C
idf.py -B build_s3 -D OVERHEAD_BOARD=s3_28c menuconfig
idf.py -B build_s3 build flash monitor
```

Every setting lives under **Overhead radar** in menuconfig: WiFi, centre
position, starting range, primary feed, poll interval, ground traffic, sweep,
and the label limit. The default centre is Heathrow.

### S3 2.8C notes

The panel has no frame memory: the S3 streams the frame out of PSRAM
continuously, and anything that starves that bus shifts or tears the picture.
The settings in `sdkconfig.defaults.s3_28c` come from traffic-display, where
they ran stable with WiFi up. This app also runs TLS, so mbedTLS is moved to
PSRAM to keep internal RAM free. It needs the Touch variant of the board: the
non-touch 2.8C aborts at boot because its GT911 never answers.

## Layout

| File | Purpose |
| --- | --- |
| `main/main.c` | Starts the display, WiFi and the fetch task |
| `main/net.c` | WiFi station (via the ESP32-C6 on the P4, native on the S3), SNTP |
| `main/feed.c` | HTTPS poll and readsb JSON parsing (buffers in PSRAM) |
| `main/radar_ui.c` | LVGL scope, aircraft rendering, touch, detail card; per-panel sizes at the top |
| `boards/waveshare__esp32_s3_touch_lcd_28c` | 2.8C BSP, vendored from traffic-display (Apache-2.0) |

The P4 board support package is `waveshare/esp32_p4_wifi6_touch_lcd_xc` 3.0.1
from the component registry, fetched only for the P4 build.
