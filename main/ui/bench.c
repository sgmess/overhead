#include "bench.h"

#if CONFIG_LV_USE_SNAPSHOT
#include "esp_timer.h"
#include "lvgl.h"

static int s_frames, s_requests;
static int64_t s_drawn_px;
static lv_area_t s_biggest;
static int64_t s_started_us, s_frame_start_us, s_frame_total_us, s_frame_max_us;
static int64_t s_flush_start_us, s_flush_total_us, s_wait_start_us, s_wait_total_us;
static int64_t s_render_start_us, s_prep_total_us, s_render_total_us, s_scope_cb_us;
static int s_flushes, s_sweep_ticks, s_rendered;
static bool s_on;

void bench_note_scope_draw(int64_t us)
{
    if (s_on) s_scope_cb_us += us;
}

void bench_note_sweep(void)
{
    if (s_on) s_sweep_ticks++;
}

static void on_render_start(lv_event_t *e)
{
    s_render_start_us = esp_timer_get_time();
    s_rendered++;
    if (s_frame_start_us) s_prep_total_us += s_render_start_us - s_frame_start_us;
}

static void on_render_ready(lv_event_t *e)
{
    if (s_render_start_us) s_render_total_us += esp_timer_get_time() - s_render_start_us;
    s_render_start_us = 0;
}

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
    s_requests++;
    if (lv_area_get_size(a) > lv_area_get_size(&s_biggest)) s_biggest = *a;
}

static void on_flush_start(lv_event_t *e)
{
    const lv_area_t *a = lv_event_get_param(e);
    if (a) s_drawn_px += lv_area_get_size(a);
    s_flushes++;
    s_flush_start_us = esp_timer_get_time();
}

static void on_flush_finish(lv_event_t *e)
{
    if (s_flush_start_us) s_flush_total_us += esp_timer_get_time() - s_flush_start_us;
    s_flush_start_us = 0;
}

static void on_wait_start(lv_event_t *e)
{
    s_wait_start_us = esp_timer_get_time();
}

static void on_wait_finish(lv_event_t *e)
{
    if (s_wait_start_us) s_wait_total_us += esp_timer_get_time() - s_wait_start_us;
    s_wait_start_us = 0;
}

static const struct {
    lv_event_cb_t cb;
    lv_event_code_t code;
} EVENTS[] = {
    {on_refr_start, LV_EVENT_REFR_START},       {on_refr_ready, LV_EVENT_REFR_READY},
    {on_invalidate, LV_EVENT_INVALIDATE_AREA},  {on_flush_start, LV_EVENT_FLUSH_START},
    {on_flush_finish, LV_EVENT_FLUSH_FINISH},   {on_wait_start, LV_EVENT_FLUSH_WAIT_START},
    {on_wait_finish, LV_EVENT_FLUSH_WAIT_FINISH}, {on_render_start, LV_EVENT_RENDER_START},
    {on_render_ready, LV_EVENT_RENDER_READY},
};

void bench_start(void)
{
    s_frames = s_requests = 0;
    s_drawn_px = 0;
    s_biggest = (lv_area_t){0, 0, -1, -1};
    s_frame_start_us = s_frame_total_us = s_frame_max_us = 0;
    s_flush_start_us = s_flush_total_us = s_wait_start_us = s_wait_total_us = 0;
    s_render_start_us = s_prep_total_us = s_render_total_us = s_scope_cb_us = 0;
    s_flushes = s_sweep_ticks = s_rendered = 0;
    s_on = true;
    s_started_us = esp_timer_get_time();
    lv_display_t *d = lv_display_get_default();
    for (size_t i = 0; i < sizeof(EVENTS) / sizeof(EVENTS[0]); i++) {
        lv_display_add_event_cb(d, EVENTS[i].cb, EVENTS[i].code, NULL);
    }
}

void bench_stop(bench_t *out)
{
    lv_display_t *d = lv_display_get_default();
    for (size_t i = 0; i < sizeof(EVENTS) / sizeof(EVENTS[0]); i++) {
        lv_display_remove_event_cb_with_user_data(d, EVENTS[i].cb, NULL);
    }
    s_on = false;
    const int f = s_frames ? s_frames : 1;
    out->prep_us = (int)(s_prep_total_us / f);
    out->render_us = (int)((s_render_total_us - s_flush_total_us) / f);
    out->flush_us = (int)(s_flush_total_us / f);
    out->flushes_x10 = s_flushes * 10 / f;
    out->scope_cb_us = (int)(s_scope_cb_us / f);
    out->sweep_ticks = s_sweep_ticks;
    out->rendered = s_rendered;
    const int64_t span = esp_timer_get_time() - s_started_us;
    out->frames = s_frames;
    out->avg_us = s_frames ? (int)(s_frame_total_us / s_frames) : 0;
    out->max_us = (int)s_frame_max_us;
    out->busy_pct = span > 0 ? (int)(100 * s_frame_total_us / span) : 0;
    out->flush_pct = span > 0 ? (int)(100 * s_flush_total_us / span) : 0;
    out->wait_pct = span > 0 ? (int)(100 * s_wait_total_us / span) : 0;
    out->drawn_kpx = (int)(s_drawn_px / 1000);
    out->requests = s_requests;
    out->biggest_x = s_biggest.x1;
    out->biggest_y = s_biggest.y1;
    out->biggest_w = lv_area_get_width(&s_biggest);
    out->biggest_h = lv_area_get_height(&s_biggest);
}
#endif
