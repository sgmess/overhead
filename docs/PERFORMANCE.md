# Performance: what was measured, and what was decided

A record of the display and memory experiments, so they aren't run again
without reason. Each entry gives the result and why it was kept or dropped.
Figures are from the M5Stack Tab5 unless they say otherwise. Re-measure
before reversing a decision: a newer component can change the answer.

## How to measure

Make a debug build, in a build directory of its own:

```bash
idf.py -B build_tab5_debug -D OVERHEAD_BOARD=tab5 -D OVERHEAD_DEBUG=1 build
```

That layers `sdkconfig.defaults.debug` (LVGL's snapshot) on top, in its own
`sdkconfig.tab5.debug`, and adds the `/api/debug/*` endpoints, among them
`/api/debug/bench`. (Before 20 Oct 2026 it meant setting
`CONFIG_LV_USE_SNAPSHOT=y` by hand in the local sdkconfig.) Every POST needs
`Content-Type: application/json`. Then run:

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
- **A frame in stages,** averaged over all frames: `prep` (refresh start to
  render start: layout updates and joining areas), `render` (render start to
  end, less the flushes), `flush`, the flushes a frame, and `scope` (time in
  the scope's draw callback, which queues its drawing and is part of
  `render`).
- **`rendered`:** frames that drew anything. LVGL also runs refreshes that
  find nothing to draw (see the sweep below), which count in `frames`, so
  divide by `rendered` for the cost of a real frame.
- **`sweep_ticks`:** sweep steps taken.

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
- **After a USB flash the Tab5 can stick in a ROM panic** loading the
  bootloader (`Store/AMO access fault`, MTVAL 0x50091000, the SHA block the
  ROM hashes the bootloader with). The host's USB reset leaves the crypto
  block unusable. `esptool.py --after watchdog-reset read-mac` boots it; then
  re-attach the USB to WSL, as the board re-enumerates. Not something a
  restart from the firmware or an over-the-air update runs into.
- **After an over-the-air update or a rollback, `idf.py flash` can leave the
  board booting the other app slot.** Check that `/api/state`'s `firmware`
  ends in `-dirty`; if it doesn't, delete `build_tab5/*_flashed.bin` and
  `build_tab5/*/*_flashed.bin` and flash again.

## Current figures (19 Oct 2026, v1.0.3 + changes)

Tab5, landscape (rotation 90), trails 120 s, about 80 aircraft. Sweep off
and on, back to back. **The "Sweep on" column is v1.0.4's sweep** (8
segments, a turn in 4 s, a step every 33 ms). Since v1.0.5 (4 segments, a
turn in 8 s, a step every 50 ms) the `radar` row with the sweep on is about
25–29% busy; see "The sweep" below.

| Mode | Sweep off | Sweep on (v1.0.4's sweep) |
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
- **IDF PPA freeze:** the adapter documents a display freeze on the P4 in
  `TRIPLE_PARTIAL` with rotation, and ships an IDF patch for it. It wasn't
  applied at first, because IDF 6.1 has its own conditional workaround and no
  freeze had been seen. **It does freeze; the patch is now applied** (19 Oct
  2026; see "The display lock-up" below).

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
- Set in `sdkconfig.defaults.tab5`.

#### The 3.4C (20 Oct 2026): two kept

Debug builds of the same code differing only in
`CONFIG_LV_DRAW_SW_DRAW_UNIT_CNT`, flashed in turn (two, one, two, one),
each settled 90 s, then `tools/bench.sh` with three runs of each mode:

| Threads | `settings` | `scope` | `radar` | Internal RAM free |
| --- | --- | --- | --- | --- |
| 2 | 43–46 fps, 21 ms (worst 23–34 ms) | **18–21 fps, 46–56 ms** | 1.8–2.4 fps, 25–37 ms, 5–7% busy | 145, 152 KB |
| 1 | **50–51 fps, 18 ms** (worst 21–27 ms) | 15–17 fps, 58–66 ms | 1.8–2.4 fps, 30–42 ms, 7% busy | 159, 147 KB |

- **Scrolling** is 12% faster with one thread, as on the Tab5 (24% there),
  but it is already well over 40 fps with two.
- **A full scope redraw** is 15–25% slower with one (8% on the Tab5): the
  3.4C's scope is the whole 800x800 screen, so a second thread has more to
  share. In normal running (`radar`) there is no difference.
- **Internal RAM** moved with other allocations by more than the 8 KB stack
  in both directions between rounds, so it doesn't decide anything here.
- **Kept two:** nothing a user sees gets better with one, and the full
  redraw (ranges, panning, the first frame) gets slower. The Tab5 differs:
  its scrolling was the slow case, at 17 fps.

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
It is short line segments (`SWEEP_SEGS`), moved every `SWEEP_TICK_MS`:
segments keep each one's redraw area small, where one long diagonal line
would ask for up to a quarter of the scope. **Now 4 segments, a step every
50 ms and a turn in 8 s** (v1.0.5; see "Slower sweep" below); this section
first measured v1.0.4's 8 segments every 33 ms and a turn in 4 s.

- **Cost (v1.0.4's sweep):** the radar goes from 12–15% busy to 55%, at about 51 frames a
  second of about 11 ms each. The full scope redraw also slows, from 73 to
  108 ms, with the segments in every frame.
- **Already free:** it stops while the radar is hidden (settings screen or
  asleep), and its lines are hidden in the big view, where hidden objects ask
  for no redraw. `big`, `settings` and `covered` measure the same with it on
  or off.
#### Profile of a sweep frame (19 Oct 2026)

With the frame stages above, sweep on, 8 segments, 5 s:

| | Value |
| --- | --- |
| Sweep steps | 140 |
| Refreshes | 280, of which **145 drew** and 135 found nothing |
| A real sweep frame | about 15 ms: about 10.5 ms rendering (2.4 ms of it in the scope's draw callback), 4 ms in the flush, 1 ms prep |
| Areas drawn a frame | about 9–10 (one flush each), about 37,000 pixels in all |

- **Two refreshes a step, one empty:** moving a line with `lv_obj_set_pos`
  only marks the layout dirty. LVGL works out the new position in the next
  refresh's layout stage, and the redraw request that makes then re-arms the
  refresh timer, after it was paused for this refresh. The area is drawn in
  this refresh, so the next one finds nothing. Each empty refresh costs under
  1 ms, about 2–3% busy in all. Not worth fixing on its own.
- **The cost is per area, not per pixel:** 37,000 pixels take 15 ms, where a
  full scope of 520,000 takes about 60 ms. Each area costs about 1.5 ms:
  - the scope's draw callback runs once per area and walks every aircraft,
    trail and airfield to cull them (`in_clip()`);
  - the background image is drawn into each area;
  - every area is a separate flush, which the adapter rotates and copies into
    the other frame buffers.
- **Fewer segments, fewer areas:** before the PPA fix, 2 or 4 segments
  froze the Tab5 soon after the sweep started (the PPA hang below). Retested
  with the fix, back to back on 19 Oct 2026:

  | Segments | Busy | Flushes a frame | A real frame |
  | --- | --- | --- | --- |
  | 8 | 46–50% | 4.7 | about 16 ms |
  | 4 | 34–38% | 2.6 | about 12 ms |

  About a quarter less work, and no freeze. Four segments make the fade
  towards the rim coarser (four steps of brightness instead of eight); it
  looked fine on the Tab5, so 4 is the setting now.
  `SWEEP_SEGS 1` doesn't build (a division by `SWEEP_SEGS - 1` in
  `radar_ui.c`).
- **Drawing the sweep in the scope (option 2): rejected.** The scope's draw
  callback drew it, and each step asked for one to three boxes around the old
  and new line (more near the diagonals) instead of moving eight line
  objects. It froze within seconds until the PPA fix; with the fix, back to
  back on 19 Oct 2026:

  | Sweep | Busy | Refreshes a step | Areas a frame | A frame |
  | --- | --- | --- | --- | --- |
  | 8 line objects | 45–47% | 2 (1 empty) | 4.6 flushes, 18 kpx | 8.3 ms, empty ones included |
  | Drawn in the scope | 36–45% | 1 | 3.4 flushes, 45 kpx | 13–17 ms |

  The empty refreshes went, but each box is larger, so a frame costs nearly
  as much. Not clearly faster, so the line objects stay. The code is in
  `git stash` ("WIP: sweep drawn in the scope") if it is ever wanted.
- **Slower sweep: kept (19 Oct 2026).** A turn in 8 s instead of 4, at a
  step every 50 ms instead of 33: 2.25° a step, finer than the old 3°. With
  4 segments the radar is **25–29% busy**, against 46–50% for v1.0.4's 8
  segments at 4 s. Both the speed and the look were judged fine on the
  Tab5. Stability: 64 minutes on the Tab5 with the sweep on, polled
  every 30 s: no missed answer, no restart, no freeze, internal RAM steady
  at about 176 KB; the 3.4C ran the same build an hour without a freeze.

### The display lock-up (19 Oct 2026): found and fixed

**Symptom.** The screen froze for good. The web server and the traffic
fetches, which wait for the display lock, stopped soon after. Nothing
recovered it but a restart.

**How it was found.**

1. A debug-only watchdog logged where the display was when its lock couldn't
   be had for 10 s: always inside the flush, with no refresh or flush
   finishing afterwards.
2. A JTAG backtrace (OpenOCD over the P4's USB, after installing OpenOCD's
   udev rules in WSL) showed the LVGL task in
   `ppa_do_operation()` (`esp_driver_ppa`), called from the adapter's
   `display_bridge_v9_flush_partial_rotate()`: waiting forever for a PPA
   scale-rotate-mirror operation to finish.

**Cause.** The P4's PPA can hang on some rotations: a hardware bug
(Espressif's DIG-734). IDF 6.1 works around it only when it reckons it
necessary, and on the Tab5's rotated partial flushes that wasn't enough.
esp_lvgl_adapter documents this exact case (P4, `TRIPLE_PARTIAL`, 90° or
270°) and ships `0001-bugfix-ppa-Temporary-fix-for-the-PPA-hang-issue.patch`
for IDF, which always applies the workaround.

**When it froze, before the fix.**

- Within seconds of the sweep starting with 2 or 4 line segments, or drawn
  in the scope: fewer, larger areas to rotate.
- About 6 s into boot on boots where the serial port had just been opened,
  which resets the board mid-boot.
- Not seen with the released 8 segments in a 9-minute sweep-on run, nor in
  hours of running with the sweep off. Not a regression of adapter 0.7.3:
  0.6.4 froze the same way.

**Fix.** `boards/tab5/esp_driver_ppa/` is IDF 6.1's PPA driver with the
patch's one change applied by hand (the patch was written for IDF 6.0). As a
component of the Tab5's board directory it replaces IDF's own for that build
only; the 3.4C doesn't rotate. After it, the sweep drawn in the scope, which
had frozen within seconds of every boot, ran without a freeze, and the
release candidate (8 line segments) ran with the sweep on for over 6 hours
without a freeze. Its one restart, early on, came as the host computer went
to sleep, and it cleared the freeze-restart count, which a watchdog restart
or a crash keeps: a chip reset over USB, not the firmware.

**Safety net.** `main/device/display_watch.c` restarts the device if the
display lock can't be had for 15 s, and counts those restarts since power-on
in memory kept through a restart (`display_restarts` in `/api/state`).
Before the fix, it brought a frozen Tab5 back within 15–20 s every time.

**Not needed after all:** other tear-avoid modes (`DOUBLE_PARTIAL` uses the
same rotated partial flush, so the same PPA path).

## AeroDataBox requests (20 Oct 2026): one connection, kept 10 s

A tap on an aircraft makes two or three AeroDataBox requests: today's
flights by its address (and by flight number or callsign if none of those
is flying about now), then the aircraft. Each new HTTPS connection to
`api.aerodatabox.com` costs most of a second before anything is asked.

Measured on the Tab5 with the debug build's `/api/debug/adb?n=5&keep=0|1`,
which times requests to AeroDataBox's free `/health/services/airports/…/feeds`
endpoint (no credits), alternating two paths:

| | Request times (ms) | Internal RAM held |
| --- | --- | --- |
| New connection each | 921, 829, 914, 923, 920 / 872, 832, 909, 825, 830 | none between requests |
| One connection | 1003, 94, 57, 52, 101 / 886, 207, 58, 143, 208 | about 2.6 KB while open |

The server answers `Connection: keep-alive` with chunked bodies, and
ESP-IDF's client keeps the connection across them. A real tap still opened
a second connection at first: the flight and the aircraft reach the task as
two wake-ups, and closing after each threw it away. So the connection is
kept until 10 s pass with no request (`KEEP_MS`), or the key changes.

A real tap afterwards: the flight by address 1547 ms (connecting), the
aircraft 256 ms on the same connection, against 1277–1509 ms for the
aircraft on a new one. About 1.2 s off each tap's second and third
requests, for about 3 KB of internal RAM for 10 s after a lookup: kept.

## Memory decisions (from earlier versions)

Covered in [TAB5.md](TAB5.md); listed here so they aren't undone:

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
