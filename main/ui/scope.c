// The scope: aircraft projected onto it, airspace and airfields under them,
// trails, the drawing, the background cache and the sweep

#include "radar_priv.h"
#include "bench.h"
#include "geo.h"

// OpenAIP layers, by aero_kind_t: chart blues for controlled airspace, reds
// for restricted. Thin and translucent so traffic stays on top.
static const struct {
    uint32_t color;
    uint8_t width;
    lv_opa_t opa;
} AERO_STYLE[AERO_KIND_COUNT + 1] = {
    [AERO_ZONE] = {0x4d8fe0, 2, LV_OPA_80},
    [AERO_AREA] = {0x3a6db0, 1, LV_OPA_70},
    [AERO_RESTRICTED] = {0xd04848, 2, LV_OPA_80},
    [AERO_DANGER] = {0xc8783c, 1, LV_OPA_80},
    [AERO_MANDATORY] = {0x8a72c8, 1, LV_OPA_70},
    [AERO_KIND_COUNT] = {C_FIELD, 1, LV_OPA_70}, // SEG_CENTRELINE
};
// Extended runway centrelines go in with the outlines, as their own kind
#define SEG_CENTRELINE AERO_KIND_COUNT
#define CL_DASHES 9      // a dash per NM, from 1 NM out to 9.5
#define CL_MAX_RANGE 50  // NM; at 100 the dashes would be specks
#define FIELD_R_MAJOR (R_SCOPE / 50)
#define FIELD_R_MINOR (R_SCOPE / 75)
#define RUNWAY_HALF (R_SCOPE / 30)
#define AERO_DECIMATE_PX 2.0f
#define AERO_SEGS_PER_PASS 96 // about 17 KB of internal RAM in flight; see render_background()

// Airspace outlines as screen segments already clipped to the scope, built
// again only when the range or the data changes
typedef struct {
    int16_t x1, y1, x2, y2;
    uint8_t kind;
} aero_seg_t;
aero_map_t *s_aero;
static aero_seg_t *s_segs;
static int s_n_segs, s_cap_segs;
int s_segs_range = -1; // range index they were built for; -1: stale

// Everything under the traffic, drawn once per range into a PSRAM image:
// redrawing airspace outlines every second cost 100-400 ms a frame
static lv_obj_t *s_bg_canvas;
static lv_draw_buf_t *s_bg_buf;
int s_bg_range = -1; // range index it shows; -1: stale

lv_obj_t *s_sweep[SWEEP_SEGS];
lv_timer_t *s_sweep_timer;
static lv_point_precise_t s_sweep_pts[SWEEP_SEGS][2];
static float s_sweep_deg;
static int16_t (*s_trail_xy)[TRAIL_PTS][2]; // per aircraft, screen points

// ---------------------------------------------------------------- layout

static int cmp_by_dist(const void *pa, const void *pb)
{
    float a = s_scr[*(const int *)pa].dist_nm;
    float b = s_scr[*(const int *)pb].dist_nm;
    return (a > b) - (a < b);
}

// Clip a-b, relative to the scope centre, to the scope's circle. False when
// none of it is inside.
static bool clip_to_scope(float *ax, float *ay, float *bx, float *by)
{
    const float r = R_SCOPE;
    const float dx = *bx - *ax, dy = *by - *ay;
    const float a = dx * dx + dy * dy;
    const float b = 2 * (*ax * dx + *ay * dy);
    const float c = *ax * *ax + *ay * *ay - r * r;
    if (a < 0.25f) return false;
    const float disc = b * b - 4 * a * c;
    if (disc <= 0) return false;
    const float sq = sqrtf(disc);
    const float t0 = LV_MAX((-b - sq) / (2 * a), 0.0f), t1 = LV_MIN((-b + sq) / (2 * a), 1.0f);
    if (t0 >= t1) return false;
    const float x0 = *ax + dx * t0, y0 = *ay + dy * t0;
    *bx = *ax + dx * t1;
    *by = *ay + dy * t1;
    *ax = x0;
    *ay = y0;
    return true;
}

// One more segment, scope-relative, if any of it is inside the scope
static bool add_seg(float ax, float ay, float bx, float by, uint8_t kind)
{
    if (!clip_to_scope(&ax, &ay, &bx, &by)) return true;
    if (s_n_segs == s_cap_segs) {
        int cap = s_cap_segs ? s_cap_segs * 2 : 1024;
        aero_seg_t *grown = heap_caps_realloc(s_segs, cap * sizeof(*grown), MALLOC_CAP_SPIRAM);
        if (!grown) return false;
        s_segs = grown;
        s_cap_segs = cap;
    }
    s_segs[s_n_segs++] = (aero_seg_t){
        .x1 = (int16_t)lroundf(CX + ax), .y1 = (int16_t)lroundf(CY + ay),
        .x2 = (int16_t)lroundf(CX + bx), .y2 = (int16_t)lroundf(CY + by),
        .kind = kind,
    };
    return true;
}

// Dashed lines out along the main runway of each major airfield, both ways,
// a dash a mile: where arrivals line up and departures climb out. OpenAIP
// gives the runway's heading but not its thresholds, so they run through
// the airfield's reference point, between any parallel runways.
static void build_centrelines(float ppn)
{
    const int nm = RANGES[s_range_idx].nm;
    for (int i = 0; i < s_aero->n_fields; i++) {
        const aero_field_t *f = &s_aero->fields[i];
        if (!f->major || f->runway_deg < 0) continue;
        if (f->x * f->x + f->y * f->y > (float)(nm + CL_DASHES + 1) * (nm + CL_DASHES + 1)) continue;
        const float fx = f->x * ppn, fy = -f->y * ppn;
        const float ux = sinf(f->runway_deg * DEG2RAD) * ppn, uy = -cosf(f->runway_deg * DEG2RAD) * ppn;
        for (int dir = -1; dir <= 1; dir += 2) {
            for (int k = 1; k <= CL_DASHES; k++) {
                const float d0 = dir * (float)k, d1 = dir * (k + 0.5f);
                if (!add_seg(fx + ux * d0, fy + uy * d0, fx + ux * d1, fy + uy * d1, SEG_CENTRELINE)) return;
            }
        }
    }
}

static void build_aero_segments(void)
{
    s_n_segs = 0;
    s_segs_range = s_range_idx;
    if (!s_aero) return;
    const settings_t *set = settings();
    const float ppn = (float)R_SCOPE / RANGES[s_range_idx].nm;
    if (set->show_airfields && set->show_centrelines && RANGES[s_range_idx].nm <= CL_MAX_RANGE) {
        build_centrelines(ppn); // first, so the outlines draw over them
    }
    if (!set->show_airspace) return;

    for (int i = 0; i < s_aero->n_spaces; i++) {
        const aero_space_t *sp = &s_aero->spaces[i];
        const aero_pt_t *p = &s_aero->pts[sp->first];
        float lx = p[0].x * ppn, ly = -p[0].y * ppn;
        // Points within a couple of pixels of the last one add nothing at
        // this range; the ring is always closed back to its first point
        for (int k = 1; k <= sp->n; k++) {
            const aero_pt_t *q = &p[k % sp->n];
            const float x = q->x * ppn, y = -q->y * ppn;
            if (k < sp->n && fabsf(x - lx) < AERO_DECIMATE_PX && fabsf(y - ly) < AERO_DECIMATE_PX) continue;
            const float ax = lx, ay = ly;
            lx = x;
            ly = y;
            if (!add_seg(ax, ay, x, y, sp->kind)) return;
        }
    }
}

// A trail's points on screen, dropping those too close together or outside
// the scope; the newest end joins the aircraft's symbol when drawn.
static void layout_trail(int i, float ppn)
{
    ac_screen_t *s = &s_scr[i];
    trail_pt_t pts[TRAIL_PTS];
    const int n = trails_get(s_trail_slot[i], pts);
    int16_t(*xy)[2] = s_trail_xy[i];
    int m = 0;
    for (int j = 0; j < n; j++) {
        const float dx = (float)lon_diff(pts[j].lon, s_lon0) * 60.0f * s_coslat0 * ppn;
        const float dy = (pts[j].lat - s_lat0) * 60.0f * ppn;
        if (dx * dx + dy * dy > (float)R_SCOPE * R_SCOPE) continue;
        const int x = CX + (int)lroundf(dx), y = CY - (int)lroundf(dy);
        if (m && abs(x - xy[m - 1][0]) < TRAIL_MIN_PX && abs(y - xy[m - 1][1]) < TRAIL_MIN_PX) continue;
        xy[m][0] = x;
        xy[m][1] = y;
        m++;
    }
    // The last report is often within a few pixels of the symbol
    if (m && abs(s->x - xy[m - 1][0]) < TRAIL_MIN_PX && abs(s->y - xy[m - 1][1]) < TRAIL_MIN_PX) m--;
    s->trail_n = m;
    s->trail_box = (lv_area_t){s->x, s->y, s->x, s->y};
    for (int j = 0; j < m; j++) {
        s->trail_box.x1 = LV_MIN(s->trail_box.x1, xy[j][0]);
        s->trail_box.y1 = LV_MIN(s->trail_box.y1, xy[j][1]);
        s->trail_box.x2 = LV_MAX(s->trail_box.x2, xy[j][0]);
        s->trail_box.y2 = LV_MAX(s->trail_box.y2, xy[j][1]);
    }
}

// Project every aircraft to screen space, moving it forward along its track
// since its last position report. Runs once a second, not per frame.
void layout(void)
{
    const range_t *r = &RANGES[s_range_idx];
    const float ppn = (float)R_SCOPE / r->nm;
    const int64_t now = esp_timer_get_time();
    const settings_t *set = settings();
    s_order_n = 0;
    s_visible = 0;
    s_in_range = 0;
    s_alert_n = 0;
    s_alert_best = -1;
    s_emerg_n = 0;
    s_emerg_best = -1;
    if (s_segs_range != s_range_idx) build_aero_segments();

    for (int i = 0; i < s_count; i++) {
        const aircraft_t *a = &s_ac[i];
        ac_screen_t *s = &s_scr[i];
        s->visible = s->labelled = s->alert = false;
        s->trail_n = 0;
        if (a->on_ground && !set->show_ground) continue;

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
        double dx = lon_diff(lon, s_lon0) * 60.0 * s_coslat0;
        double dy = (lat - s_lat0) * 60.0;
        s->dist_nm = (float)hypot(dx, dy);
        s->brg_deg = (float)fmod(atan2(dx, dy) * 180.0 / M_PI + 360.0, 360.0);
        if (s->dist_nm > r->nm) continue;
        s_in_range++;
        // An emergency shows whatever the filter says
        if (!a->emergency && filtered_out(a, set)) continue;

        s->x = (int16_t)lroundf(CX + dx * ppn);
        s->y = (int16_t)lroundf(CY - dy * ppn);
        s->visible = true;
        s_visible++;

        if (set->alert && !a->on_ground && a->alt_ft != AC_ALT_UNKNOWN && a->alt_ft <= set->alert_ft &&
            s->dist_nm * 10 <= set->alert_nm10) {
            s->alert = true;
            if (s_alert_best < 0 || s->dist_nm < s_scr[s_alert_best].dist_nm) s_alert_best = i;
            s_alert_n++;
            // A new emergency on the big view isn't pushed aside by traffic overhead
            if (note_alert(s_alerted, a, now) && !(s_big_auto_emerg && s_big_auto[0])) big_auto(a->hex, false);
        }
        if (a->emergency && set->emerg_alert) {
            if (s_emerg_best < 0 || s->dist_nm < s_scr[s_emerg_best].dist_nm) s_emerg_best = i;
            s_emerg_n++;
            if (note_alert(s_emerg_alerted, a, now)) big_auto(a->hex, true);
        }

        bool flagged = is_selected(a) || a->emergency || s->alert;
        s->labelled = flagged;
        // Ground traffic would swamp an airport-centred view, so it only
        // gets a tag or a list row at the closest range
        if (flagged || !a->on_ground || s_range_idx == 0) {
            s_order[s_order_n++] = i;
        }
    }

    // Label the nearest aircraft first so busy airspace stays readable
    qsort(s_order, s_order_n, sizeof(*s_order), cmp_by_dist);
    for (int k = 0; k < s_order_n && k < set->max_labels; k++) {
        s_scr[s_order[k]].labelled = true;
    }

    // Trails for the nearest airborne aircraft, and any flagged further out
    if (s_trail_xy && set->trail_s > 0) {
        for (int k = 0, drawn = 0; k < s_order_n; k++) {
            const int i = s_order[k];
            const aircraft_t *a = &s_ac[i];
            if (a->on_ground) continue;
            const bool flagged = is_selected(a) || a->emergency || s_scr[i].alert;
            if (drawn >= TRAIL_MAX_AC && !flagged) continue;
            layout_trail(i, ppn);
            drawn++;
        }
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
void draw_arrow_scaled(lv_layer_t *layer, int x, int y, float track_rad, lv_color_t color, float scale)
{
    static const float shape[4][2] = {{0, 11}, {-7, -7}, {0, -3}, {7, -7}}; // {right, forward}
    const float fs = sinf(track_rad), fc = cosf(track_rad);
    lv_point_precise_t p[4];
    for (int k = 0; k < 4; k++) {
        float rt = shape[k][0] * scale, fw = shape[k][1] * scale;
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

static void draw_arrow(lv_layer_t *layer, int x, int y, float track_rad, lv_color_t color)
{
    draw_arrow_scaled(layer, x, y, track_rad, color, ARROW_SCALE);
}

// Rotorcraft: a body, a rotor crossed at 45 degrees to the track and a tail
// boom behind, so it reads differently from the fixed-wing arrowhead.
static void draw_heli(lv_layer_t *layer, int x, int y, float track_rad, lv_color_t color)
{
    const float rotor = 9.0f * ARROW_SCALE, tail = 11.0f * ARROW_SCALE;
    for (int k = 0; k < 2; k++) {
        const float a = track_rad + (k ? -1.0f : 1.0f) * (float)M_PI / 4;
        const int dx = (int)lroundf(rotor * sinf(a)), dy = (int)lroundf(rotor * cosf(a));
        draw_line(layer, x - dx, y + dy, x + dx, y - dy, color, 2, LV_OPA_COVER);
    }
    draw_line(layer, x, y, x - (int)lroundf(tail * sinf(track_rad)), y + (int)lroundf(tail * cosf(track_rad)),
              color, 2, LV_OPA_COVER);
    draw_dot(layer, x, y, (int)lroundf(4.0f * ARROW_SCALE), color);
}

// Military: a diamond around the symbol, apart from the emergency and
// selection rings
static void draw_diamond(lv_layer_t *layer, int x, int y, int r, lv_color_t color)
{
    draw_line(layer, x, y - r, x + r, y, color, 2, LV_OPA_COVER);
    draw_line(layer, x + r, y, x, y + r, color, 2, LV_OPA_COVER);
    draw_line(layer, x, y + r, x - r, y, color, 2, LV_OPA_COVER);
    draw_line(layer, x - r, y, x, y - r, color, 2, LV_OPA_COVER);
}

// The tag's two lines, each from the fields chosen on the configuration page:
// callsign and type, then altitude in hundreds of feet (as a controller's
// radar reads) with a climb or descent arrow, and ground speed.
static void format_tag(const aircraft_t *a, char *buf, size_t n)
{
    const int f = settings()->tag_fields;
    char l1[24] = "", l2[24] = "";
    if (f & TAG_CALLSIGN) strlcpy(l1, ac_name(a), sizeof(l1));
    if ((f & TAG_TYPE) && a->type[0]) {
        if (l1[0]) strlcat(l1, " ", sizeof(l1));
        strlcat(l1, a->type, sizeof(l1));
    }
    if (f & TAG_ALT) {
        if (a->on_ground) {
            strlcpy(l2, "GND", sizeof(l2));
        } else if (a->alt_ft == AC_ALT_UNKNOWN) {
            strlcpy(l2, "---", sizeof(l2));
        } else {
            const char *trend = a->vs_fpm > 300 ? " " LV_SYMBOL_UP : a->vs_fpm < -300 ? " " LV_SYMBOL_DOWN : "";
            snprintf(l2, sizeof(l2), "%03d%s", (int)(a->alt_ft + 50) / 100, trend);
        }
    }
    if ((f & TAG_SPEED) && a->gs_kt >= 0 && !a->on_ground) {
        char gs[12];
        snprintf(gs, sizeof(gs), "%s%.0fkt", l2[0] ? "  " : "", a->gs_kt);
        strlcat(l2, gs, sizeof(l2));
    }
    // Nothing chosen for the first line (only speed, say): the name, so
    // the tag still says whose it is
    if (!l1[0] && !l2[0]) strlcpy(l1, ac_name(a), sizeof(l1));
    snprintf(buf, n, "%s%s%s", l1, l1[0] && l2[0] ? "\n" : "", l2);
}

static void draw_trails(lv_layer_t *layer)
{
    for (int i = 0; i < s_count; i++) {
        const ac_screen_t *s = &s_scr[i];
        if (!s->visible || !s->trail_n) continue;
        const lv_area_t *b = &s->trail_box;
        if (!in_clip(layer, b->x1 - 2, b->y1 - 2, b->x2 + 2, b->y2 + 2)) continue;
        const lv_color_t c = alt_color(&s_ac[i]);
        const int16_t(*xy)[2] = s_trail_xy[i];
        // Fading out with age
        for (int j = 0; j < s->trail_n; j++) {
            const int x2 = j + 1 < s->trail_n ? xy[j + 1][0] : s->x;
            const int y2 = j + 1 < s->trail_n ? xy[j + 1][1] : s->y;
            const lv_opa_t opa = LV_OPA_10 + (LV_OPA_60 - LV_OPA_10) * (j + 1) / s->trail_n;
            draw_line(layer, xy[j][0], xy[j][1], x2, y2, c, 2, opa);
        }
    }
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
        if (!s->visible || !in_clip(layer, s->x - 80, s->y - 80, s->x + LV_MAX(80, TAG_DX + TAG_W), s->y + 80)) continue;
        const aircraft_t *a = &s_ac[i];
        const lv_color_t c = alt_color(a);
        const bool sel = is_selected(a);

        const bool moving = a->gs_kt > 0 && a->track_deg >= 0;
        const float t = moving ? a->track_deg * DEG2RAD : 0;
        if (moving && !a->on_ground && settings()->show_vector) {
            // One-minute velocity vector, clamped so jets don't streak across the 5 NM view
            float len = fminf(a->gs_kt / 60.0f * ppn, (float)VECTOR_MAX);
            draw_line(layer, s->x, s->y, s->x + (int)(len * sinf(t)), s->y - (int)(len * cosf(t)), c, 2,
                      LV_OPA_50);
        }
        if (a->heli) {
            draw_heli(layer, s->x, s->y, t, c);
        } else if (a->on_ground) {
            draw_dot(layer, s->x, s->y, 3, c);
        } else if (moving) {
            draw_arrow(layer, s->x, s->y, t, c);
        } else {
            draw_dot(layer, s->x, s->y, 5, c);
        }

        if (is_military(a)) draw_diamond(layer, s->x, s->y, SEL_RING - 2, lv_color_hex(C_MIL));
        if (sel || a->emergency) {
            draw_ring(layer, s->x, s->y, SEL_RING, lv_color_hex(sel ? C_SELECT : C_EMERG), 2);
        }
        if (s->alert && s_blink) draw_ring(layer, s->x, s->y, SEL_RING + 6, lv_color_hex(C_WARN), 3);

        if (s->labelled) {
            format_tag(a, buf, sizeof(buf));
            draw_text(layer, s->x + TAG_DX, s->y - TAG_DY, TAG_W, buf, &F_TAG,
                      sel ? lv_color_hex(C_SELECT) : c, LV_TEXT_ALIGN_LEFT);
        }
    }
}

// Under the traffic: airspace outlines, then airfields as a ring crossed by
// the main runway. Smaller fields and the labels come in at closer ranges.
static void draw_airspace(lv_layer_t *layer, int from, int to)
{
    for (int i = from; i < to; i++) {
        const aero_seg_t *g = &s_segs[i];
        draw_line(layer, g->x1, g->y1, g->x2, g->y2, lv_color_hex(AERO_STYLE[g->kind].color),
                  AERO_STYLE[g->kind].width, AERO_STYLE[g->kind].opa);
    }
}

static void draw_airfields(lv_layer_t *layer)
{
    if (!s_aero || !settings()->show_airfields) return;

    const int nm = RANGES[s_range_idx].nm;
    const float ppn = (float)R_SCOPE / nm;
    const lv_color_t color = lv_color_hex(C_FIELD);
    const int line_h = lv_font_get_line_height(&F_RING);
    for (int i = 0; i < s_aero->n_fields; i++) {
        const aero_field_t *f = &s_aero->fields[i];
        if (!f->major && nm > 25) continue;
        const float fx = f->x * ppn, fy = -f->y * ppn;
        const int r = f->major ? FIELD_R_MAJOR : FIELD_R_MINOR;
        if (fx * fx + fy * fy > (float)(R_SCOPE - r) * (R_SCOPE - r)) continue;
        const int x = CX + (int)lroundf(fx), y = CY + (int)lroundf(fy);
        if (!in_clip(layer, x - 4 * r, y - RUNWAY_HALF, x + 100, y + RUNWAY_HALF)) continue;

        draw_ring(layer, x, y, r, color, f->major ? 2 : 1);
        if (f->runway_deg >= 0) {
            const float rs = sinf(f->runway_deg * DEG2RAD) * RUNWAY_HALF;
            const float rc = cosf(f->runway_deg * DEG2RAD) * RUNWAY_HALF;
            draw_line(layer, x - (int)rs, y + (int)rc, x + (int)rs, y - (int)rc, color, 2, LV_OPA_COVER);
        }
        if (f->ident[0] && nm <= (f->major ? 50 : 10)) {
            draw_text(layer, x + RUNWAY_HALF + 2, y - line_h / 2, 110, f->ident, &F_RING, color,
                      LV_TEXT_ALIGN_LEFT);
        }
    }
}

void on_scope_draw(lv_event_t *e)
{
#if CONFIG_LV_USE_SNAPSHOT
    const int64_t started = esp_timer_get_time();
#endif
    lv_layer_t *layer = lv_event_get_layer(e);
    if (s_bg_buf && s_bg_range == s_range_idx) {
        lv_draw_image_dsc_t d;
        lv_draw_image_dsc_init(&d);
        d.src = s_bg_buf;
        lv_area_t a = {0, 0, SCR_W - 1, SCR_W - 1};
        lv_draw_image(layer, &d, &a);
    } else {
        draw_scope(layer);
        draw_airspace(layer, 0, s_n_segs);
        draw_airfields(layer);
    }
    draw_trails(layer);
    draw_aircraft(layer);
#if CONFIG_LV_USE_SNAPSHOT
    bench_note_scope_draw(esp_timer_get_time() - started);
#endif
}

// Not from a draw callback: it renders, and renders don't nest
void render_background(void)
{
    if (!s_bg_buf) return;
    if (s_segs_range != s_range_idx) build_aero_segments();
    lv_canvas_fill_bg(s_bg_canvas, lv_color_hex(C_BG), LV_OPA_COVER);

    // In passes, each finished before the next: LVGL queues every draw
    // task of a layer before running any, each about 180 bytes of internal
    // RAM, and 4,000 outline segments at once drained it to ~1 KB
    lv_layer_t layer;
    lv_canvas_init_layer(s_bg_canvas, &layer);
    draw_scope(&layer);
    lv_canvas_finish_layer(s_bg_canvas, &layer);
    for (int from = 0; from < s_n_segs; from += AERO_SEGS_PER_PASS) {
        lv_canvas_init_layer(s_bg_canvas, &layer);
        draw_airspace(&layer, from, LV_MIN(from + AERO_SEGS_PER_PASS, s_n_segs));
        lv_canvas_finish_layer(s_bg_canvas, &layer);
    }
    lv_canvas_init_layer(s_bg_canvas, &layer);
    draw_airfields(&layer); // on top of the outlines
    lv_canvas_finish_layer(s_bg_canvas, &layer);
    s_bg_range = s_range_idx;
}

void create_background(lv_obj_t *scr)
{
    s_bg_buf = lv_draw_buf_create(SCR_W, SCR_W, lv_display_get_color_format(NULL), LV_STRIDE_AUTO);
    if (!s_bg_buf) return; // then the scope draws it all every frame
    s_bg_canvas = lv_canvas_create(scr);
    lv_canvas_set_draw_buf(s_bg_canvas, s_bg_buf);
    lv_obj_set_hidden(s_bg_canvas, true); // only its buffer is used
}

void show_sweep(bool on)
{
    for (int i = 0; i < SWEEP_SEGS; i++) lv_obj_set_hidden(s_sweep[i], !on);
    if (on) {
        lv_timer_resume(s_sweep_timer);
    } else {
        lv_timer_pause(s_sweep_timer);
    }
}

// The history and its screen points, once trails are on
void start_trails(void)
{
    if (settings()->trail_s <= 0) return;
    trails_init();
    if (!s_trail_xy) s_trail_xy = heap_caps_calloc(AC_MAX, sizeof(*s_trail_xy), MALLOC_CAP_SPIRAM);
}

// Each segment is its own small line object so a frame only redraws the
// thin band the sweep actually crosses, not one huge bounding box.
void on_sweep(lv_timer_t *t)
{
    if (radar_hidden()) return;
#if CONFIG_LV_USE_SNAPSHOT
    bench_note_sweep();
#endif
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

