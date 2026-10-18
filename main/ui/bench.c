#include "bench.h"

#if CONFIG_LV_USE_SNAPSHOT
#include "esp_timer.h"
#include "lvgl.h"

static int s_frames, s_inv_areas;
static int64_t s_inv_px;
static lv_area_t s_biggest;
static int64_t s_started_us, s_frame_start_us, s_frame_total_us, s_frame_max_us;

static void on_refr_start(lv_event_t *e)
{
    s_frame_start_us = esp_timer_get_time();
}

static void on_refr_ready(lv_event_t *e)
{
    if (!s_frame_start_us) return;
    const int64_t d = esp_timer_get_time() - s_frame_start_us;
    s_frames++;
    s_frame_total_us += d;
    if (d > s_frame_max_us) s_frame_max_us = d;
}

static void on_invalidate(lv_event_t *e)
{
    const lv_area_t *a = lv_event_get_param(e);
    if (!a) return;
    s_inv_areas++;
    s_inv_px += lv_area_get_size(a);
    if (lv_area_get_size(a) > lv_area_get_size(&s_biggest)) s_biggest = *a;
}

void bench_start(void)
{
    s_inv_areas = 0;
    s_inv_px = 0;
    s_biggest = (lv_area_t){0, 0, -1, -1};
    s_frames = 0;
    s_frame_start_us = s_frame_total_us = s_frame_max_us = 0;
    s_started_us = esp_timer_get_time();
    lv_display_t *d = lv_display_get_default();
    lv_display_add_event_cb(d, on_refr_start, LV_EVENT_REFR_START, NULL);
    lv_display_add_event_cb(d, on_refr_ready, LV_EVENT_REFR_READY, NULL);
    lv_display_add_event_cb(d, on_invalidate, LV_EVENT_INVALIDATE_AREA, NULL);
}

void bench_stop(bench_t *out)
{
    lv_display_t *d = lv_display_get_default();
    lv_display_remove_event_cb_with_user_data(d, on_refr_start, NULL);
    lv_display_remove_event_cb_with_user_data(d, on_refr_ready, NULL);
    lv_display_remove_event_cb_with_user_data(d, on_invalidate, NULL);
    out->inv_areas = s_inv_areas;
    out->inv_kpx = (int)(s_inv_px / 1000);
    out->biggest_kpx = (int)(lv_area_get_size(&s_biggest) / 1000);
    out->biggest_x = s_biggest.x1;
    out->biggest_y = s_biggest.y1;
    out->biggest_w = lv_area_get_width(&s_biggest);
    out->biggest_h = lv_area_get_height(&s_biggest);
    const int64_t span = esp_timer_get_time() - s_started_us;
    out->frames = s_frames;
    out->avg_us = s_frames ? (int)(s_frame_total_us / s_frames) : 0;
    out->max_us = (int)s_frame_max_us;
    out->busy_pct = span > 0 ? (int)(100 * s_frame_total_us / span) : 0;
}
#endif
