// LVGL's heap, in PSRAM.
//
// With the C library's malloc, LVGL's allocations of 1 KB or less went to
// internal RAM (SPIRAM_MALLOC_ALWAYSINTERNAL), and nearly all of them are:
// objects, styles, label text, draw tasks. Opening the settings screen took
// 36 KB of internal RAM in small pieces (72 to 36 KB free, measured on the
// Tab5, 12 October 2026). esp_hosted then couldn't get a 3.5 KB receive
// buffer, dropped data, and the network stopped until a restart. LVGL's
// draw buffers don't come from here: the board code allocates them, in
// internal DMA memory.

#include "lvgl.h"

#if LV_USE_STDLIB_MALLOC == LV_STDLIB_CUSTOM

#include <stdlib.h>

#include "esp_heap_caps.h"

#define CAPS (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)

void lv_mem_init(void)
{
}

void lv_mem_deinit(void)
{
}

lv_mem_pool_t lv_mem_add_pool(void *mem, size_t bytes)
{
    return NULL;
}

void lv_mem_remove_pool(lv_mem_pool_t pool)
{
}

void *lv_malloc_core(size_t size)
{
    void *p = heap_caps_malloc(size, CAPS);
    return p ? p : malloc(size); // PSRAM full: better internal than nothing
}

void *lv_realloc_core(void *p, size_t new_size)
{
    void *q = heap_caps_realloc(p, new_size, CAPS);
    return q ? q : realloc(p, new_size);
}

void lv_free_core(void *p)
{
    free(p);
}

void lv_mem_monitor_core(lv_mem_monitor_t *mon_p)
{
}

lv_result_t lv_mem_test_core(void)
{
    return LV_RESULT_OK;
}

#endif
