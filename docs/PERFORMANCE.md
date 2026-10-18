# Performance: what was measured, and what was decided

A record of the display and memory experiments, so they aren't run again
without reason. Each entry gives the result and why it was kept or dropped.
Figures are from the M5Stack Tab5 unless they say otherwise. Re-measure
before reversing a decision: a newer component can change the answer.

## How to measure

Build with LVGL's snapshot on in the local `sdkconfig.<board>` only
(`CONFIG_LV_USE_SNAPSHOT=y`, never in the defaults). That adds the
`/api/debug/*` endpoints, among them `/api/debug/bench`. Then run:

```bash
tools/bench.sh 192.168.1.226
```

| Mode (`?what=`) | What it times |
| --- | --- |
| `settings` | Scrolls the settings screen up and down: a full-screen redraw every frame. The smoothness you feel. |
| `scope` | Redraws the whole radar scope every frame: what the once-a-second scope redraw costs. |
| `radar` | The radar as it is, with nothing forced: the everyday cost. |
| `big` | The big view as it is. |
| `covered` | The settings screen open and still over the radar. |

Each run reports:

- **fps, and the average and longest refresh.** A refresh runs from LVGL's
  `REFR_START` to `REFR_READY`, so it includes handing the frame to the panel.
- **`busy`:** the share of the time spent refreshing. **This is display
  pipeline occupancy, not CPU use.** It includes time inside the flush and
  waiting, so 96% means the display was never idle, not that the CPU was 96%
  used.
- **`flush` and `wait`:** within `busy`, the time inside the flush callback
  (`FLUSH_START` to `FLUSH_FINISH`) and in LVGL's own wait for a flush to
  finish (`FLUSH_WAIT_START` to `FLUSH_WAIT_FINISH`). With the adapter the
  waiting happens inside its flush callback, where it also rotates, so `wait`
  reads 0% and `flush` carries both. Drawing is roughly `busy` minus `flush`.
- **`drawn`:** the pixels handed to the panel (the areas of `FLUSH_START`):
  what was really drawn.
- **`requests` and the biggest request:** LVGL's `INVALIDATE_AREA` events.
  They fire before duplicate areas are dropped, and again while LVGL sizes the
  draw buffer, so they count requests, not areas drawn. The biggest still
  shows when the whole screen or scope was asked for. (Before 19 Oct 2026
  these were reported as `inv_areas` and `inv_kpx`, wrongly suggesting areas
  drawn.)
- **Internal RAM free,** now and the lowest since boot.

Feed parsing and the aircraft layout aren't timed; add that before
optimising either.

**Pitfalls, each of which has cost a wrong result:**

- **Traffic moves the scope numbers by about 15%** from one run to the next,
  as aircraft, labels and trails come and go. Compare builds with runs taken
  back to back, and never against figures from another day. `settings`
  doesn't depend on traffic and is steady to within 1%.
- **Reading the MAC (`esptool.py read-mac`) resets the board**, and so does
  opening its serial port. Wait about 45 s after either before benchmarking.
- **A failed build still lets `idf.py flash` run the old image.** Check the
  build passed before trusting the numbers.
- **The settings screen closes after a minute untouched**, and a board that
  has just booted already counts as untouched. The bench registers activity
  as it opens the screen. Before that fix, `covered` was partly measuring the
  radar.
- **After an over-the-air update or a rollback, `idf.py flash` can leave the
  board booting the other app slot.** Check that `/api/state`'s `firmware`
  ends in `-dirty`; if it doesn't, delete `build_tab5/*_flashed.bin` and
  `build_tab5/*/*_flashed.bin` and flash again.

## Current figures (19 Oct 2026, v1.0.3 + changes)

Tab5, landscape (rotation 90), trails 120 s, about 80 aircraft. Sweep off
and on, back to back:

| Mode | Sweep off | Sweep on |
| --- | --- | --- |
| `settings` | 20.7 fps, 47 ms, busy 96% (flush 44%) | the same: no sweep while covered |
| `scope` | 13–14 fps, 73 ms, busy 97% (flush 15%) | 9 fps, 108 ms, busy 98% (flush 10%) |
| `radar` | 2.3 fps, 51–66 ms, **busy 12–15%** (flush 2%) | 51 fps, 11 ms, **busy 55%** (flush 11%) |
| `big` | 2.3 fps, 14–25 ms, busy 3–5% | the same |
| `covered` | under 1 fps, busy about 0% | the same |

Tab5 portrait (rotation 0), sweep off: `settings` **25.6 fps**, 38 ms, busy
97% (flush 18%); `scope` 14 fps, 71 ms (flush 7%).

Internal RAM: about 178–181 KB free while running, lowest since boot about
95–104 KB. Static D/IRAM 128,670 bytes against CI's 140,000 limit.

3.4C (800x800, never rotated), on v1.0.1: `settings` 46 fps, 21 ms a frame.

## Tab5 display pipeline

The panel is natively portrait, 720x1280. Landscape, the default, needs every
frame rotated.

- **With esp_lvgl_port** (up to v1.0.1), rotation was most of the cost:
  about 45 ms of each 81 ms landscape frame.
- **With the adapter it is about 30%.** Scrolling the settings, the flush
  (rotation, copying and waiting for a frame buffer) takes 44% of a 47 ms
  landscape frame, about 21 ms. In portrait, with no rotation, it takes 18%
  of a 38 ms frame, about 7 ms. So rotation costs about 14 ms a frame, and
  portrait scrolls at 25.6 fps against landscape's 20.7.
- **The rest of the gap to the 3.4C's 46 fps** is drawing: about 26–31 ms a
  frame on the Tab5. The likely causes are 44% more pixels (921,600 against
  640,000), PSRAM at 200 MHz against 250, and a rev v1.3 chip against
  rev v3.1. That split hasn't been measured on the 3.4C.

### esp_lvgl_port (up to v1.0.1): replaced

The BSP's own display layer. Measured 16 Oct 2026 scrolling the settings:

| Setup | fps | Frame |
| --- | --- | --- |
| Landscape, 24-line strips in internal RAM (as shipped) | 12 | 81 ms |
| Portrait, no rotation | 27 | 36 ms |
| Landscape, 48-line strips in internal RAM | — | boot loop |
| Landscape, strips in PSRAM (24, 160 or 360 lines) | 7–7.5 | about 132 ms |

- **Where the time went:** about 45 ms of each 81 ms landscape frame was
  rotation. esp_lvgl_port has the PPA rotate each strip, waits for it, then
  copies the result into the frame buffer. Drawing the next strip can't
  overlap any of that.
- **48-line strips** don't fit: `lvgl_port_ppa_create` asserts
  (`ppa_ctx->buffer != NULL`) at boot, out of internal RAM, and the board
  reboots in a loop.
- **Strips in PSRAM** fit at any height and free about 100 KB of internal
  RAM, but drawing into PSRAM is slower: 7 fps whatever the height. So strip
  height was never the bottleneck.

### esp_lvgl_adapter (v1.0.2 on): kept

The Tab5 now draws through `esp_lvgl_adapter`, as the 3.4C does, in
`TRIPLE_PARTIAL` mode. LVGL draws into one internal buffer, and in landscape
the PPA rotates it straight into one of three PSRAM frame buffers
(`CONFIG_BSP_LCD_DPI_BUFFER_NUMS=3`, about 5.5 MB of PSRAM).

| | esp_lvgl_port | Adapter 0.6.4 |
| --- | --- | --- |
| Landscape | 12 fps | 16.7 fps |
| Portrait | 27 fps | 20.5 fps |
| Internal RAM free while running | about 135 KB | about 173 KB |

- **Why kept:** landscape, the normal setup, is about 40% faster and about
  40 KB more internal RAM is free.
- **Against:** portrait got slower (27 → 20.5 fps). A hybrid (the old layer
  for portrait only) was offered and not taken: two display paths to keep.
  With one drawing thread (below) portrait is back to 25.6.
- **Touch isn't rotated by the adapter.** The board code sets the touch
  driver's swap and mirror flags to match. esp_lvgl_port's
  `LV_DISPLAY_ROTATION_90` is the adapter's `ROTATE_270`.
- **IDF PPA freeze:** a patch for a PPA freeze exists upstream. It wasn't
  applied, because IDF 6.1 has its own conditional workaround and no freeze
  has been seen.

Tried on adapter 0.6.4, landscape, scrolling the settings:

| Change | fps | Verdict |
| --- | --- | --- |
| None (40-line buffer) | 16.7 | Baseline |
| `enable_ppa_accel` (the adapter's own PPA drawing) | 16.8 | No difference; left off |
| 64-line draw buffer (about 60 KB more internal RAM) | 16.7 | No difference; stayed at 40 |

### Adapter 0.7.3 and the noglib BSP (18 Oct 2026): kept

- **What:** `esp_lvgl_adapter` 0.6.4 → 0.7.3, and `m5stack_tab5` 1.3.1 →
  `m5stack_tab5_noglib` 1.3.2, the variant without esp_lvgl_port.
- **Speed:** unchanged (`settings` 17.0 fps before and after; portrait 20.7).
  About 1 KB more internal RAM free.
- **Why kept:**
  - 0.7.3 fixes races in the tear-avoiding modes, which include
    `TRIPLE_PARTIAL`.
  - 0.7 builds against LVGL 9.6's headers without the deprecated-header
    `#warning`s.
  - noglib stops building an unused display layer.
- 0.7's TE pipeline is for panels with a tearing-effect line, not this MIPI
  DPI panel, so it was never expected to add fps.
- **The 3.4C stays on 0.6.x:** Waveshare's BSP 3.0.1 pins the adapter to
  `~0.6`. It still needs the `-Wno-error=cpp` workaround, now in the top-level
  `CMakeLists.txt` for that board alone.

### LVGL drawing threads (18 Oct 2026): one on the Tab5

`CONFIG_LV_DRAW_SW_DRAW_UNIT_CNT`, back to back on adapter 0.7.3:

| Threads | `settings` | `scope` | Internal RAM free |
| --- | --- | --- | --- |
| 2 (the shared default) | 16.8 fps, 58 ms | 70–78 ms | 172.7 KB |
| 1 | **20.8 fps, 47 ms** | 76–82 ms | 180.9 KB |

- **Why one:** scrolling is 24% faster, and the second thread's 8 KB
  internal stack is free.
- **Against:** a full scope redraw is about 8% slower. That happens once a
  second and isn't visible.
- **Likely cause:** with one internal draw buffer and PSRAM shared with the
  PPA's rotation, a second thread adds contention more than parallelism.
- **Portrait gains more:** 20.7 fps with two threads, 25.6 with one
  (measured a day apart, but `settings` doesn't depend on traffic).
- Set in `sdkconfig.defaults.tab5`. **The 3.4C wasn't measured** and keeps
  two.

### LVGL's PPA renderer (18 Oct 2026): rejected

`CONFIG_LV_USE_PPA` (with `CONFIG_LV_USE_PPA_IMG`). This is LVGL's own PPA
draw unit for fills, blends and simple images, not the adapter's
`ppa_accel`.

- **Needs** `CONFIG_LV_DRAW_BUF_ALIGN=128` (the P4's L2 cache line; the
  build stops with `#error` otherwise).
- **Needs a build fix:** `esp_lv_decoder`'s `lvgl_docoder_lib` includes
  LVGL's private PPA header without depending on `esp_driver_ppa`. Linking it
  was a test-only change in CMake:
  `target_link_libraries(lvgl_docoder_lib PRIVATE idf::esp_driver_ppa)`.
- **Result, one thread:** `settings` 18.2 fps against 20.8 without; `scope`
  no better.
- **Why rejected:** slower than drawing in software here. Most of what the
  radar draws is lines, arcs and text, which the PPA can't do.
- `CONFIG_LV_USE_DRAW_DMA2D` is the STM32 renderer, not a P4 option.

### Other display limits

- **PSRAM speed:** the Tab5 runs PSRAM at 200 MHz. 250 MHz
  (`CONFIG_SPIRAM_SPEED_250M`) needs a rev v3 P4, which the 3.4C has
  (rev v3.1, set there) and the Tab5 (rev v1.3) doesn't.
- **Already on:** `-O2` (`COMPILER_OPTIMIZATION_PERF`), a 256 KB L2 cache
  with 128-byte lines, and LVGL's heap in PSRAM.

## Redraw work (18 Oct 2026)

LVGL redraws an object whenever a label's text or a style is set, **even to
the value it already has**. It also redraws whatever sits on top of an area
that is marked for redrawing: the scope redrawn under the settings screen
repaints the settings screen. Unhiding an object that is already visible is
free (`lv_obj_set_hidden` returns early), and so are `lv_obj_set_x`, `_y`,
`_width` and `_height` with an unchanged value. `lv_obj_align` and every
`lv_obj_set_style_*` are never free.

| Change | Result | Verdict |
| --- | --- | --- |
| Skip all radar drawing while the settings screen covers it or the backlight is off (`radar_hidden()`), refreshing once on waking | `covered` from about 13% busy (partly radar) to about 0%; under 1 frame a second, small areas only | Kept |
| The status line (set by the fetch task) and the battery corner (every 2 s from the battery task) skip updates while hidden, and catch up in `refresh()` once it shows | Removes the last radar redraws behind the settings screen; `covered` was already about 0% | Kept (19 Oct) |
| The big view redraws its heading pointer only when its heading or colour changes, not every second | Small | Kept (19 Oct) |
| Card, alert banner, big view and battery corner set text and colours only when they change; the 3.4C's card is aligned only when it moves | `radar` from 3.0 fps and 10% busy to 2.2 fps and 8–9% | Kept |
| Redraw only the changed parts of the scope, not all of it each second | Not built. LVGL keeps at most 32 areas to redraw a frame and redraws the whole screen past that; about 80 aircraft, each with a label and trail, always pass it. **Estimate, not measured:** at most about 4% of the time, on a scope drawn once a second. | Rejected |
| Look the route up once per refresh, not twice (`fmt_route` and `fmt_progress`) | A mutex and a cache copy: microseconds (estimate) | Not worth the code |
| Cache the trigonometry in `layout()` | **Estimate, not measured:** once a second, about 80 aircraft × about 6 double-precision calls. The P4's FPU is single precision, so doubles are done in software: roughly 1–3 ms a second. | Only if profiling points there |
| Parse the feed's JSON selectively, not into a full cJSON tree | **Not measured:** about 45 KB every 5 s, into PSRAM; probably a few ms | Measure first |

With sweep off, the radar's remaining cost is one full scope redraw a second
(about 50–65 ms) plus the list and labels.

### The sweep (19 Oct 2026): the largest remaining cost

The sweep is on by default (`settings_defaults()`), so a new device runs it.
It moves eight short line segments every 33 ms (`SWEEP_TICK_MS`); eight
keep each one's redraw area small, where one long diagonal line would ask
for up to a quarter of the scope.

- **Cost:** the radar goes from 12–15% busy to 55%, at about 51 frames a
  second of about 11 ms each. The full scope redraw also slows, from 73 to
  108 ms, with the segments in every frame.
- **Already free:** it stops while the radar is hidden (settings screen or
  asleep), and its lines are hidden in the big view, where hidden objects ask
  for no redraw. `big`, `settings` and `covered` measure the same with it on
  or off.
- **Why 11 ms for a small area:** the scope's draw already skips aircraft,
  labels and airspace outside the area being drawn (`in_clip()` in
  `scope.c`). So most of each frame is fixed overhead per frame (LVGL's
  refresh, the background image for each area, the adapter's flush and
  rotation) rather than drawing the sweep itself. Not broken down further.
- **Next candidates, untested:** a slower tick (66 ms would roughly halve
  the cost, but looks less smooth); a profile of a sweep frame, to see which
  overhead dominates.

## Memory decisions (from earlier versions)

Covered in the README's Tab5 notes; listed here so they aren't undone:

- **Internal RAM at boot is the Tab5's tightest constraint.** esp_hosted
  allocates its SDIO buffers before `app_main`. Its queues went from 20 to 12
  deep (125 KB → 99 KB) after v0.2.0 boot-looped. CI fails a Tab5 build whose
  static D/IRAM passes 140,000 bytes.
- **esp_hosted's buffers in PSRAM** (`ESP_HOSTED_MEMPOOL_PREFER_SPIRAM`)
  broke the first transmit after joining on every boot: rejected.
- **LVGL's heap in PSRAM** (`main/ui/lvgl_mem.c`): with the C library's
  malloc, LVGL's small allocations filled internal RAM until esp_hosted
  couldn't get a 3.5 KB receive buffer (v0.2.5).
- **TLS buffers in PSRAM** (`MBEDTLS_EXTERNAL_MEM_ALLOC`): esp_hosted 2.x
  left too little internal RAM for a full TLS record.
- **Code runs from PSRAM** (`SPIRAM_XIP_FROM_PSRAM`): otherwise every flash
  write stops the PSRAM frame buffer and the panel flashes. It costs about
  10 KB of static internal RAM.
- **Updates write flash from an internal buffer:** from PSRAM, IDF copied
  32 bytes at a time with the caches paused, starving the C6's link until the
  download died (v0.1.1). A 2 MB update now takes about 12 s.
- **Airspace drawn once per range into a PSRAM image:** drawing about 4,000
  segments each frame took 100–400 ms at 100 NM; copying the image takes
  25–65 ms.

## Components (checked 18 Oct 2026)

| Component | In use | Newest | Notes |
| --- | --- | --- | --- |
| ESP-IDF | 6.1 | 6.1 | No 6.1.x patch release yet |
| LVGL | 9.6.0~1 | 9.6.0~1 | — |
| esp_lvgl_adapter | 0.7.3 Tab5, 0.6.4 3.4C | 0.7.3 | The 3.4C's BSP pins `~0.6` |
| m5stack_tab5(_noglib) | 1.3.2 | 1.3.2 | 1.3.2 releases the LCD reset before detection, which our board code also does |
| waveshare/esp32_p4_wifi6_touch_lcd_xc | 3.0.1 | 3.0.1 | — |
| esp_hosted | 2.12.13 | 3.0.9 | The host's major version must match the C6 firmware (2.12.6 on the Tab5); 3.x means updating the C6 first |
| esp_wifi_remote, mdns, cJSON | current | — | — |
