// The big view: one aircraft in large type, in the scope's square

#include "radar_priv.h"

// ---------------------------------------------------------------- big view
//
// One aircraft in large type, for across the room: one that has just started
// alerting (an emergency, or overhead), then the selection, then the nearest
// airborne. A tap selects the next nearest, so it stays picked on the scope. A swipe swaps it with the
// scope, in the same square; on the Tab5 the list stays beside it.

#define BIG_STEPS 10        // a tap steps through the nearest this many
#define BIG_HOLD_NM 0.5f    // the one shown stays until another is this much nearer

lv_obj_t *s_big;
static lv_obj_t *s_big_flag, *s_big_logo_box, *s_big_logo, *s_big_call, *s_big_airline;
static lv_obj_t *s_big_alt, *s_big_vs, *s_big_pointer, *s_big_gs, *s_big_trk, *s_big_where;
static lv_obj_t *s_big_pic, *s_big_photo, *s_big_credit; // the photo and who took it, in s_big_pic
static lv_image_dsc_t s_photo_dsc;
static char s_photo_hex[8]; // whose photo is in s_photo_dsc; "" for none
static lv_obj_t *s_big_route, *s_big_places, *s_big_progress, *s_big_times, *s_big_info, *s_big_sub, *s_big_foot, *s_big_body, *s_big_empty;
static char s_big_iso[3], s_big_icao[4] = "-";
static float s_big_ptr_rad;
static lv_color_t s_big_ptr_color;

// In the air: not flagged on the ground, and not sitting at a gate with no
// altitude (some transponders on the ground never say they are)
static bool airborne(const aircraft_t *a)
{
    return !a->on_ground && !(a->alt_ft == AC_ALT_UNKNOWN && a->gs_kt < 50);
}

static const char *ordinal(int k)
{
    static const char *const N[] = {"Nearest", "2nd nearest", "3rd nearest", "4th nearest", "5th nearest",
                                    "6th nearest", "7th nearest", "8th nearest", "9th nearest", "10th nearest"};
    return N[k < BIG_STEPS ? k : BIG_STEPS - 1];
}

// The airborne aircraft nearest first, as many as a tap steps through, and
// how many there are in all
static int big_near(int near[BIG_STEPS], int *n_airborne)
{
    int n_near = 0;
    *n_airborne = 0;
    for (int k = 0; k < s_order_n; k++) { // s_order is sorted by distance
        const int i = s_order[k];
        if (!airborne(&s_ac[i])) continue;
        if (n_near < BIG_STEPS) near[n_near++] = i;
        (*n_airborne)++;
    }
    return n_near;
}

// Which aircraft, and the footer that says why
static int big_target(char *why, size_t n)
{
    if (s_big_auto[0]) {
        const int i = find_aircraft(s_big_auto);
        if (i >= 0 && s_scr[i].visible && (s_big_auto_emerg ? s_ac[i].emergency : s_scr[i].alert)) {
            snprintf(why, n, "%s  " LV_SYMBOL_BULLET "  tap for the next", s_big_auto_emerg ? "Emergency" : "Overhead");
            return i;
        }
        s_big_auto[0] = '\0'; // it has passed
    }
    int near[BIG_STEPS], n_airborne;
    const int n_near = big_near(near, &n_airborne);
    const int sel = s_sel_hex[0] ? find_aircraft(s_sel_hex) : -1;
    if (sel >= 0 && s_scr[sel].visible) {
        int k = 0;
        while (k < n_near && near[k] != sel) k++;
        if (k < n_near) {
            snprintf(why, n, "%s of %d  " LV_SYMBOL_BULLET "  tap for the next", ordinal(k), n_airborne);
        } else {
            strlcpy(why, "Selected  " LV_SYMBOL_BULLET "  tap for the nearest", n);
        }
        return sel;
    }
    if (!n_near) return -1;
    int i = near[0];
    // Two at about the same distance shouldn't make it flicker between them
    const int held = s_big_hex[0] ? find_aircraft(s_big_hex) : -1;
    if (held >= 0 && held != i && s_scr[held].visible && airborne(&s_ac[held]) &&
        s_scr[held].dist_nm <= s_scr[i].dist_nm + BIG_HOLD_NM) {
        i = held;
    }
    snprintf(why, n, "Nearest of %d  " LV_SYMBOL_BULLET "  tap for the next", n_airborne);
    return i;
}

// The photo, once it has come
static void update_big_photo(const aircraft_t *a)
{
    if (strcmp(s_photo_hex, a->hex) != 0) {
        s_photo_hex[0] = '\0';
        int w, h;
        char credit[48];
        if (s_photo_dsc.data &&
            details_photo(a->hex, a->reg, (uint16_t *)s_photo_dsc.data, &w, &h, credit, sizeof(credit))) {
            strlcpy(s_photo_hex, a->hex, sizeof(s_photo_hex));
            s_photo_dsc.header.w = w;
            s_photo_dsc.header.h = h;
            s_photo_dsc.header.stride = w * 2;
            s_photo_dsc.data_size = w * h * 2;
            lv_image_cache_drop(&s_photo_dsc); // the same buffer, a new picture
            lv_image_set_src(s_big_photo, &s_photo_dsc);
            lv_obj_invalidate(s_big_photo);
            // planespotters.net asks that the photographer is named
#ifdef HAS_LIST
            lv_label_set_text_fmt(s_big_credit, "Photo %s\nplanespotters.net", credit); // under the photo
#else
            lv_label_set_text_fmt(s_big_credit, "Photo %s  " LV_SYMBOL_BULLET "  planespotters.net", credit);
#endif
        }
    }
    lv_obj_set_hidden(s_big_pic, !s_photo_hex[0]);
}

void update_big(void)
{
    if (s_view != VIEW_BIG) return;
    char why[64], text[64], num[32];
    const int i = big_target(why, sizeof(why));
    lv_obj_set_hidden(s_big_body, i < 0);
    lv_obj_set_hidden(s_big_empty, i >= 0);
    if (i < 0) {
        s_big_hex[0] = '\0';
        return;
    }
    const aircraft_t *a = &s_ac[i];
    const ac_screen_t *s = &s_scr[i];
    strlcpy(s_big_hex, a->hex, sizeof(s_big_hex));

    const country_t *country = country_for_hex(a->hex);
    const char *iso = country ? country->iso : "";
    if (strcmp(iso, s_big_iso) != 0) {
        strlcpy(s_big_iso, iso, sizeof(s_big_iso));
        const lv_image_dsc_t *img = iso[0] ? flag_image(iso, 1) : NULL;
        if (img) lv_image_set_src(s_big_flag, img);
        lv_obj_set_hidden(s_big_flag, !img);
    }
    set_text_if_changed(s_big_call, ac_name(a));
    char airline[64];
    const airline_t *al = fmt_airline(a, country, airline, sizeof(airline));
    set_text_if_changed(s_big_airline, airline);
    lv_obj_set_hidden(s_big_airline, !airline[0]);
#ifdef HAS_LOGO
    const char *icao = al ? al->icao : "";
    if (strcmp(icao, s_big_icao) != 0) {
        strlcpy(s_big_icao, icao, sizeof(s_big_icao));
        const lv_image_dsc_t *img = icao[0] ? logo_image(icao, 1) : NULL;
        if (img) lv_image_set_src(s_big_logo, img);
        lv_obj_set_hidden(s_big_logo_box, !img);
    }
#else
    (void)al;
#endif

    // Altitude in its scope colour, with which way it is going
    char alt[24];
    fmt_alt(a, alt, sizeof(alt));
    const char *trend = a->on_ground ? "" : a->vs_fpm > 250 ? " " LV_SYMBOL_UP : a->vs_fpm < -250 ? " " LV_SYMBOL_DOWN : "";
    snprintf(text, sizeof(text), "%s%s", alt, trend);
    set_text_if_changed(s_big_alt, text);
    set_text_color_if_changed(s_big_alt, alt_color(a));
    fmt_vs(a, text, sizeof(text));
    set_text_if_changed(s_big_vs, a->alt_ft == AC_ALT_UNKNOWN ? "" : text);

    // Speed, with the arrow along the track: which way it is going, north up
    // as on the scope
    if (a->gs_kt >= 0) {
        snprintf(text, sizeof(text), "%.0f kt", a->gs_kt);
    } else {
        strlcpy(text, "-", sizeof(text));
    }
    set_text_if_changed(s_big_gs, text);
    text[0] = '\0';
    if (a->track_deg >= 0) snprintf(text, sizeof(text), "track %03.0f\xC2\xB0", a->track_deg);
    set_text_if_changed(s_big_trk, text);
    lv_obj_set_hidden(s_big_pointer, a->track_deg < 0);
    // Redrawn only when it turns or changes colour (hiding redraws by itself)
    const float rad = a->track_deg * DEG2RAD;
    const lv_color_t color = alt_color(a);
    if (rad != s_big_ptr_rad || !lv_color_eq(color, s_big_ptr_color)) {
        s_big_ptr_rad = rad;
        s_big_ptr_color = color;
        lv_obj_invalidate(s_big_pointer);
    }

    // Where it is from here, and its squawk
    snprintf(text, sizeof(text), "%.1f NM  " LV_SYMBOL_BULLET "  bearing %03.0f\xC2\xB0", s->dist_nm, s->brg_deg);
    if (a->squawk[0]) {
        snprintf(num, sizeof(num), "  " LV_SYMBOL_BULLET "  squawk %s", a->squawk);
        strlcat(text, num, sizeof(text));
    }
    set_text_if_changed(s_big_where, text);
    set_text_color_if_changed(s_big_where, lv_color_hex(a->emergency ? C_EMERG : C_TEXT));

    char route[64], places[ROUTE_PLACES_MAX + 16]; // 64: a multi-stop route, as the card allows
    fmt_route(a, route, sizeof(route), places, sizeof(places));
    set_text_if_changed(s_big_route, route);
    lv_obj_set_hidden(s_big_route, !route[0]);
    set_text_if_changed(s_big_places, places);
    lv_obj_set_hidden(s_big_places, !places[0]);
    char progress[64];
    fmt_progress(a, true, progress, sizeof(progress));
    set_text_if_changed(s_big_progress, progress);
    lv_obj_set_hidden(s_big_progress, !progress[0]);
    char times[160];
#ifdef HAS_LIST
    fmt_times(a, true, times, sizeof(times));
#else
    fmt_times(a, false, times, sizeof(times)); // the card's one line: the round panels have no more room
#endif
    set_text_if_changed(s_big_times, times);
    lv_obj_set_hidden(s_big_times, !times[0]);
    char sub[64];
    fmt_sub(a, false, sub, sizeof(sub));
    set_text_if_changed(s_big_sub, sub);
    char info[112];
    fmt_info(a, airline_for_callsign(a->callsign), info, sizeof(info));
    set_text_if_changed(s_big_info, info);
    lv_obj_set_hidden(s_big_info, !info[0]);
    update_big_photo(a);
    set_text_if_changed(s_big_foot, why);
}


// Open the big view on hex, selected, ahead of anything alerting
void open_big(const char *hex)
{
    char copy[sizeof(s_sel_hex)]; // hex may be s_sel_hex itself
    strlcpy(copy, hex, sizeof(copy));
    if (s_view != VIEW_BIG) show_view(VIEW_BIG);
    s_big_auto[0] = '\0';
    select_aircraft(copy);
}

// A tap selects the next nearest after the one shown, back round to the nearest
static void on_big_click(lv_event_t *e)
{
    if (swiped()) return;
    int near[BIG_STEPS], n_airborne;
    const int n_near = big_near(near, &n_airborne);
    if (!n_near) return;
    int next = 0;
    for (int k = 0; k < n_near; k++) {
        if (strcmp(s_ac[near[k]].hex, s_big_hex) == 0) next = (k + 1) % n_near;
    }
    open_big(s_ac[near[next]].hex);
}

static void on_big_pointer_draw(lv_event_t *e)
{
    lv_obj_t *obj = lv_event_get_target(e);
    lv_area_t a;
    lv_obj_get_coords(obj, &a);
    draw_arrow_scaled(lv_event_get_layer(e), (a.x1 + a.x2) / 2, (a.y1 + a.y2) / 2, s_big_ptr_rad, s_big_ptr_color,
                      BIG_POINTER);
}

static lv_obj_t *big_box(lv_obj_t *parent, lv_flex_flow_t flow)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(o, flow);
    lv_obj_set_flex_align(o, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollable(o, false);
    lv_obj_set_clickable(o, false);
    return o;
}


lv_obj_t *big_label(lv_obj_t *parent, const lv_font_t *font, uint32_t color)
{
    lv_obj_t *l = make_label(parent, font, color);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    return l;
}

void create_big(lv_obj_t *scr)
{
    s_big = lv_obj_create(scr);
    lv_obj_remove_style_all(s_big);
    lv_obj_set_size(s_big, SCR_W, SCR_W);
    lv_obj_set_style_bg_color(s_big, lv_color_hex(C_BG), 0);
    lv_obj_set_style_bg_opa(s_big, LV_OPA_COVER, 0);
    // Centred both ways: on the round panels that is where the circle is widest
    lv_obj_set_style_pad_ver(s_big, BIG_PAD_TOP, 0);
    lv_obj_set_style_pad_hor(s_big, BIG_PAD_SIDE, 0);
    lv_obj_set_flex_flow(s_big, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_big, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollable(s_big, false);
    lv_obj_set_clickable(s_big, true);
    lv_obj_add_event_cb(s_big, on_big_click, LV_EVENT_SHORT_CLICKED, NULL);
    lv_obj_add_event_cb(s_big, on_scope_long_press, LV_EVENT_LONG_PRESSED, NULL);
    lv_obj_add_event_cb(s_big, on_press, LV_EVENT_PRESSED, NULL);
    lv_obj_remove_flag(s_big, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(s_big, on_gesture, LV_EVENT_GESTURE, NULL);
    lv_obj_set_hidden(s_big, true);

    s_big_body = big_box(s_big, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_width(s_big_body, LV_PCT(100));
    lv_obj_set_style_pad_row(s_big_body, CARD_PAD / 2, 0);

#ifdef HAS_LIST
    // The Tab5's square is wide enough for the photo beside who it is
    lv_obj_t *top = big_box(s_big_body, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(top, CARD_PAD * 2, 0);
    lv_obj_t *who = big_box(top, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(who, CARD_PAD / 2, 0);
    s_big_pic = big_box(top, LV_FLEX_FLOW_COLUMN);
#else
    lv_obj_t *who = s_big_body;
#endif
    lv_obj_t *head = big_box(who, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(head, CARD_PAD, 0);
    s_big_flag = lv_image_create(head);
    lv_obj_set_hidden(s_big_flag, true);
#ifdef HAS_LOGO
    s_big_logo_box = big_box(head, LV_FLEX_FLOW_ROW);
    lv_obj_set_size(s_big_logo_box, LOGO_MAX_W + 12, LOGO_MAX_H + 10);
    lv_obj_set_style_bg_color(s_big_logo_box, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(s_big_logo_box, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_big_logo_box, 8, 0);
    s_big_logo = lv_image_create(s_big_logo_box);
    lv_obj_set_hidden(s_big_logo_box, true);
#endif

    s_big_call = big_label(who, &F_BIG, C_SELECT);
    s_big_airline = big_label(who, &F_BIG_TEXT, C_TEXT);

    // Altitude and speed side by side, the largest things on the screen
    lv_obj_t *nums = big_box(s_big_body, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(nums, CARD_PAD * 3, 0);
    lv_obj_set_style_pad_ver(nums, CARD_PAD / 2, 0);
    lv_obj_t *left = big_box(nums, LV_FLEX_FLOW_COLUMN);
    s_big_alt = big_label(left, &F_BIG, C_TEXT);
    s_big_vs = big_label(left, &F_BIG_TEXT, C_DIM);
    lv_obj_t *right = big_box(nums, LV_FLEX_FLOW_COLUMN);
    lv_obj_t *speed = big_box(right, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(speed, CARD_PAD / 2, 0);
    s_big_pointer = lv_obj_create(speed);
    lv_obj_remove_style_all(s_big_pointer);
    const int ptr = (int)(26 * BIG_POINTER);
    lv_obj_set_size(s_big_pointer, ptr, ptr);
    lv_obj_set_clickable(s_big_pointer, false);
    lv_obj_add_event_cb(s_big_pointer, on_big_pointer_draw, LV_EVENT_DRAW_MAIN, NULL);
    s_big_gs = big_label(speed, &F_BIG, C_TEXT);
    s_big_trk = big_label(right, &F_BIG_TEXT, C_DIM);

    s_big_where = big_label(s_big_body, &F_BIG_TEXT, C_TEXT);
    s_big_route = big_label(s_big_body, &F_BIG_MID, C_TEXT);
    s_big_places = big_label(s_big_body, &F_BIG_TEXT, C_TEXT);
    lv_obj_set_width(s_big_places, LV_PCT(100));
    lv_label_set_long_mode(s_big_places, LV_LABEL_LONG_WRAP); // a multi-leg route can be long
    s_big_progress = big_label(s_big_body, &F_BIG_TEXT, C_TEXT);
    s_big_times = big_label(s_big_body, &F_BIG_TEXT, C_TEXT);
    lv_obj_set_width(s_big_times, LV_PCT(100));
    lv_label_set_long_mode(s_big_times, LV_LABEL_LONG_WRAP);
    lv_obj_set_hidden(s_big_times, true);
    s_big_sub = big_label(s_big_body, &F_BIG_TEXT, C_DIM);
    s_big_info = big_label(s_big_body, &F_BIG_TEXT, C_DIM);
    lv_obj_set_width(s_big_info, LV_PCT(100));
    lv_label_set_long_mode(s_big_info, LV_LABEL_LONG_WRAP);
    // A photo of it, once it has come, and who took it: on the round boards
    // at the foot, where there is room
#ifndef HAS_LIST
    s_big_pic = big_box(s_big_body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_top(s_big_pic, CARD_PAD / 2, 0);
#endif
    lv_obj_set_style_pad_row(s_big_pic, 4, 0);
    lv_obj_set_hidden(s_big_pic, true);
    s_big_photo = lv_image_create(s_big_pic);
    s_big_credit = big_label(s_big_pic, &F_STATUS, C_DIM);
    s_photo_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    s_photo_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    s_photo_dsc.data = heap_caps_malloc(DETAILS_PHOTO_W_MAX * DETAILS_PHOTO_H * 2, MALLOC_CAP_SPIRAM);
    s_big_foot = big_label(s_big_body, &F_STATUS, C_DIM);
    lv_obj_set_style_pad_top(s_big_foot, CARD_PAD, 0);

    s_big_empty = big_label(s_big, &F_BIG_TEXT, C_DIM);
    lv_label_set_text(s_big_empty, "No aircraft in range\n\nSwipe for the radar");
    lv_obj_set_hidden(s_big_empty, true);
}

