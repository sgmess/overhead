#include "radar_ui.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "bsp/display.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "lvgl.h"

// The scope is the top SCR_W x SCR_W square: the whole panel on the round
// boards, the upper part of the Tab5's portrait 720x1280 with the traffic
// list below it.
#define SCR_W BSP_LCD_H_RES
#define CX (SCR_W / 2)
#define CY (SCR_W / 2)
#define DEG2RAD ((float)M_PI / 180.0f)

// Sizes per board. They follow physical size, not resolution: pixel pitch is
// 0.107 mm on the P4 3.4C, 0.148 mm on the 2.8C and 0.0865 mm on the Tab5.
#if defined(OVERHEAD_BOARD_TAB5) // 720x1280, scope 720x720 on top
#define HAS_LIST 1
#define R_SCOPE 306
#define TAP_RADIUS 48
#define TICK_LEN_30 16
#define TICK_LEN_10 10
#define TICK_LEN_5 5
#define ROSE_TEXT_R (R_SCOPE + 32)
#define ARROW_SCALE 1.2f
#define VECTOR_MAX 80
#define SEL_RING 22
#define HOME_ARM 11
#define TAG_DX 15
#define TAG_DY 11
#define TAG_W 150
#define CLOCK_Y 18
#define RANGE_Y 50
#define STATUS_Y 24
#define CARD_W (SCR_W - 2 * PANEL_PAD)
#define CARD_H 200
#define CARD_PAD 16
#define CARD_SUB_Y 40
#define CARD_ROWS_Y 76
#define CARD_LINE_SPACE 6
#define CARD_COLS {0, 70, 340, 410}
#define F_CARDINAL lv_font_montserrat_24
#define F_ROSE lv_font_montserrat_16
#define F_RING lv_font_montserrat_14
#define F_TAG lv_font_montserrat_16
#define F_CLOCK lv_font_montserrat_24
#define F_RANGE lv_font_montserrat_32
#define F_STATUS lv_font_montserrat_16
#define F_TITLE lv_font_montserrat_32
#define F_CARD lv_font_montserrat_20
#define F_LIST lv_font_montserrat_20
#define F_LIST_HEAD lv_font_montserrat_14
#define PANEL_PAD 16
#define LIST_ROWS 11
#define ROW_H 44
#define LIST_COLS {0, 170, 270, 390, 490, 600}
#elif defined(OVERHEAD_BOARD_P4_34C) // 800x800 round
#define R_SCOPE 350
#define TAP_RADIUS 40
#define TICK_LEN_30 14
#define TICK_LEN_10 8
#define TICK_LEN_5 4
#define ROSE_TEXT_R (R_SCOPE + 32)
#define ARROW_SCALE 1.0f
#define VECTOR_MAX 70
#define SEL_RING 18
#define HOME_ARM 9
#define TAG_DX 13
#define TAG_DY 10
#define TAG_W 130
#define CLOCK_Y 16
#define RANGE_Y 42
#define STATUS_Y 20
#define CARD_W 464
#define CARD_H 236
#define CARD_PAD 18
#define CARD_SUB_Y 38
#define CARD_ROWS_Y 76
#define CARD_LINE_SPACE 8
#define CARD_COLS {0, 48, 210, 256}
#define F_CARDINAL lv_font_montserrat_20
#define F_ROSE lv_font_montserrat_14
#define F_RING lv_font_montserrat_12
#define F_TAG lv_font_montserrat_14
#define F_CLOCK lv_font_montserrat_20
#define F_RANGE lv_font_montserrat_28
#define F_STATUS lv_font_montserrat_14
#define F_TITLE lv_font_montserrat_28
#define F_CARD lv_font_montserrat_16
#else // S3 2.8C, 480x480 round
#define R_SCOPE 206
#define TAP_RADIUS 30
#define TICK_LEN_30 9
#define TICK_LEN_10 5
#define TICK_LEN_5 3
#define ROSE_TEXT_R (R_SCOPE + 20)
#define ARROW_SCALE 0.75f
#define VECTOR_MAX 45
#define SEL_RING 14
#define HOME_ARM 7
#define TAG_DX 10
#define TAG_DY 8
#define TAG_W 100
#define CLOCK_Y 10
#define RANGE_Y 28
#define STATUS_Y 12
#define CARD_W 320
#define CARD_H 170
#define CARD_PAD 12
#define CARD_SUB_Y 26
#define CARD_ROWS_Y 52
#define CARD_LINE_SPACE 4
#define CARD_COLS {0, 38, 148, 184}
#define F_CARDINAL lv_font_montserrat_16
#define F_ROSE lv_font_montserrat_12
#define F_RING lv_font_montserrat_12
#define F_TAG lv_font_montserrat_12
#define F_CLOCK lv_font_montserrat_16
#define F_RANGE lv_font_montserrat_20
#define F_STATUS lv_font_montserrat_12
#define F_TITLE lv_font_montserrat_20
#define F_CARD lv_font_montserrat_14
#endif

#define SWEEP_SEGS 8
#define SWEEP_PERIOD_MS 4000
#define SWEEP_TICK_MS 33
#define MAX_EXTRAPOLATE_S 60
#define DROP_STALE_S 90

#ifdef CONFIG_OVERHEAD_SHOW_GROUND
#define SHOW_GROUND true
#else
#define SHOW_GROUND false
#endif

typedef struct {
    int nm;
    int ring_nm;
} range_t;

static const range_t RANGES[] = {{5, 1}, {10, 2}, {25, 5}, {50, 10}, {100, 25}};
#define RANGE_COUNT ((int)(sizeof(RANGES) / sizeof(RANGES[0])))

#define C_BG 0x030806
#define C_DISC 0x06120d
#define C_RING 0x1c4535
#define C_TICK 0x3d7a61
#define C_TEXT 0x7cc9a4
#define C_DIM 0x4b8a6d
#define C_SWEEP 0x46ffa8
#define C_HOME 0xe8fff4
#define C_GROUND 0x5f6b66
#define C_UNKNOWN 0xb0c4bc
#define C_EMERG 0xff3b3b
#define C_SELECT 0xffffff
#define C_WARN 0xffb020

typedef struct {
    int16_t x, y;
    float dist_nm;
    float brg_deg;
    bool visible;
    bool labelled;
} ac_screen_t;

static aircraft_t *s_ac;
static ac_screen_t *s_scr;
static int s_count;
static int s_visible;
static int s_order[AC_MAX]; // visible, taggable aircraft, nearest first
static int s_order_n;
static double s_lat0, s_lon0, s_coslat0;
static volatile int s_range_idx = CONFIG_OVERHEAD_DEFAULT_RANGE_INDEX;
static char s_sel_hex[8];
static int64_t s_last_update_us;
static char s_feed[16];
static char s_status[64];
static void (*s_on_range_change)(void);

static lv_obj_t *s_scope, *s_clock, *s_range_lbl, *s_status_lbl;
static lv_obj_t *s_card, *s_card_title, *s_card_sub, *s_card_k1, *s_card_v1, *s_card_k2, *s_card_v2;
#ifdef CONFIG_OVERHEAD_SWEEP
static lv_obj_t *s_sweep[SWEEP_SEGS];
static lv_point_precise_t s_sweep_pts[SWEEP_SEGS][2];
static float s_sweep_deg;
#endif
#ifdef HAS_LIST
static lv_obj_t *s_rows[LIST_ROWS];
static lv_obj_t *s_cells[LIST_ROWS][6];
static char s_row_hex[LIST_ROWS][8];
static lv_obj_t *s_batt_level, *s_batt_detail;
#endif

// ---------------------------------------------------------------- helpers

static lv_color_t alt_color(const aircraft_t *a)
{
    if (a->emergency) return lv_color_hex(C_EMERG);
    if (a->on_ground) return lv_color_hex(C_GROUND);
    if (a->alt_ft == AC_ALT_UNKNOWN) return lv_color_hex(C_UNKNOWN);

    // Warm and low to cool and high, blended between stops
    static const struct {
        int ft;
        uint32_t rgb;
    } stops[] = {{0, 0xffb020}, {10000, 0xa3e635}, {20000, 0x22d3ee}, {30000, 0x60a5fa}, {40000, 0xc084fc}};
    const int n = sizeof(stops) / sizeof(stops[0]);
    if (a->alt_ft <= stops[0].ft) return lv_color_hex(stops[0].rgb);
    for (int i = 1; i < n; i++) {
        if (a->alt_ft < stops[i].ft) {
            int t = (a->alt_ft - stops[i - 1].ft) * 255 / (stops[i].ft - stops[i - 1].ft);
            return lv_color_mix(lv_color_hex(stops[i].rgb), lv_color_hex(stops[i - 1].rgb), t);
        }
    }
    return lv_color_hex(stops[n - 1].rgb);
}

static void fmt_thousands(char *buf, size_t n, int v)
{
    if (v >= 1000 || v <= -1000) {
        snprintf(buf, n, "%d,%03d", v / 1000, abs(v % 1000));
    } else {
        snprintf(buf, n, "%d", v);
    }
}

static int find_aircraft(const char *hex)
{
    for (int i = 0; i < s_count; i++) {
        if (strcmp(s_ac[i].hex, hex) == 0) return i;
    }
    return -1;
}

static bool is_selected(const aircraft_t *a)
{
    return s_sel_hex[0] && strcmp(a->hex, s_sel_hex) == 0;
}

// ---------------------------------------------------------------- layout

static int cmp_by_dist(const void *pa, const void *pb)
{
    float a = s_scr[*(const int *)pa].dist_nm;
    float b = s_scr[*(const int *)pb].dist_nm;
    return (a > b) - (a < b);
}

// Project every aircraft to screen space, moving it forward along its track
// since its last position report. Runs once a second, not per frame.
static void layout(void)
{
    const range_t *r = &RANGES[s_range_idx];
    const float ppn = (float)R_SCOPE / r->nm;
    const int64_t now = esp_timer_get_time();
    s_order_n = 0;
    s_visible = 0;

    for (int i = 0; i < s_count; i++) {
        const aircraft_t *a = &s_ac[i];
        ac_screen_t *s = &s_scr[i];
        s->visible = s->labelled = false;
        if (a->on_ground && !SHOW_GROUND) continue;

        double lat = a->lat, lon = a->lon;
        if (!a->on_ground && a->gs_kt > 0 && a->track_deg >= 0) {
            double dt = (now - a->pos_time_us) / 1e6;
            if (dt > MAX_EXTRAPOLATE_S) dt = MAX_EXTRAPOLATE_S;
            if (dt > 0) {
                double d_nm = a->gs_kt * dt / 3600.0;
                double t = a->track_deg * (M_PI / 180.0);
                lat += d_nm * cos(t) / 60.0;
                lon += d_nm * sin(t) / (60.0 * cos(lat * (M_PI / 180.0)));
            }
        }

        // Flat-earth projection is well under a pixel out at 100 NM
        double dx = (lon - s_lon0) * 60.0 * s_coslat0;
        double dy = (lat - s_lat0) * 60.0;
        s->dist_nm = (float)hypot(dx, dy);
        s->brg_deg = (float)fmod(atan2(dx, dy) * 180.0 / M_PI + 360.0, 360.0);
        if (s->dist_nm > r->nm) continue;

        s->x = (int16_t)lroundf(CX + dx * ppn);
        s->y = (int16_t)lroundf(CY - dy * ppn);
        s->visible = true;
        s_visible++;

        bool flagged = is_selected(a) || a->emergency;
        s->labelled = flagged;
        // Ground traffic would swamp an airport-centred view, so it only
        // gets a tag or a list row at the closest range
        if (flagged || !a->on_ground || s_range_idx == 0) {
            s_order[s_order_n++] = i;
        }
    }

    // Label the nearest aircraft first so busy airspace stays readable
    qsort(s_order, s_order_n, sizeof(s_order[0]), cmp_by_dist);
    for (int k = 0; k < s_order_n && k < CONFIG_OVERHEAD_MAX_LABELS; k++) {
        s_scr[s_order[k]].labelled = true;
    }
}

// ---------------------------------------------------------------- drawing

static bool in_clip(const lv_layer_t *layer, int x1, int y1, int x2, int y2)
{
    const lv_area_t *c = &layer->_clip_area;
    return !(x2 < c->x1 || x1 > c->x2 || y2 < c->y1 || y1 > c->y2);
}

static void draw_line(lv_layer_t *layer, int x1, int y1, int x2, int y2,
                      lv_color_t color, int width, lv_opa_t opa)
{
    if (!in_clip(layer, LV_MIN(x1, x2) - width, LV_MIN(y1, y2) - width,
                 LV_MAX(x1, x2) + width, LV_MAX(y1, y2) + width)) {
        return;
    }
    lv_draw_line_dsc_t d;
    lv_draw_line_dsc_init(&d);
    d.p1.x = x1;
    d.p1.y = y1;
    d.p2.x = x2;
    d.p2.y = y2;
    d.color = color;
    d.width = width;
    d.opa = opa;
    d.round_start = d.round_end = 1;
    lv_draw_line(layer, &d);
}

static void draw_ring(lv_layer_t *layer, int x, int y, int radius, lv_color_t color, int width)
{
    lv_draw_arc_dsc_t d;
    lv_draw_arc_dsc_init(&d);
    d.center.x = x;
    d.center.y = y;
    d.radius = radius;
    d.start_angle = 0;
    d.end_angle = 360;
    d.width = width;
    d.color = color;
    lv_draw_arc(layer, &d);
}

static void draw_dot(lv_layer_t *layer, int x, int y, int radius, lv_color_t color)
{
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.radius = LV_RADIUS_CIRCLE;
    d.bg_color = color;
    d.bg_opa = LV_OPA_COVER;
    lv_area_t a = {x - radius, y - radius, x + radius, y + radius};
    lv_draw_rect(layer, &d, &a);
}

static void draw_text(lv_layer_t *layer, int x, int y, int w, const char *text,
                      const lv_font_t *font, lv_color_t color, lv_text_align_t align)
{
    int h = lv_font_get_line_height(font) * 2;
    if (!in_clip(layer, x, y, x + w, y + h)) return;
    lv_draw_label_dsc_t d;
    lv_draw_label_dsc_init(&d);
    d.text = text;
    d.text_local = 1; // draw tasks run later; copy the stack buffer
    d.font = font;
    d.color = color;
    d.align = align;
    lv_area_t a = {x, y, x + w - 1, y + h - 1};
    lv_draw_label(layer, &d, &a);
}

// Arrowhead pointing along the track: two triangles sharing a notched tail.
static void draw_arrow(lv_layer_t *layer, int x, int y, float track_rad, lv_color_t color)
{
    static const float shape[4][2] = {{0, 11}, {-7, -7}, {0, -3}, {7, -7}}; // {right, forward}
    const float fs = sinf(track_rad), fc = cosf(track_rad);
    lv_point_precise_t p[4];
    for (int k = 0; k < 4; k++) {
        float rt = shape[k][0] * ARROW_SCALE, fw = shape[k][1] * ARROW_SCALE;
        p[k].x = (lv_value_precise_t)lroundf(x + fw * fs + rt * fc);
        p[k].y = (lv_value_precise_t)lroundf(y - fw * fc + rt * fs);
    }
    lv_draw_triangle_dsc_t d;
    lv_draw_triangle_dsc_init(&d);
    d.color = color;
    d.opa = LV_OPA_COVER;
    d.p[0] = p[0];
    d.p[1] = p[1];
    d.p[2] = p[2];
    lv_draw_triangle(layer, &d);
    d.p[1] = p[2];
    d.p[2] = p[3];
    lv_draw_triangle(layer, &d);
}

static void draw_scope(lv_layer_t *layer)
{
    const range_t *r = &RANGES[s_range_idx];
    char buf[8];

    lv_draw_rect_dsc_t disc;
    lv_draw_rect_dsc_init(&disc);
    disc.radius = LV_RADIUS_CIRCLE;
    disc.bg_color = lv_color_hex(C_DISC);
    disc.bg_opa = LV_OPA_COVER;
    lv_area_t da = {CX - R_SCOPE, CY - R_SCOPE, CX + R_SCOPE, CY + R_SCOPE};
    lv_draw_rect(layer, &disc, &da);

    // Dashed crosshair
    lv_draw_line_dsc_t cross;
    lv_draw_line_dsc_init(&cross);
    cross.color = lv_color_hex(C_RING);
    cross.width = 1;
    cross.dash_width = 3;
    cross.dash_gap = 7;
    cross.p1.x = CX - R_SCOPE;
    cross.p1.y = CY;
    cross.p2.x = CX + R_SCOPE;
    cross.p2.y = CY;
    lv_draw_line(layer, &cross);
    cross.p1.x = CX;
    cross.p1.y = CY - R_SCOPE;
    cross.p2.x = CX;
    cross.p2.y = CY + R_SCOPE;
    lv_draw_line(layer, &cross);

    // Range rings, labelled along the 060 radial
    for (int nm = r->ring_nm; nm <= r->nm; nm += r->ring_nm) {
        int rad = R_SCOPE * nm / r->nm;
        bool outer = nm == r->nm;
        draw_ring(layer, CX, CY, rad, lv_color_hex(outer ? C_TICK : C_RING), outer ? 2 : 1);
        if (!outer) {
            snprintf(buf, sizeof(buf), "%d", nm);
            draw_text(layer, CX + (int)(rad * 0.866f) + 4, CY - rad / 2 - 18, 40, buf,
                      &F_RING, lv_color_hex(C_DIM), LV_TEXT_ALIGN_LEFT);
        }
    }

    // Compass rose outside the scope
    for (int deg = 0; deg < 360; deg += 5) {
        int len = deg % 30 == 0 ? TICK_LEN_30 : deg % 10 == 0 ? TICK_LEN_10 : TICK_LEN_5;
        float s = sinf(deg * DEG2RAD), c = cosf(deg * DEG2RAD);
        int r0 = R_SCOPE + 3, r1 = R_SCOPE + 3 + len;
        draw_line(layer, CX + (int)(r0 * s), CY - (int)(r0 * c), CX + (int)(r1 * s), CY - (int)(r1 * c),
                  lv_color_hex(C_TICK), deg % 30 == 0 ? 2 : 1, LV_OPA_COVER);
    }
    static const char *const CARD[] = {"N", "03", "06", "E", "12", "15", "S", "21", "24", "W", "30", "33"};
    for (int k = 0; k < 12; k++) {
        bool cardinal = k % 3 == 0;
        const lv_font_t *f = cardinal ? &F_CARDINAL : &F_ROSE;
        float a = k * 30 * DEG2RAD;
        int rad = ROSE_TEXT_R;
        int lh = lv_font_get_line_height(f);
        draw_text(layer, CX + (int)(rad * sinf(a)) - 20, CY - (int)(rad * cosf(a)) - lh / 2, 40, CARD[k], f,
                  lv_color_hex(cardinal ? C_TEXT : C_DIM), LV_TEXT_ALIGN_CENTER);
    }

    // Home position
    draw_line(layer, CX - HOME_ARM, CY, CX + HOME_ARM, CY, lv_color_hex(C_HOME), 2, LV_OPA_COVER);
    draw_line(layer, CX, CY - HOME_ARM, CX, CY + HOME_ARM, lv_color_hex(C_HOME), 2, LV_OPA_COVER);
}

static void draw_aircraft(lv_layer_t *layer)
{
    const float ppn = (float)R_SCOPE / RANGES[s_range_idx].nm;
    char buf[40];

    for (int i = 0; i < s_count; i++) {
        const ac_screen_t *s = &s_scr[i];
        if (!s->visible || !in_clip(layer, s->x - 80, s->y - 80, s->x + 140, s->y + 80)) continue;
        const aircraft_t *a = &s_ac[i];
        const lv_color_t c = alt_color(a);
        const bool sel = is_selected(a);

        if (a->on_ground) {
            draw_dot(layer, s->x, s->y, 3, c);
        } else if (a->gs_kt > 0 && a->track_deg >= 0) {
            // One-minute velocity vector, clamped so jets don't streak across the 5 NM view
            float t = a->track_deg * DEG2RAD;
            float len = fminf(a->gs_kt / 60.0f * ppn, (float)VECTOR_MAX);
            draw_line(layer, s->x, s->y, s->x + (int)(len * sinf(t)), s->y - (int)(len * cosf(t)), c, 2,
                      LV_OPA_50);
            draw_arrow(layer, s->x, s->y, t, c);
        } else {
            draw_dot(layer, s->x, s->y, 5, c);
        }

        if (sel || a->emergency) {
            draw_ring(layer, s->x, s->y, SEL_RING, lv_color_hex(sel ? C_SELECT : C_EMERG), 2);
        }

        if (s->labelled) {
            const char *trend = a->vs_fpm > 300 ? " " LV_SYMBOL_UP : a->vs_fpm < -300 ? " " LV_SYMBOL_DOWN : "";
            if (a->on_ground) {
                snprintf(buf, sizeof(buf), "%s\nGND", ac_name(a));
            } else if (a->alt_ft == AC_ALT_UNKNOWN) {
                snprintf(buf, sizeof(buf), "%s\n---", ac_name(a));
            } else {
                // Hundreds of feet, the way a controller's radar tags read
                snprintf(buf, sizeof(buf), "%s\n%03d%s", ac_name(a), (int)(a->alt_ft + 50) / 100, trend);
            }
            draw_text(layer, s->x + TAG_DX, s->y - TAG_DY, TAG_W, buf, &F_TAG,
                      sel ? lv_color_hex(C_SELECT) : c, LV_TEXT_ALIGN_LEFT);
        }
    }
}

static void on_scope_draw(lv_event_t *e)
{
    lv_layer_t *layer = lv_event_get_layer(e);
    draw_scope(layer);
    draw_aircraft(layer);
}

// ---------------------------------------------------------------- overlay

static void update_range_label(void)
{
    lv_label_set_text_fmt(s_range_lbl, "%d NM", RANGES[s_range_idx].nm);
}

static void update_status_label(void)
{
    if (s_status[0]) {
        lv_label_set_text(s_status_lbl, s_status);
        return;
    }
    if (!s_last_update_us) {
        lv_label_set_text(s_status_lbl, "Waiting for data");
        return;
    }
    int age = (int)((esp_timer_get_time() - s_last_update_us) / 1000000);
    if (age > 3 * CONFIG_OVERHEAD_FETCH_INTERVAL_SEC + 10) {
        lv_label_set_text_fmt(s_status_lbl, "%d aircraft  " LV_SYMBOL_BULLET "  %s  " LV_SYMBOL_BULLET "  %ds old",
                              s_visible, s_feed, age);
    } else {
        lv_label_set_text_fmt(s_status_lbl, "%d aircraft  " LV_SYMBOL_BULLET "  %s", s_visible, s_feed);
    }
}

static void update_clock(void)
{
    time_t now = time(NULL);
    struct tm tm;
    gmtime_r(&now, &tm);
    if (tm.tm_year < 120) { // not synced yet
        lv_label_set_text(s_clock, "--:--:--Z");
    } else {
        lv_label_set_text_fmt(s_clock, "%02d:%02d:%02dZ", tm.tm_hour, tm.tm_min, tm.tm_sec);
    }
}

static void update_card(void)
{
    int i = s_sel_hex[0] ? find_aircraft(s_sel_hex) : -1;
    if (i < 0) {
        s_sel_hex[0] = '\0';
        lv_obj_set_hidden(s_card, true);
        return;
    }
    const aircraft_t *a = &s_ac[i];
    const ac_screen_t *s = &s_scr[i];
    char alt[24], vs[24], num[16];

    lv_label_set_text(s_card_title, ac_name(a));

    char sub[48] = "";
    if (a->reg[0] && strcmp(a->reg, ac_name(a)) != 0) strlcat(sub, a->reg, sizeof(sub));
    if (a->type[0]) {
        if (sub[0]) strlcat(sub, "  " LV_SYMBOL_BULLET "  ", sizeof(sub));
        strlcat(sub, a->type, sizeof(sub));
    }
    if (sub[0]) strlcat(sub, "  " LV_SYMBOL_BULLET "  ", sizeof(sub));
    strlcat(sub, a->hex, sizeof(sub));
    lv_label_set_text(s_card_sub, sub);

    if (a->on_ground) {
        strlcpy(alt, "Ground", sizeof(alt));
    } else if (a->alt_ft == AC_ALT_UNKNOWN) {
        strlcpy(alt, "-", sizeof(alt));
    } else {
        fmt_thousands(num, sizeof(num), a->alt_ft);
        snprintf(alt, sizeof(alt), "%s ft", num);
    }
    if (a->vs_fpm == 0 || a->on_ground) {
        strlcpy(vs, "Level", sizeof(vs));
    } else {
        fmt_thousands(num, sizeof(num), a->vs_fpm);
        snprintf(vs, sizeof(vs), "%s%s fpm", a->vs_fpm > 0 ? "+" : "", num);
    }

    char gs[16] = "-", trk[16] = "-";
    if (a->gs_kt >= 0) snprintf(gs, sizeof(gs), "%.0f kt", a->gs_kt);
    if (a->track_deg >= 0) snprintf(trk, sizeof(trk), "%03.0f\xC2\xB0", a->track_deg);

    lv_label_set_text_fmt(s_card_v1, "%s\n%s\n%s%s", alt, gs, a->squawk[0] ? a->squawk : "-",
                          a->emergency ? "  " LV_SYMBOL_WARNING : "");
    lv_label_set_text_fmt(s_card_v2, "%s\n%s\n%.1f NM  %03.0f\xC2\xB0", vs, trk, s->dist_nm, s->brg_deg);
    lv_obj_set_hidden(s_card, false);
}

#ifdef HAS_LIST
// Setting a label's text redraws it even when nothing changed; at 1 Hz across
// a whole table that is most of the panel, so only touch what changed.
static void set_text_if_changed(lv_obj_t *label, const char *text)
{
    if (strcmp(lv_label_get_text(label), text) != 0) lv_label_set_text(label, text);
}

static void update_list(void)
{
    static bool row_sel[LIST_ROWS];
    static lv_color_t row_color[LIST_ROWS];
    char buf[24];

    for (int r = 0; r < LIST_ROWS; r++) {
        if (r >= s_order_n) {
            s_row_hex[r][0] = '\0';
            lv_obj_set_hidden(s_rows[r], true);
            continue;
        }
        const aircraft_t *a = &s_ac[s_order[r]];
        const ac_screen_t *s = &s_scr[s_order[r]];
        const bool sel = is_selected(a);
        const lv_color_t color = sel ? lv_color_hex(C_SELECT) : alt_color(a);
        strlcpy(s_row_hex[r], a->hex, sizeof(s_row_hex[r]));
        lv_obj_set_hidden(s_rows[r], false);

        if (sel != row_sel[r]) {
            lv_obj_set_style_bg_opa(s_rows[r], sel ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
            row_sel[r] = sel;
        }
        if (!lv_color_eq(color, row_color[r])) {
            lv_obj_set_style_text_color(s_cells[r][0], color, 0);
            row_color[r] = color;
        }

        set_text_if_changed(s_cells[r][0], ac_name(a));
        set_text_if_changed(s_cells[r][1], a->type[0] ? a->type : "-");
        if (a->on_ground) {
            strlcpy(buf, "GND", sizeof(buf));
        } else if (a->alt_ft == AC_ALT_UNKNOWN) {
            strlcpy(buf, "---", sizeof(buf));
        } else {
            const char *trend = a->vs_fpm > 300 ? " " LV_SYMBOL_UP : a->vs_fpm < -300 ? " " LV_SYMBOL_DOWN : "";
            snprintf(buf, sizeof(buf), "%03d%s", (int)(a->alt_ft + 50) / 100, trend);
        }
        set_text_if_changed(s_cells[r][2], buf);
        if (a->gs_kt >= 0) {
            snprintf(buf, sizeof(buf), "%.0f", a->gs_kt);
        } else {
            strlcpy(buf, "-", sizeof(buf));
        }
        set_text_if_changed(s_cells[r][3], buf);
        snprintf(buf, sizeof(buf), "%.1f", s->dist_nm);
        set_text_if_changed(s_cells[r][4], buf);
        snprintf(buf, sizeof(buf), "%03.0f\xC2\xB0", s->brg_deg);
        set_text_if_changed(s_cells[r][5], buf);
    }
}
#endif

static void refresh(void)
{
    layout();
    update_card();
#ifdef HAS_LIST
    update_list();
#endif
    update_status_label();
    lv_obj_invalidate(s_scope);
}

static void select_aircraft(const char *hex)
{
    if (hex) {
        strlcpy(s_sel_hex, hex, sizeof(s_sel_hex));
    } else {
        s_sel_hex[0] = '\0';
    }
    refresh();
}

static void on_scope_click(lv_event_t *e)
{
    lv_point_t p;
    lv_indev_get_point(lv_indev_active(), &p);

    int best = -1;
    int best_d2 = TAP_RADIUS * TAP_RADIUS;
    for (int i = 0; i < s_count; i++) {
        if (!s_scr[i].visible) continue;
        int dx = s_scr[i].x - p.x, dy = s_scr[i].y - p.y;
        int d2 = dx * dx + dy * dy;
        if (d2 < best_d2) {
            best_d2 = d2;
            best = i;
        }
    }

    if (best >= 0) {
        select_aircraft(s_ac[best].hex);
    } else if (s_sel_hex[0]) {
        select_aircraft(NULL);
    } else {
        s_range_idx = (s_range_idx + 1) % RANGE_COUNT;
        update_range_label();
        refresh();
        if (s_on_range_change) s_on_range_change();
    }
}

static void on_card_click(lv_event_t *e)
{
    select_aircraft(NULL);
}

#ifdef HAS_LIST
// Tapping a row selects that aircraft; tapping the selected row clears it.
static void on_row_click(lv_event_t *e)
{
    int r = (int)(intptr_t)lv_event_get_user_data(e);
    if (!s_row_hex[r][0]) return;
    select_aircraft(strcmp(s_row_hex[r], s_sel_hex) == 0 ? NULL : s_row_hex[r]);
}
#endif

static void on_tick(lv_timer_t *t)
{
    update_clock();
    if (s_count && esp_timer_get_time() - s_last_update_us > DROP_STALE_S * 1000000LL) {
        s_count = 0; // don't keep extrapolating ghosts forever
    }
    refresh();
}

#ifdef CONFIG_OVERHEAD_SWEEP
// Each segment is its own small line object so a frame only redraws the
// thin band the sweep actually crosses, not one huge bounding box.
static void on_sweep(lv_timer_t *t)
{
    s_sweep_deg = fmodf(s_sweep_deg + 360.0f * SWEEP_TICK_MS / SWEEP_PERIOD_MS, 360.0f);
    const float sx = sinf(s_sweep_deg * DEG2RAD), sy = -cosf(s_sweep_deg * DEG2RAD);
    for (int i = 0; i < SWEEP_SEGS; i++) {
        int x0 = CX + (int)(R_SCOPE * i / SWEEP_SEGS * sx), y0 = CY + (int)(R_SCOPE * i / SWEEP_SEGS * sy);
        int x1 = CX + (int)(R_SCOPE * (i + 1) / SWEEP_SEGS * sx), y1 = CY + (int)(R_SCOPE * (i + 1) / SWEEP_SEGS * sy);
        int ox = LV_MIN(x0, x1) - 2, oy = LV_MIN(y0, y1) - 2;
        s_sweep_pts[i][0].x = x0 - ox;
        s_sweep_pts[i][0].y = y0 - oy;
        s_sweep_pts[i][1].x = x1 - ox;
        s_sweep_pts[i][1].y = y1 - oy;
        lv_obj_set_pos(s_sweep[i], ox, oy);
        lv_line_set_points(s_sweep[i], s_sweep_pts[i], 2);
    }
}
#endif

static lv_obj_t *make_label(lv_obj_t *parent, const lv_font_t *font, uint32_t color)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_label_set_text(l, "");
    return l;
}

static void create_card(lv_obj_t *parent)
{
    s_card = lv_obj_create(parent);
    lv_obj_set_size(s_card, CARD_W, CARD_H);
#ifndef HAS_LIST
    // Over the lower half of the scope; on the Tab5 the panel places it
    lv_obj_align(s_card, LV_ALIGN_CENTER, 0, SCR_W * 18 / 100);
#endif
    lv_obj_set_style_bg_color(s_card, lv_color_hex(0x0a1813), 0);
    lv_obj_set_style_bg_opa(s_card, LV_OPA_90, 0);
    lv_obj_set_style_border_color(s_card, lv_color_hex(C_TICK), 0);
    lv_obj_set_style_border_width(s_card, 2, 0);
    lv_obj_set_style_radius(s_card, CARD_PAD + 6, 0);
    lv_obj_set_style_pad_all(s_card, CARD_PAD, 0);
    lv_obj_set_scrollable(s_card, false);
    lv_obj_add_event_cb(s_card, on_card_click, LV_EVENT_CLICKED, NULL);

    s_card_title = make_label(s_card, &F_TITLE, C_SELECT);
    s_card_sub = make_label(s_card, &F_CARD, C_TEXT);
    lv_obj_set_pos(s_card_sub, 0, CARD_SUB_Y);

    s_card_k1 = make_label(s_card, &F_CARD, C_DIM);
    s_card_v1 = make_label(s_card, &F_CARD, C_SELECT);
    s_card_k2 = make_label(s_card, &F_CARD, C_DIM);
    s_card_v2 = make_label(s_card, &F_CARD, C_SELECT);
    lv_obj_t *cols[] = {s_card_k1, s_card_v1, s_card_k2, s_card_v2};
    const int xs[] = CARD_COLS;
    for (int k = 0; k < 4; k++) {
        lv_obj_set_pos(cols[k], xs[k], CARD_ROWS_Y);
        lv_obj_set_style_text_line_space(cols[k], CARD_LINE_SPACE, 0);
    }
    lv_label_set_text(s_card_k1, "ALT\nGS\nSQK");
    lv_label_set_text(s_card_k2, "V/S\nTRK\nRNG");

    lv_obj_set_hidden(s_card, true);
}

#ifdef HAS_LIST
static lv_obj_t *make_box(lv_obj_t *parent)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_scrollable(o, false);
    lv_obj_set_clickable(o, false);
    return o;
}

// The panel below the scope: the details card on top while something is
// selected, then the nearest aircraft. Rows that don't fit are clipped.
static void create_panel(lv_obj_t *scr)
{
    lv_obj_t *panel = make_box(scr);
    lv_obj_set_pos(panel, 0, SCR_W);
    lv_obj_set_size(panel, SCR_W, BSP_LCD_V_RES - SCR_W);
    lv_obj_set_style_pad_all(panel, PANEL_PAD, 0);
    lv_obj_set_style_pad_row(panel, 12, 0);
    lv_obj_set_style_border_side(panel, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(C_RING), 0);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);

    create_card(panel); // flex skips it while hidden, so the list moves up

    lv_obj_t *list = make_box(panel);
    lv_obj_set_width(list, LV_PCT(100));
    lv_obj_set_flex_grow(list, 1);

    static const char *const HEAD[] = {"CALLSIGN", "TYPE", "ALT", "GS KT", "NM", "BRG"};
    const int xs[] = LIST_COLS;
    for (int k = 0; k < 6; k++) {
        lv_obj_t *h = make_label(list, &F_LIST_HEAD, C_DIM);
        lv_label_set_text(h, HEAD[k]);
        lv_obj_set_pos(h, xs[k] + 8, 0);
    }

    const int head_h = lv_font_get_line_height(&F_LIST_HEAD) + 6;
    for (int r = 0; r < LIST_ROWS; r++) {
        lv_obj_t *row = make_box(list);
        lv_obj_set_pos(row, 0, head_h + r * ROW_H);
        lv_obj_set_size(row, LV_PCT(100), ROW_H);
        lv_obj_set_style_radius(row, 8, 0);
        lv_obj_set_style_bg_color(row, lv_color_hex(0x0e2a20), 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_set_clickable(row, true);
        lv_obj_add_event_cb(row, on_row_click, LV_EVENT_CLICKED, (void *)(intptr_t)r);
        for (int k = 0; k < 6; k++) {
            s_cells[r][k] = make_label(row, &F_LIST, k == 0 ? C_SELECT : C_TEXT);
            lv_obj_align(s_cells[r][k], LV_ALIGN_LEFT_MID, xs[k] + 8, 0);
        }
        lv_obj_set_hidden(row, true);
        s_rows[r] = row;
    }
}
#endif

// ---------------------------------------------------------------- public

void radar_ui_create(double center_lat, double center_lon, void (*on_range_change)(void))
{
    s_ac = heap_caps_calloc(AC_MAX, sizeof(aircraft_t), MALLOC_CAP_SPIRAM);
    s_scr = heap_caps_calloc(AC_MAX, sizeof(ac_screen_t), MALLOC_CAP_SPIRAM);
    s_lat0 = center_lat;
    s_lon0 = center_lon;
    s_coslat0 = cos(center_lat * M_PI / 180.0);
    s_on_range_change = on_range_change;

    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(C_BG), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_scrollable(scr, false);

    s_scope = lv_obj_create(scr);
    lv_obj_remove_style_all(s_scope);
    lv_obj_set_size(s_scope, SCR_W, SCR_W);
    lv_obj_set_clickable(s_scope, true);
    lv_obj_add_event_cb(s_scope, on_scope_draw, LV_EVENT_DRAW_MAIN, NULL);
    lv_obj_add_event_cb(s_scope, on_scope_click, LV_EVENT_CLICKED, NULL);

#ifdef CONFIG_OVERHEAD_SWEEP
    for (int i = 0; i < SWEEP_SEGS; i++) {
        s_sweep[i] = lv_line_create(scr);
        lv_obj_set_style_line_color(s_sweep[i], lv_color_hex(C_SWEEP), 0);
        lv_obj_set_style_line_width(s_sweep[i], 3, 0);
        // Brighter towards the rim, like a phosphor trace
        lv_obj_set_style_line_opa(s_sweep[i], 40 + 170 * i / (SWEEP_SEGS - 1), 0);
        lv_obj_set_clickable(s_sweep[i], false);
    }
    on_sweep(NULL);
    lv_timer_create(on_sweep, SWEEP_TICK_MS, NULL);
#endif

    // Children of the scope, so they sit against its edges on every board.
    // Labels aren't clickable, so taps still reach the scope.
    s_clock = make_label(s_scope, &F_CLOCK, C_TEXT);
    lv_obj_align(s_clock, LV_ALIGN_TOP_MID, 0, CY - R_SCOPE + CLOCK_Y);
    s_range_lbl = make_label(s_scope, &F_RANGE, C_TEXT);
    lv_obj_align(s_range_lbl, LV_ALIGN_BOTTOM_MID, 0, -(CY - R_SCOPE + RANGE_Y));
    s_status_lbl = make_label(s_scope, &F_STATUS, C_DIM);
    lv_obj_align(s_status_lbl, LV_ALIGN_BOTTOM_MID, 0, -(CY - R_SCOPE + STATUS_Y));

#ifdef HAS_LIST
    create_panel(scr);

    // Battery in the scope square's top-right corner, outside the circle.
    // Hidden until the first reading, so it never shows on a board without one.
    s_batt_level = make_label(s_scope, &F_CLOCK, C_TEXT);
    lv_obj_align(s_batt_level, LV_ALIGN_TOP_RIGHT, -18, 14);
    s_batt_detail = make_label(s_scope, &F_STATUS, C_DIM);
    lv_obj_align(s_batt_detail, LV_ALIGN_TOP_RIGHT, -18, 14 + lv_font_get_line_height(&F_CLOCK));
    lv_obj_set_hidden(s_batt_level, true);
    lv_obj_set_hidden(s_batt_detail, true);
#else
    create_card(scr);
#endif

    update_clock();
    update_range_label();
    refresh();
    lv_timer_create(on_tick, 1000, NULL);
}

void radar_ui_set_aircraft(const aircraft_t *list, int count, const char *feed)
{
    if (count > AC_MAX) count = AC_MAX;
    memcpy(s_ac, list, count * sizeof(aircraft_t));
    s_count = count;
    s_last_update_us = esp_timer_get_time();
    strlcpy(s_feed, feed, sizeof(s_feed));
    refresh();
}

void radar_ui_set_status(const char *text)
{
    strlcpy(s_status, text ? text : "", sizeof(s_status));
    update_status_label();
}

int radar_ui_range_nm(void)
{
    return RANGES[s_range_idx].nm;
}

void radar_ui_set_battery(const battery_status_t *st)
{
#ifdef HAS_LIST
    char detail[40];
    uint32_t color = C_TEXT;
    const char *icon;

    if (st->state == BATT_NO_PACK || st->state == BATT_CHECKING) {
        lv_label_set_text(s_batt_level, LV_SYMBOL_BATTERY_EMPTY);
        lv_label_set_text(s_batt_detail, st->state == BATT_NO_PACK ? "No battery" : "Checking battery");
        lv_obj_set_style_text_color(s_batt_level, lv_color_hex(C_DIM), 0);
        lv_obj_set_hidden(s_batt_level, false);
        lv_obj_set_hidden(s_batt_detail, false);
        return;
    }

    if (st->state == BATT_CHARGING) {
        icon = LV_SYMBOL_CHARGE;
    } else if (st->percent >= 80) {
        icon = LV_SYMBOL_BATTERY_FULL;
    } else if (st->percent >= 55) {
        icon = LV_SYMBOL_BATTERY_3;
    } else if (st->percent >= 30) {
        icon = LV_SYMBOL_BATTERY_2;
    } else if (st->percent >= 10) {
        icon = LV_SYMBOL_BATTERY_1;
    } else {
        icon = LV_SYMBOL_BATTERY_EMPTY;
    }

    if (st->shutdown_in_s > 0) {
        snprintf(detail, sizeof(detail), "Battery flat, off in %d s", st->shutdown_in_s);
        color = C_EMERG;
    } else if (st->state == BATT_CHARGING) {
        snprintf(detail, sizeof(detail), "Charging  %.2f A", -st->amps);
    } else if (st->state == BATT_EXTERNAL) {
        strlcpy(detail, "External power", sizeof(detail));
    } else if (st->minutes_left >= 0) {
        snprintf(detail, sizeof(detail), "~%dh %02dm left", st->minutes_left / 60, st->minutes_left % 60);
    } else {
        strlcpy(detail, "On battery", sizeof(detail));
    }
    if (st->state == BATT_DISCHARGING && st->low) {
        color = st->percent <= 5 ? C_EMERG : C_WARN;
    }

    // Percent is read off the voltage curve, so it is an estimate
    lv_label_set_text_fmt(s_batt_level, "%s  %d%%", icon, st->percent);
    lv_obj_set_style_text_color(s_batt_level, lv_color_hex(color), 0);
    lv_label_set_text(s_batt_detail, detail);
    lv_obj_set_style_text_color(s_batt_detail, lv_color_hex(st->shutdown_in_s > 0 ? C_EMERG : C_DIM), 0);
    lv_obj_set_hidden(s_batt_level, false);
    lv_obj_set_hidden(s_batt_detail, false);
#else
    (void)st;
#endif
}
