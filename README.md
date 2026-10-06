# Overhead

A small, single-screen ADS-B radar for ESP32 touch displays. It joins
WiFi, polls a free online ADS-B feed, and plots the aircraft around a fixed
point on a radar scope.

| Board | `OVERHEAD_BOARD` | Panel |
| --- | --- | --- |
| ESP32-P4-WIFI6-Touch-LCD-3.4C | `p4_34c` (default) | 800x800 MIPI DSI, WiFi via ESP32-C6 |
| ESP32-S3-Touch-LCD-2.8C | `s3_28c` | 480x480 ST7701 RGB, native WiFi |
| M5Stack Tab5 | `tab5` | 720x1280 MIPI DSI portrait, WiFi via ESP32-C6 |

- Aircraft drawn as arrowheads along their track with a one-minute velocity
  vector, coloured by altitude (amber low, lime, cyan, blue, violet high).
- Radar-style data tags: callsign, altitude in hundreds of feet, and a climb or
  descent arrow. The nearest aircraft get labelled first.
- Positions are moved forward between polls using ground speed and track, so
  traffic moves smoothly instead of jumping every few seconds.
- Emergency squawks (7500/7600/7700) are drawn red and circled.
- UTC clock, rotating sweep, compass rose, range rings.
- Battery level, charging state and time left on the Tab5, with charging
  enabled and a clean power-off before the pack runs flat.

Data comes from [adsb.lol](https://adsb.lol), with an automatic switch to
[adsb.fi](https://adsb.fi) if a request fails. Neither needs an API key.

## Touch

| Tap | Does |
| --- | --- |
| An aircraft | Opens its card: registration, type, altitude, V/S, speed, track, squawk, range and bearing |
| Empty scope, card open | Closes the card |
| Empty scope | Next range: 5, 10, 25, 50, 100 NM |
| A list row (Tab5) | Selects that aircraft; tap it again to clear |

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

# M5Stack Tab5
idf.py -B build_tab5 -D OVERHEAD_BOARD=tab5 menuconfig
idf.py -B build_tab5 build flash monitor
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

### Tab5 notes

Portrait, the panel's native orientation: the scope fills the top 720x720 and
the nearest aircraft are listed below it, with the details card above the list
while something is selected. The BSP (`espressif/m5stack_tab5`) detects the
three panel revisions at boot by which touch controller answers on I2C. The
board code releases the panel and touch resets and waits for that controller
first, because without it detection failed on a revision-3 (ST7121) unit. The
Tab5's P4 is a rev v1.x chip, so its build uses the pre-v3 settings. The
ESP32-C6's power is switched through an I/O expander before WiFi starts.

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
down and the firmware powers off (Overhead radar > Battery in menuconfig). The
With no pack fitted the charger's output doesn't
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
| `main/net.c` | WiFi station (via the ESP32-C6 on the P4 boards, native on the S3), SNTP |
| `main/feed.c` | HTTPS poll and readsb JSON parsing (buffers in PSRAM) |
| `main/battery.c` | Battery state, charge estimate, time left, low-voltage power-off |
| `main/radar_ui.c` | LVGL scope, aircraft rendering, touch, detail card, Tab5 list; per-board sizes at the top |
| `boards/<board>/board/` | Per-board bring-up: display start, WiFi power, LVGL lock; the BSP is its dependency |
| `boards/s3_28c/waveshare__esp32_s3_touch_lcd_28c` | 2.8C BSP, vendored from traffic-display (Apache-2.0) |

The P4 3.4C and Tab5 BSPs come from the component registry
(`waveshare/esp32_p4_wifi6_touch_lcd_xc` 3.0.1, `espressif/m5stack_tab5` 1.3.1),
each fetched only for its own board.
