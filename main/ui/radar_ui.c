// The radar screen: the state its parts share, the overlay (clock, status,
// alert banner), the views and touch, the battery and setup panels, and the
// public API. The parts are scope.c, card.c, big_view.c, lists.c and
// describe.c; radar_priv.h is what they share.

#include "radar_priv.h"

// ---------------------------------------------------------------- state

aircraft_t *s_ac;
ac_screen_t *s_scr;
int16_t *s_trail_slot;               // per aircraft, from trails_update()
int s_count;
int s_visible;
int s_in_range; // visible, plus those the filter hides
int *s_order; // AC_MAX of them: visible, taggable aircraft, nearest first
int s_order_n;
double s_lat0, s_lon0, s_coslat0;
volatile int s_range_idx;
char s_sel_hex[8];
int s_view = VIEW_RADAR;
// One that has just started alerting, which the big view shows ahead of the
// selection until it stops or a tap moves on
char s_big_auto[8];
bool s_big_auto_emerg;
char s_big_hex[8]; // the one it shows
int64_t s_last_update_us;
static char s_feed[16];
static char s_status[64];
int s_alert_n, s_alert_best; // aircraft alerting, and the nearest
int s_emerg_n, s_emerg_best; // in an emergency, and the nearest
bool s_blink;                // flips every second, for the alert ring
// Recently alerting, so each chimes once: overhead and emergency apart, so
// one doesn't silence the other
alert_memory_t s_alerted[ALERT_MEMORY], s_emerg_alerted[ALERT_MEMORY];
static void (*s_on_range_change)(void);

static lv_obj_t *s_scope, *s_clock, *s_clock2, *s_range_lbl, *s_status_lbl, *s_alert_lbl;
static lv_obj_t *s_setup, *s_setup_text;
static lv_obj_t *s_sleep_shield; // over everything while the screen is off: takes the waking touch
#if LV_USE_QRCODE
static lv_obj_t *s_setup_qr;
#endif
#ifdef HAS_LIST
const panel_layout_t *s_panel = &PANEL_PORTRAIT;
static lv_obj_t *s_batt_row, *s_batt_level, *s_batt_body, *s_batt_fill, *s_batt_nub, *s_batt_detail;
static lv_obj_t *s_count_lbl, *s_feed_lbl; // bottom-right corner
#endif

// ---------------------------------------------------------------- helpers

lv_color_t alt_color(const aircraft_t *a)
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

void fmt_thousands(char *buf, size_t n, int v)
{
    if (v >= 1000 || v <= -1000) {
        snprintf(buf, n, "%d,%03d", v / 1000, abs(v % 1000));
    } else {
        snprintf(buf, n, "%d", v);
    }
}

int find_aircraft(const char *hex)
{
    for (int i = 0; i < s_count; i++) {
        if (strcmp(s_ac[i].hex, hex) == 0) return i;
    }
    return -1;
}

bool is_selected(const aircraft_t *a)
{
    return s_sel_hex[0] && strcmp(a->hex, s_sel_hex) == 0;
}

bool is_military(const aircraft_t *a)
{
    return a->military && settings()->show_military;
}

// Chime for an aircraft that has just started alerting, unless it was
// alerting a moment ago (wandering in and out at the edge, say)
bool note_alert(alert_memory_t *mem, const aircraft_t *a, int64_t now)
{
    bool fresh = false;
    int slot = -1, oldest = 0;
    for (int k = 0; k < ALERT_MEMORY; k++) {
        if (strcmp(mem[k].hex, a->hex) == 0) {
            slot = k;
            break;
        }
        if (mem[k].seen_us < mem[oldest].seen_us) oldest = k;
    }
    if (slot < 0 || now - mem[slot].seen_us > ALERT_REARM_S * 1000000LL) {
        fresh = true;
        if (settings()->alert_sound && !backlight_asleep()) board_chime(); // no chimes in the night
        if (slot < 0) {
            slot = oldest;
            strlcpy(mem[slot].hex, a->hex, sizeof(mem[slot].hex));
        }
    }
    mem[slot].seen_us = now;
    return fresh;
}

// The big view moves to an aircraft that has just started alerting
void big_auto(const char *hex, bool emerg)
{
    strlcpy(s_big_auto, hex, sizeof(s_big_auto));
    s_big_auto_emerg = emerg;
}

// Hidden by the display filter: not one of the kinds chosen, or airborne
// outside the altitude band. Unknown altitudes stay, as nothing says where.
bool filtered_out(const aircraft_t *a, const settings_t *set)
{
    if (!(a->kind & set->show_kinds)) return true;
    if (a->on_ground || a->alt_ft == AC_ALT_UNKNOWN) return false;
    return a->alt_ft < set->filter_min_ft ||
           (set->filter_max_ft < FILTER_NO_CEILING && a->alt_ft > set->filter_max_ft);
}
// ---------------------------------------------------------------- overlay

static void update_range_label(void)
{
    lv_label_set_text_fmt(s_range_lbl, "%d NM", RANGES[s_range_idx].nm);
}

// Setting a label's text redraws it even when nothing changed, so only touch
// what changed.
void set_text_if_changed(lv_obj_t *label, const char *text)
{
    if (strcmp(lv_label_get_text(label), text) != 0) lv_label_set_text(label, text);
}

// "12 aircraft", or "12 of 30 aircraft" while the filter hides some
void format_count(char *buf, size_t n)
{
    if (s_visible < s_in_range) {
        snprintf(buf, n, "%d of %d aircraft", s_visible, s_in_range);
    } else {
        snprintf(buf, n, "%d aircraft", s_visible);
    }
}

static void update_status_label(void)
{
    char text[96];
    int age = s_last_update_us ? (int)((esp_timer_get_time() - s_last_update_us) / 1000000) : 0;
    char stale[24] = "";
    if (age > 3 * settings()->fetch_s + 10) snprintf(stale, sizeof(stale), "%ds old", age);
#ifdef HAS_LIST
    // Count and feed in the bottom-right corner; under the scope only what
    // is going on (connecting, errors), and nothing once traffic flows
    if (s_last_update_us) {
        format_count(text, sizeof(text));
        set_text_if_changed(s_count_lbl, text);
        snprintf(text, sizeof(text), "%s%s%s", s_feed, stale[0] ? "  " LV_SYMBOL_BULLET "  " : "", stale);
        set_text_if_changed(s_feed_lbl, text);
    }
    lv_obj_set_hidden(s_count_lbl, !s_last_update_us);
    lv_obj_set_hidden(s_feed_lbl, !s_last_update_us);
    set_text_if_changed(s_status_lbl, s_status[0] ? s_status : s_last_update_us ? "" : "Waiting for data");
#else
    if (s_status[0]) {
        strlcpy(text, s_status, sizeof(text));
    } else if (!s_last_update_us) {
        strlcpy(text, "Waiting for data", sizeof(text));
    } else {
        char count[40];
        format_count(count, sizeof(count));
        snprintf(text, sizeof(text), "%s  " LV_SYMBOL_BULLET "  %s%s%s", count, s_feed,
                 stale[0] ? "  " LV_SYMBOL_BULLET "  " : "", stale);
    }
    set_text_if_changed(s_status_lbl, text);
#endif
}

// "13:04:52 BST", or with no timezone set "12:04:52Z"
static void format_time(char *buf, size_t n, time_t now, bool local, bool seconds)
{
    struct tm tm;
    if (local && settings()->tz[0]) {
        localtime_r(&now, &tm);
        strftime(buf, n, seconds ? "%H:%M:%S %Z" : "%H:%M %Z", &tm);
    } else {
        gmtime_r(&now, &tm);
        strftime(buf, n, seconds ? "%H:%M:%SZ" : "%H:%MZ", &tm);
    }
}

// UTC, local, or both: local large with UTC small underneath
static void update_clock(void)
{
    const int mode = settings()->clock;
    time_t now = time(NULL);
    struct tm tm;
    gmtime_r(&now, &tm);
    char buf[32];
    if (tm.tm_year < 120) { // not synced yet
        set_text_if_changed(s_clock, "--:--:--");
        lv_obj_set_hidden(s_clock2, true);
        return;
    }
    format_time(buf, sizeof(buf), now, mode != 0, true);
    set_text_if_changed(s_clock, buf);
    if (mode == 2 && settings()->tz[0]) {
        format_time(buf, sizeof(buf), now, false, false);
        set_text_if_changed(s_clock2, buf);
        lv_obj_set_hidden(s_clock2, false);
    } else {
        lv_obj_set_hidden(s_clock2, true);
    }
}

static const char *emerg_name(uint8_t e)
{
    switch (e) {
    case AC_EMERG_RADIO: return "RADIO FAILURE";
    case AC_EMERG_HIJACK: return "HIJACK";
    case AC_EMERG_MEDICAL: return "MEDICAL";
    case AC_EMERG_FUEL: return "LOW FUEL";
    case AC_EMERG_DOWNED: return "DOWNED";
    default: return "EMERGENCY";
    }
}

// The banner over the top of the scope: an emergency anywhere in range, in
// red, before anything overhead, in amber
static void update_alert(void)
{
    char more[16] = "", text[96];
    if (s_emerg_best >= 0) {
        const aircraft_t *a = &s_ac[s_emerg_best];
        const bool code = strcmp(a->squawk, "7500") == 0 || strcmp(a->squawk, "7600") == 0 ||
                          strcmp(a->squawk, "7700") == 0;
        if (s_emerg_n > 1) snprintf(more, sizeof(more), "  +%d", s_emerg_n - 1);
        snprintf(text, sizeof(text), LV_SYMBOL_WARNING "  %s%s%s  %s  %.1f NM%s", code ? a->squawk : "",
                 code ? " " : "", emerg_name(a->emerg), ac_name(a), s_scr[s_emerg_best].dist_nm, more);
        lv_obj_set_style_bg_color(s_alert_lbl, lv_color_hex(C_EMERG), 0);
        lv_obj_set_style_text_color(s_alert_lbl, lv_color_hex(C_SELECT), 0);
    } else if (s_alert_best >= 0) {
        const aircraft_t *a = &s_ac[s_alert_best];
        char alt[16];
        fmt_thousands(alt, sizeof(alt), a->alt_ft);
        if (s_alert_n > 1) snprintf(more, sizeof(more), "  +%d", s_alert_n - 1);
        snprintf(text, sizeof(text), LV_SYMBOL_BELL "  %s  %s ft  %.1f NM%s", ac_name(a), alt,
                 s_scr[s_alert_best].dist_nm, more);
        lv_obj_set_style_bg_color(s_alert_lbl, lv_color_hex(C_WARN), 0);
        lv_obj_set_style_text_color(s_alert_lbl, lv_color_hex(C_ALERT_INK), 0);
    } else {
        lv_obj_set_hidden(s_alert_lbl, true);
        return;
    }
    set_text_if_changed(s_alert_lbl, text);
    lv_obj_set_hidden(s_alert_lbl, false);
}

// ---------------------------------------------------------------- views and touch

static void refresh(void)
{
    layout();
    if (s_bg_range != s_range_idx) render_background();
    update_card();
    update_big(); // before the list, which marks the one it shows
#ifdef HAS_LIST
    update_list();
#endif
    update_status_label();
    update_alert();
#ifndef HAS_LIST
    update_rlist();
#endif
    if (s_view == VIEW_RADAR) lv_obj_invalidate(s_scope);
}

void select_aircraft(const char *hex)
{
    if (hex) {
        strlcpy(s_sel_hex, hex, sizeof(s_sel_hex));
    } else {
        s_sel_hex[0] = '\0';
    }
    refresh();
}

void radar_ui_select(const char *hex)
{
    select_aircraft(hex);
}


static void on_scope_click(lv_event_t *e)
{
    if (swiped()) return; // the end of a swipe, not a tap
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
    }
}

// Up or down a range, stopping at either end
static void step_range(int by)
{
    const int idx = s_range_idx + by;
    if (idx < 0 || idx >= RANGE_COUNT) return;
    s_range_idx = idx;
    update_range_label();
    refresh();
    if (s_on_range_change) s_on_range_change();
}

// Where the finger went down, so a long press that has moved, the start of
// a slow swipe, doesn't open the settings
static lv_point_t s_press_pt;

void on_press(lv_event_t *e)
{
    lv_indev_get_point(lv_indev_active(), &s_press_pt);
}

void on_scope_long_press(lv_event_t *e)
{
    lv_point_t p;
    lv_indev_get_point(lv_indev_active(), &p);
    const int dx = p.x - s_press_pt.x, dy = p.y - s_press_pt.y;
    if (swiped() || dx * dx + dy * dy > PRESS_SLOP * PRESS_SLOP) return;
    settings_ui_open();
}

// The scope, the big view or (on the round boards) the list, in the square
void show_view(int view)
{
    s_view = view;
    s_big_hex[0] = s_big_auto[0] = '\0';
    // Anything alerting is the first thing the big view shows
    if (s_emerg_best >= 0) {
        big_auto(s_ac[s_emerg_best].hex, true);
    } else if (s_alert_best >= 0) {
        big_auto(s_ac[s_alert_best].hex, false);
    }
    lv_obj_set_hidden(s_big, view != VIEW_BIG);
    lv_obj_set_hidden(s_scope, view != VIEW_RADAR); // and its drawing, every second, with it
#ifndef HAS_LIST
    lv_obj_set_hidden(s_rlist, view != VIEW_LIST);
#endif
    show_sweep(view == VIEW_RADAR && settings()->sweep);
    // The alert banner goes with whichever is showing: over the top of the
    // scope, or as the first line of the other views' columns
    lv_obj_t *parent = s_scope;
    if (view == VIEW_BIG) parent = s_big;
#ifndef HAS_LIST
    if (view == VIEW_LIST) parent = s_rlist;
#endif
    lv_obj_set_parent(s_alert_lbl, parent);
    if (view != VIEW_RADAR) {
        lv_obj_remove_flag(s_alert_lbl, LV_OBJ_FLAG_FLOATING);
        lv_obj_move_to_index(s_alert_lbl, 0);
    } else {
        lv_obj_add_flag(s_alert_lbl, LV_OBJ_FLAG_FLOATING);
    }
    refresh();
}

#if CONFIG_LV_USE_SNAPSHOT
void radar_ui_debug_view(int view)
{
    if (view >= 0 && view < VIEW_COUNT && view != s_view) show_view(view);
}
#endif

// A swipe also ends in a release, which mustn't count as a tap as well
bool swiped(void)
{
    return lv_indev_get_gesture_dir(lv_indev_active()) != LV_DIR_NONE;
}

void on_gesture(lv_event_t *e)
{
    const lv_dir_t d = lv_indev_get_gesture_dir(lv_indev_active());
    if (d == LV_DIR_LEFT) show_view((s_view + 1) % VIEW_COUNT);
    if (d == LV_DIR_RIGHT) show_view((s_view + VIEW_COUNT - 1) % VIEW_COUNT);
    // On the scope, up and down change the range: up to see further
    if (s_view == VIEW_RADAR && d == LV_DIR_TOP) step_range(1);
    if (s_view == VIEW_RADAR && d == LV_DIR_BOTTOM) step_range(-1);
}

static void on_alert_click(lv_event_t *e)
{
    const int i = s_emerg_best >= 0 ? s_emerg_best : s_alert_best;
    if (i >= 0) select_aircraft(s_ac[i].hex);
}

// The level for now, and the shield over the screen while it is off
static void update_backlight(void)
{
    const bool asleep = backlight_update(s_alert_n > 0 || s_emerg_n > 0, lv_display_get_inactive_time(NULL));
    lv_obj_set_hidden(s_sleep_shield, !asleep);
}

// A touch on the dark screen only wakes it: the shield takes it, so it
// doesn't also pick an aircraft or change the range
static void on_shield_pressed(lv_event_t *e)
{
    update_backlight(); // the touch has just reset the idle time: on at once
}

static void on_tick(lv_timer_t *t)
{
    update_clock();
    if (s_count && esp_timer_get_time() - s_last_update_us > DROP_STALE_S * 1000000LL) {
        s_count = 0; // don't keep extrapolating ghosts forever
    }
    s_blink = !s_blink;
    refresh();
    update_backlight();
}

lv_obj_t *make_label(lv_obj_t *parent, const lv_font_t *font, uint32_t color)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_label_set_text(l, "");
    return l;
}

lv_obj_t *make_box(lv_obj_t *parent)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_scrollable(o, false);
    lv_obj_set_clickable(o, false);
    return o;
}

#ifdef HAS_LIST
// "78%" and a battery outline whose fill is as wide as the charge, then the
// state underneath. Hidden until the first reading.
static void create_battery(lv_obj_t *parent)
{
    s_batt_row = make_box(parent);
    lv_obj_set_size(s_batt_row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s_batt_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_batt_row, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(s_batt_row, 10, 0);
    lv_obj_align(s_batt_row, LV_ALIGN_TOP_RIGHT, -CORNER_X, CORNER_Y);

    s_batt_level = make_label(s_batt_row, &F_CLOCK, C_TEXT);

    // Body and terminal nub sit touching, so they get their own gapless row
    lv_obj_t *icon = make_box(s_batt_row);
    lv_obj_set_size(icon, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(icon, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(icon, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    s_batt_body = make_box(icon);
    lv_obj_set_size(s_batt_body, BATT_W, BATT_H);
    lv_obj_set_style_border_width(s_batt_body, 2, 0);
    lv_obj_set_style_radius(s_batt_body, 4, 0);
    lv_obj_set_style_pad_all(s_batt_body, 3, 0);

    // Percent widths are of the body's inside, so the fill tracks the charge
    // exactly rather than in the icon font's five steps
    s_batt_fill = make_box(s_batt_body);
    lv_obj_set_height(s_batt_fill, LV_PCT(100));
    lv_obj_set_style_radius(s_batt_fill, 2, 0);
    lv_obj_set_style_bg_opa(s_batt_fill, LV_OPA_COVER, 0);

    s_batt_nub = make_box(icon);
    lv_obj_set_size(s_batt_nub, 4, BATT_H / 2);
    lv_obj_set_style_radius(s_batt_nub, 1, 0);
    lv_obj_set_style_bg_opa(s_batt_nub, LV_OPA_COVER, 0);

    s_batt_detail = make_label(parent, &F_STATUS, C_DIM);
    lv_obj_align(s_batt_detail, LV_ALIGN_TOP_RIGHT, -CORNER_X, CORNER_Y + lv_font_get_line_height(&F_CLOCK) + 2);

    lv_obj_set_hidden(s_batt_row, true);
    lv_obj_set_hidden(s_batt_detail, true);
}

static void show_battery(int percent, uint32_t outline, uint32_t fill, const char *level, const char *detail,
                         uint32_t detail_color)
{
    lv_obj_set_style_border_color(s_batt_body, lv_color_hex(outline), 0);
    lv_obj_set_style_bg_color(s_batt_nub, lv_color_hex(outline), 0);
    lv_obj_set_style_bg_color(s_batt_fill, lv_color_hex(fill), 0);
    lv_obj_set_width(s_batt_fill, LV_PCT(LV_CLAMP(0, percent, 100)));
    lv_label_set_text(s_batt_level, level);
    lv_obj_set_style_text_color(s_batt_level, lv_color_hex(outline), 0);
    lv_obj_set_hidden(s_batt_level, level[0] == '\0');
    lv_label_set_text(s_batt_detail, detail);
    lv_obj_set_style_text_color(s_batt_detail, lv_color_hex(detail_color), 0);
    lv_obj_set_hidden(s_batt_row, false);
    lv_obj_set_hidden(s_batt_detail, false);
}
#endif

// Over the middle of the scope while the setup portal is open: how to join
// it, with a QR code that does it for you (phone cameras read WIFI: codes).
static void create_setup(lv_obj_t *parent)
{
    s_setup = lv_obj_create(parent);
    lv_obj_set_size(s_setup, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_center(s_setup);
    lv_obj_set_style_bg_color(s_setup, lv_color_hex(0x0a1813), 0);
    lv_obj_set_style_bg_opa(s_setup, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_setup, lv_color_hex(C_TICK), 0);
    lv_obj_set_style_border_width(s_setup, 2, 0);
    lv_obj_set_style_radius(s_setup, CARD_PAD + 6, 0);
    lv_obj_set_style_pad_all(s_setup, CARD_PAD, 0);
    lv_obj_set_style_pad_row(s_setup, CARD_PAD / 2, 0);
    lv_obj_set_flex_flow(s_setup, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_setup, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollable(s_setup, false);

    lv_obj_t *title = make_label(s_setup, &F_TITLE, C_SELECT);
    lv_label_set_text(title, "WiFi setup");
#if LV_USE_QRCODE
    s_setup_qr = lv_qrcode_create(s_setup);
    lv_qrcode_set_size(s_setup_qr, QR_SIZE);
    lv_qrcode_set_dark_color(s_setup_qr, lv_color_black());
    lv_qrcode_set_light_color(s_setup_qr, lv_color_white());
    lv_qrcode_set_quiet_zone(s_setup_qr, true);
#endif
    s_setup_text = make_label(s_setup, &F_CARD, C_TEXT);
    lv_obj_set_style_text_align(s_setup_text, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_line_space(s_setup_text, CARD_LINE_SPACE, 0);

    lv_obj_set_hidden(s_setup, true);
}

// ---------------------------------------------------------------- public

void radar_ui_create(double center_lat, double center_lon, void (*on_range_change)(void))
{
    // All in PSRAM. Static internal RAM is short on the Tab5: esp_hosted takes
    // most of what is left before the scheduler starts, and v0.2.0, 10 KB of
    // statics heavier than v0.1.1, failed to boot for want of the timer task's
    // 2 KB stack.
    s_ac = heap_caps_calloc(AC_MAX, sizeof(aircraft_t), MALLOC_CAP_SPIRAM);
    s_order = heap_caps_calloc(AC_MAX, sizeof(int), MALLOC_CAP_SPIRAM);
    s_scr = heap_caps_calloc(AC_MAX, sizeof(ac_screen_t), MALLOC_CAP_SPIRAM);
    s_trail_slot = heap_caps_calloc(AC_MAX, sizeof(int16_t), MALLOC_CAP_SPIRAM);

    // A hold of LONG_PRESS_MS for the settings: LVGL's 400 ms caught slow swipes
    for (lv_indev_t *in = lv_indev_get_next(NULL); in; in = lv_indev_get_next(in)) {
        if (lv_indev_get_type(in) == LV_INDEV_TYPE_POINTER) lv_indev_set_long_press_time(in, LONG_PRESS_MS);
    }
    start_trails();
    s_lat0 = center_lat;
    s_lon0 = center_lon;
    s_coslat0 = cos(center_lat * M_PI / 180.0);
    s_on_range_change = on_range_change;
    s_range_idx = settings()->range_idx;
#ifdef HAS_LIST
    // The board has already rotated the display, if the settings asked
    if (lv_display_get_horizontal_resolution(NULL) > lv_display_get_vertical_resolution(NULL)) {
        s_panel = &PANEL_LANDSCAPE;
    }
#endif

    settings_ui_init(); // the theme, before any widget
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(C_BG), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_scrollable(scr, false);
    create_background(scr);

    s_scope = lv_obj_create(scr);
    lv_obj_remove_style_all(s_scope);
    lv_obj_set_size(s_scope, SCR_W, SCR_W);
    lv_obj_set_clickable(s_scope, true);
    lv_obj_add_event_cb(s_scope, on_scope_draw, LV_EVENT_DRAW_MAIN, NULL);
    // Short: a long press opens the settings, and its release isn't a tap
    lv_obj_add_event_cb(s_scope, on_scope_click, LV_EVENT_SHORT_CLICKED, NULL);
    lv_obj_add_event_cb(s_scope, on_scope_long_press, LV_EVENT_LONG_PRESSED, NULL);
    lv_obj_add_event_cb(s_scope, on_press, LV_EVENT_PRESSED, NULL);

    // Made whether it is on or not, so turning it on later keeps it under
    // the card and the panel
    for (int i = 0; i < SWEEP_SEGS; i++) {
        s_sweep[i] = lv_line_create(scr);
        lv_obj_set_style_line_color(s_sweep[i], lv_color_hex(C_SWEEP), 0);
        lv_obj_set_style_line_width(s_sweep[i], 3, 0);
        // Brighter towards the rim, like a phosphor trace
        lv_obj_set_style_line_opa(s_sweep[i], 40 + 170 * i / (SWEEP_SEGS - 1), 0);
        lv_obj_set_clickable(s_sweep[i], false);
    }
    on_sweep(NULL);
    s_sweep_timer = lv_timer_create(on_sweep, SWEEP_TICK_MS, NULL);
    show_sweep(settings()->sweep);

    // Children of the scope, so they sit against its edges on every board.
    // Labels aren't clickable, so taps still reach the scope.
    s_clock = make_label(s_scope, &F_CLOCK, C_TEXT);
    s_clock2 = make_label(s_scope, &F_STATUS, C_DIM);
    s_range_lbl = make_label(s_scope, &F_RANGE, C_TEXT);
    s_status_lbl = make_label(s_scope, &F_STATUS, C_DIM);
    lv_obj_align(s_status_lbl, LV_ALIGN_BOTTOM_MID, 0, -(CY - R_SCOPE + STATUS_Y));
    const int clock_h = lv_font_get_line_height(&F_CLOCK);
#ifdef HAS_LIST
    // The square around the scope has room in its corners: clock top left,
    // battery top right, range bottom left, traffic count bottom right
    lv_obj_align(s_clock, LV_ALIGN_TOP_LEFT, CORNER_X, CORNER_Y);
    lv_obj_align(s_clock2, LV_ALIGN_TOP_LEFT, CORNER_X, CORNER_Y + clock_h + 2);
    lv_obj_align(s_range_lbl, LV_ALIGN_BOTTOM_LEFT, CORNER_X, -CORNER_Y);
    s_feed_lbl = make_label(s_scope, &F_STATUS, C_DIM);
    lv_obj_align(s_feed_lbl, LV_ALIGN_BOTTOM_RIGHT, -CORNER_X, -CORNER_Y);
    s_count_lbl = make_label(s_scope, &F_CLOCK, C_TEXT);
    lv_obj_align(s_count_lbl, LV_ALIGN_BOTTOM_RIGHT, -CORNER_X,
                 -(CORNER_Y + lv_font_get_line_height(&F_STATUS) + 2));
    const int alert_y = CY - R_SCOPE + RANGE_Y; // low enough that the circle is wide
#else
    // A round panel has no corners: along the top and bottom of the scope
    lv_obj_align(s_clock, LV_ALIGN_TOP_MID, 0, CY - R_SCOPE + CLOCK_Y);
    lv_obj_align(s_clock2, LV_ALIGN_TOP_MID, 0, CY - R_SCOPE + CLOCK_Y + clock_h);
    lv_obj_align(s_range_lbl, LV_ALIGN_BOTTOM_MID, 0, -(CY - R_SCOPE + RANGE_Y));
    const int alert_y = CY - R_SCOPE + CLOCK_Y + clock_h + lv_font_get_line_height(&F_STATUS) + 4;
#endif
    lv_obj_set_hidden(s_clock2, true);

    // Tapping it opens the nearest alerting aircraft's card
    s_alert_lbl = make_label(s_scope, &F_STATUS, C_ALERT_INK);
    lv_obj_align(s_alert_lbl, LV_ALIGN_TOP_MID, 0, alert_y);
    lv_obj_set_style_bg_color(s_alert_lbl, lv_color_hex(C_WARN), 0);
    lv_obj_set_style_bg_opa(s_alert_lbl, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_alert_lbl, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_hor(s_alert_lbl, CARD_PAD, 0);
    lv_obj_set_style_pad_ver(s_alert_lbl, CARD_PAD / 3, 0);
    lv_obj_set_clickable(s_alert_lbl, true);
    lv_obj_add_event_cb(s_alert_lbl, on_alert_click, LV_EVENT_CLICKED, NULL);
    lv_obj_add_flag(s_alert_lbl, LV_OBJ_FLAG_FLOATING); // aligned to the scope, not laid out
    lv_obj_set_style_margin_bottom(s_alert_lbl, CARD_PAD, 0); // from the flag, in the big view
    lv_obj_set_hidden(s_alert_lbl, true);
    // LVGL sends a swipe to the first object up from the touched one that
    // doesn't pass it to its parent, and objects pass it on by default
    lv_obj_remove_flag(s_scope, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(s_scope, on_gesture, LV_EVENT_GESTURE, NULL);
    create_big(scr);
#ifndef HAS_LIST
    create_rlist(scr);
#endif

#ifdef HAS_LIST
    create_panel(scr);

    // Battery in the scope square's top-right corner, outside the circle.
    // Hidden until the first reading, so it never shows on a board without one.
    create_battery(s_scope);
#else
    // On the scope, so the alert banner can stay in front of it
    create_card(s_scope);
    lv_obj_move_to_index(s_card, lv_obj_get_index(s_alert_lbl));
#endif
    create_setup(s_scope);

    update_clock();
    update_range_label();
    refresh();
    s_sleep_shield = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_sleep_shield);
    lv_obj_set_size(s_sleep_shield, LV_PCT(100), LV_PCT(100));
    lv_obj_set_clickable(s_sleep_shield, true);
    lv_obj_add_event_cb(s_sleep_shield, on_shield_pressed, LV_EVENT_PRESSED, NULL);
    lv_obj_set_hidden(s_sleep_shield, true);
    update_backlight();
    lv_timer_create(on_tick, 1000, NULL);
    splash_show(); // last, over everything
}

#if CONFIG_LV_USE_SNAPSHOT
static char s_fake_emerg[8]; // for development: this aircraft squawks 7700

static void apply_fake_emergency(void)
{
    int i = s_fake_emerg[0] ? find_aircraft(s_fake_emerg) : -1;
    if (i < 0) return;
    s_ac[i].emergency = true;
    s_ac[i].emerg = AC_EMERG_GENERAL;
    strlcpy(s_ac[i].squawk, "7700", sizeof(s_ac[i].squawk));
}

void radar_ui_debug_emergency(const char *hex)
{
    strlcpy(s_fake_emerg, hex ? hex : "", sizeof(s_fake_emerg));
    apply_fake_emergency();
    refresh();
}
#endif

void radar_ui_set_aircraft(const aircraft_t *list, int count, const char *feed)
{
    if (count > AC_MAX) count = AC_MAX;
    memcpy(s_ac, list, count * sizeof(aircraft_t));
#if CONFIG_LV_USE_SNAPSHOT
    apply_fake_emergency();
#endif
    if (s_trail_slot) trails_update(s_ac, count, s_trail_slot);
    s_count = count;
    s_last_update_us = esp_timer_get_time();
    strlcpy(s_feed, feed, sizeof(s_feed));
    splash_set_status("Receiving traffic");
    refresh();
}

void radar_ui_set_status(const char *text)
{
    strlcpy(s_status, text ? text : "", sizeof(s_status));
    splash_set_status(s_status);
    update_status_label();
}

void radar_ui_apply_settings(const settings_t *old)
{
    const settings_t *s = settings();
    if (s->sweep != old->sweep) show_sweep(s->sweep && s_view == VIEW_RADAR);
    if (s->trail_s != old->trail_s) start_trails();
    // The starting range, chosen again: go to it now
    if (s->range_idx != old->range_idx && s->range_idx != s_range_idx) {
        s_range_idx = s->range_idx;
        update_range_label();
        if (s_on_range_change) s_on_range_change();
    }
    if (s->show_airspace != old->show_airspace || s->show_airfields != old->show_airfields ||
        s->show_centrelines != old->show_centrelines) {
        s_segs_range = -1;
        s_bg_range = -1;
    }
    if (strcmp(s->tz, old->tz) != 0 || s->clock != old->clock) update_clock();
    refresh();
}

void radar_ui_set_centre(double lat, double lon)
{
    s_lat0 = lat;
    s_lon0 = lon;
    s_coslat0 = cos(lat * M_PI / 180.0);
    s_count = 0; // the next fetch fills in around the new centre
    s_sel_hex[0] = '\0';
    radar_ui_set_aero(NULL); // projected for the old one; OpenAIP fetches again
}

int radar_ui_range_nm(void)
{
    return RANGES[s_range_idx].nm;
}

void radar_ui_set_battery(const battery_status_t *st)
{
#ifdef HAS_LIST
    if (st->state == BATT_NO_PACK || st->state == BATT_CHECKING) {
        show_battery(0, C_DIM, C_DIM, "", st->state == BATT_NO_PACK ? "No battery" : "Checking battery", C_DIM);
        return;
    }

    char level[8], detail[40];
    uint32_t color = C_TEXT, fill = C_TEXT;
    if (st->shutdown_in_s > 0) {
        snprintf(detail, sizeof(detail), "Battery flat, off in %d s", st->shutdown_in_s);
        color = fill = C_EMERG;
    } else if (st->state == BATT_CHARGING) {
        snprintf(detail, sizeof(detail), "Charging  %.2f A", -st->amps);
        fill = C_SWEEP;
    } else if (st->state == BATT_EXTERNAL) {
        strlcpy(detail, "External power", sizeof(detail));
    } else if (st->minutes_left >= 0) {
        snprintf(detail, sizeof(detail), "~%dh %02dm left", st->minutes_left / 60, st->minutes_left % 60);
    } else {
        strlcpy(detail, "On battery", sizeof(detail));
    }
    if (st->state == BATT_DISCHARGING && st->low) {
        color = fill = st->percent <= 5 ? C_EMERG : C_WARN;
    }

    // Percent is read off the voltage curve, so it is an estimate
    snprintf(level, sizeof(level), "%d%%", st->percent);
    show_battery(st->percent, color, fill, level, detail, st->shutdown_in_s > 0 ? C_EMERG : C_DIM);
#else
    (void)st;
#endif
}

void radar_ui_set_aero(aero_map_t *map)
{
    aero_map_free(s_aero);
    s_aero = map;
    s_segs_range = -1;
    s_bg_range = -1;
    refresh();
}

void radar_ui_show_setup(const char *ssid, const char *url)
{
    if (!ssid) {
        lv_obj_set_hidden(s_setup, true);
        return;
    }
    splash_hide();
#if LV_USE_QRCODE
    char qr[64];
    snprintf(qr, sizeof(qr), "WIFI:T:nopass;S:%s;;", ssid);
    lv_qrcode_update(s_setup_qr, qr, strlen(qr));
#endif
    lv_label_set_text_fmt(s_setup_text, "Scan, or join the WiFi network\n%s\nThe setup page opens itself,\nor browse to %s",
                          ssid, url);
    lv_obj_set_hidden(s_setup, false);
}
