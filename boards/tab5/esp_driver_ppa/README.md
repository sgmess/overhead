# esp_driver_ppa, patched

ESP-IDF 6.1's PPA driver (`components/esp_driver_ppa`, copied without its
`test_apps`), with one change in `src/ppa_srm.c`: the scale-rotate-mirror
engine always bypasses macro-block ordering
(`ppa_ll_srm_bypass_mb_order(..., true)`), instead of only when IDF's
DIG-734 workaround reckons it necessary. That is the change
`esp_lvgl_adapter` ships as `0001-bugfix-ppa-Temporary-fix-for-the-PPA-hang-issue.patch`
(written against IDF 6.0, so applied here by hand).

Why: in landscape the Tab5 draws through esp_lvgl_adapter in
`TRIPLE_PARTIAL` mode, and the PPA rotates every flushed area. With IDF's
condition, a rotation sometimes never finished: the LVGL task waited in
`ppa_do_operation()` forever, holding the display lock, and the screen froze
(backtrace over JTAG, 19 Oct 2026; docs/PERFORMANCE.md). Small or irregular
areas made it likely: the sweep drawn in fewer, larger pieces froze within
seconds.

Being in `boards/tab5/`, a component directory of the Tab5 build only, it
replaces IDF's own component for that board. The 3.4C doesn't rotate.
Its `CMakeLists.txt` stops the build on any IDF but 6.1.0. On a new IDF,
check whether `ppa_srm.c` fixes the hang: if it does, delete this directory;
if not, copy the new driver here and make the same change again. Then
confirm with the sweep benchmark in docs/PERFORMANCE.md.
