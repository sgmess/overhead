#include "splash.h"

#include <math.h>
#include <string.h>

#include "board.h"
#include "esp_app_desc.h"
#include "esp_timer.h"
#include "lvgl.h"

#define SPLASH_MS 10000
#define TICK_MS 50

// Sizes per board, following physical size as radar_ui.c does
#if defined(OVERHEAD_BOARD_TAB5)
#define EMBLEM_R 96
#define F_TITLE lv_font_montserrat_32
#define F_LINE lv_font_montserrat_20
#define F_SMALL lv_font_montserrat_16
#define GAP 18
#elif defined(OVERHEAD_BOARD_P4_34C)
#define EMBLEM_R 84
#define F_TITLE lv_font_montserrat_28
#define F_LINE lv_font_montserrat_16
#define F_SMALL lv_font_montserrat_14
#define GAP 14
#else // S3 2.8C
#define EMBLEM_R 52
#define F_TITLE lv_font_montserrat_20
#define F_LINE lv_font_montserrat_14
#define F_SMALL lv_font_montserrat_12
#define GAP 8
#endif
#define ARC_GAP (EMBLEM_R / 8)
#define ARC_W (EMBLEM_R / 20 + 2)
#define EMBLEM_SIZE (2 * (EMBLEM_R + ARC_GAP + ARC_W))

// radar_ui.c's palette
#define C_BG 0x030806
#define C_DISC 0x06120d
#define C_RING 0x1c4535
#define C_TICK 0x3d7a61
#define C_TEXT 0x7cc9a4
#define C_DIM 0x4b8a6d
#define C_SWEEP 0x46ffa8
#define C_HOME 0xe8fff4
#define C_LOW 0xffb020  // amber, low
#define C_MID 0x22d3ee  // cyan, 20,000 ft
#define C_HIGH 0xc084fc // violet, high

static lv_obj_t *s_root, *s_status;
static lv_timer_t *s_timer;
static int64_t s_started_us;

static void close_splash(void)
{
    if (s_timer) {
        lv_timer_delete(s_timer);
        s_timer = NULL;
    }
    if (s_root) {
        lv_obj_delete_async(s_root); // perhaps from its own press event
        s_root = NULL;
        s_status = NULL;
    }
}

static void line(lv_layer_t *layer, int x1, int y1, int x2, int y2, uint32_t color, int width, lv_opa_t opa)
{
    lv_draw_line_dsc_t d;
    lv_draw_line_dsc_init(&d);
    d.p1.x = x1;
    d.p1.y = y1;
    d.p2.x = x2;
    d.p2.y = y2;
    d.color = lv_color_hex(color);
    d.width = width;
    d.opa = opa;
    d.round_start = d.round_end = 1;
    lv_draw_line(layer, &d);
}

static void ring(lv_layer_t *layer, int x, int y, int r, uint32_t color, int width)
{
    lv_draw_arc_dsc_t d;
    lv_draw_arc_dsc_init(&d);
    d.center.x = x;
    d.center.y = y;
    d.radius = r;
    d.start_angle = 0;
    d.end_angle = 360;
    d.width = width;
    d.color = lv_color_hex(color);
    lv_draw_arc(layer, &d);
}

// An aircraft as the radar draws it: an arrowhead along its track
static void arrow(lv_layer_t *layer, int x, int y, float track_deg, uint32_t color, float scale)
{
    static const float shape[4][2] = {{0, 11}, {-7, -7}, {0, -3}, {7, -7}}; // {right, forward}
    const float t = track_deg * (float)M_PI / 180.0f, fs = sinf(t), fc = cosf(t);
    lv_point_precise_t p[4];
    for (int k = 0; k < 4; k++) {
        const float rt = shape[k][0] * scale, fw = shape[k][1] * scale;
        p[k].x = (lv_value_precise_t)lroundf(x + fw * fs + rt * fc);
        p[k].y = (lv_value_precise_t)lroundf(y - fw * fc + rt * fs);
    }
    lv_draw_triangle_dsc_t d;
    lv_draw_triangle_dsc_init(&d);
    d.color = lv_color_hex(color);
    d.p[0] = p[0];
    d.p[1] = p[1];
    d.p[2] = p[2];
    lv_draw_triangle(layer, &d);
    d.p[1] = p[2];
    d.p[2] = p[3];
    lv_draw_triangle(layer, &d);
}

// The radar in miniature, and the time left as an arc round it, running
// clockwise from the top and retreating back to it
static void on_emblem_draw(lv_event_t *e)
{
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t a;
    lv_obj_get_coords(lv_event_get_target(e), &a);
    const int cx = (a.x1 + a.x2) / 2, cy = (a.y1 + a.y2) / 2, r = EMBLEM_R;

    lv_draw_rect_dsc_t disc;
    lv_draw_rect_dsc_init(&disc);
    disc.radius = LV_RADIUS_CIRCLE;
    disc.bg_color = lv_color_hex(C_DISC);
    disc.bg_opa = LV_OPA_COVER;
    lv_area_t da = {cx - r, cy - r, cx + r, cy + r};
    lv_draw_rect(layer, &disc, &da);
    ring(layer, cx, cy, r, C_TICK, 2);
    ring(layer, cx, cy, r / 2, C_RING, 1);
    line(layer, cx - r, cy, cx + r, cy, C_RING, 1, LV_OPA_COVER);
    line(layer, cx, cy - r, cx, cy + r, C_RING, 1, LV_OPA_COVER);

    // The sweep at 045, fading behind it
    for (int k = 0; k < 6; k++) {
        const float t = (45.0f - k * 6.0f) * (float)M_PI / 180.0f;
        line(layer, cx, cy, cx + (int)(r * sinf(t)), cy - (int)(r * cosf(t)), C_SWEEP, 3, LV_OPA_COVER - k * 40);
    }

    const float s = r / 90.0f;
    arrow(layer, cx + (int)(0.48f * r), cy - (int)(0.30f * r), 60, C_LOW, s);
    arrow(layer, cx - (int)(0.42f * r), cy + (int)(0.40f * r), 310, C_MID, s);
    arrow(layer, cx - (int)(0.20f * r), cy - (int)(0.62f * r), 160, C_HIGH, s);
    line(layer, cx - r / 10, cy, cx + r / 10, cy, C_HOME, 2, LV_OPA_COVER);
    line(layer, cx, cy - r / 10, cx, cy + r / 10, C_HOME, 2, LV_OPA_COVER);

    const float left = 1.0f - (esp_timer_get_time() - s_started_us) / 1000.0f / SPLASH_MS;
    if (left > 0) {
        lv_draw_arc_dsc_t arc;
        lv_draw_arc_dsc_init(&arc);
        arc.color = lv_color_hex(C_SWEEP);
        arc.opa = LV_OPA_60;
        arc.width = ARC_W;
        arc.radius = r + ARC_GAP + ARC_W;
        arc.center.x = cx;
        arc.center.y = cy;
        // LVGL's angles run clockwise from three o'clock; the top is 270
        arc.start_angle = 270;
        arc.end_angle = 270 + (int)(left * 360.0f);
        arc.rounded = 1;
        lv_draw_arc(layer, &arc);
    }
}

static void on_tick(lv_timer_t *t)
{
    if (esp_timer_get_time() - s_started_us >= SPLASH_MS * 1000LL) {
        close_splash();
        return;
    }
    lv_obj_invalidate(lv_obj_get_child(s_root, 0)); // the emblem, for the arc
}

// The press dismisses it; the rest of the touch lands on it too, still
// there until the next refresh, so it can't change the range underneath
static void on_press(lv_event_t *e)
{
    close_splash();
}

static lv_obj_t *text(lv_obj_t *parent, const char *s, const lv_font_t *font, uint32_t color, int spacing)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_obj_set_style_text_letter_space(l, spacing, 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(l, s);
    return l;
}

void splash_show(void)
{
    if (s_root) return;
    s_started_us = esp_timer_get_time();

    s_root = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(s_root);
    lv_obj_set_size(s_root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_root, lv_color_hex(C_BG), 0);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, 0);
    lv_obj_set_scrollable(s_root, false);
    lv_obj_set_clickable(s_root, true);
    lv_obj_add_event_cb(s_root, on_press, LV_EVENT_PRESSED, NULL);
    lv_obj_set_flex_flow(s_root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_root, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(s_root, GAP, 0);

    lv_obj_t *emblem = lv_obj_create(s_root); // child 0, see on_tick()
    lv_obj_remove_style_all(emblem);
    lv_obj_set_size(emblem, EMBLEM_SIZE, EMBLEM_SIZE);
    lv_obj_set_clickable(emblem, false);
    lv_obj_add_event_cb(emblem, on_emblem_draw, LV_EVENT_DRAW_MAIN, NULL);
    lv_obj_set_style_margin_bottom(emblem, GAP, 0);

    text(s_root, "OVERHEAD", &F_TITLE, C_SWEEP, 6);
    text(s_root, "Live ADS-B radar", &F_LINE, C_TEXT, 1);
    s_status = text(s_root, "Starting", &F_LINE, C_DIM, 0);
    lv_obj_set_style_margin_top(s_status, GAP, 0);
    lv_obj_t *tap = text(s_root, "TAP TO START", &F_SMALL, C_DIM, 2);
    lv_obj_set_style_margin_top(tap, GAP * 2, 0);
    char fw[64];
    lv_snprintf(fw, sizeof(fw), "Firmware %s  " LV_SYMBOL_BULLET "  %s", esp_app_get_description()->version,
                BOARD_NAME);
    lv_obj_t *ver = text(s_root, fw, &F_SMALL, C_DIM, 0);
    lv_label_set_long_mode(ver, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(ver, LV_PCT(70));

    s_timer = lv_timer_create(on_tick, TICK_MS, NULL);
}

void splash_hide(void)
{
    close_splash();
}

void splash_set_status(const char *text)
{
    if (s_status && text && text[0] && strcmp(lv_label_get_text(s_status), text) != 0) {
        lv_label_set_text(s_status, text);
    }
}
