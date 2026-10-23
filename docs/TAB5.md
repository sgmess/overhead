[← Overhead](../README.md)

# Tab5 notes

What is particular to the M5Stack Tab5: its layout and display pipeline,
internal RAM, ESP-IDF 6.1, the ESP32-C6's firmware and the battery. Some of
it applies to the 3.4C too, where it says so.

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
