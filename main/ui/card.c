// The card: the selected aircraft's details, over the scope on the round
// boards and above the list on the Tab5

#include "radar_priv.h"

lv_obj_t *s_card;
static lv_obj_t *s_card_flag, *s_card_title, *s_card_route, *s_card_airline, *s_card_sub;
static lv_obj_t *s_card_k1, *s_card_v1, *s_card_k2, *s_card_v2, *s_card_times, *s_card_info;
static char s_card_iso[3]; // the flag showing; "" for none
#ifdef HAS_LOGO
static lv_obj_t *s_card_logo_box, *s_card_logo;
static char s_card_logo_icao[4]; // the logo showing; "" for none
#endif

// The flag before the callsign, and the callsign after it or at the edge
static void update_card_flag(const char *iso)
{
    if (strcmp(iso, s_card_iso) == 0) return;
    strlcpy(s_card_iso, iso, sizeof(s_card_iso));
    const lv_image_dsc_t *img = iso[0] ? flag_image(iso, 0) : NULL;
    lv_obj_set_hidden(s_card_flag, !img);
    if (!img) {
        lv_obj_set_pos(s_card_title, 0, 0);
        return;
    }
    lv_image_set_src(s_card_flag, img);
    const int w = img->header.w * FLAG_SCALE / 256, h = img->header.h * FLAG_SCALE / 256;
    lv_obj_set_pos(s_card_flag, 0, (lv_font_get_line_height(&F_TITLE) - h) / 2);
    lv_obj_set_pos(s_card_title, w + w / 3, 0);
}

#ifdef HAS_LOGO
static void update_card_logo(const char *icao)
{
    if (strcmp(icao, s_card_logo_icao) == 0) return;
    strlcpy(s_card_logo_icao, icao, sizeof(s_card_logo_icao));
    const lv_image_dsc_t *img = icao[0] ? logo_image(icao, 0) : NULL;
    if (img) lv_image_set_src(s_card_logo, img);
    lv_obj_set_hidden(s_card_logo_box, !img);
}
#endif

void update_card(void)
{
    int i = s_sel_hex[0] ? find_aircraft(s_sel_hex) : -1;
    // Only with the scope: the other views show their own
    if (s_view != VIEW_RADAR) {
        lv_obj_set_hidden(s_card, true);
        return;
    }
    if (i < 0) {
        s_sel_hex[0] = '\0';
        lv_obj_set_hidden(s_card, true);
        return;
    }
    const aircraft_t *a = &s_ac[i];
    const ac_screen_t *s = &s_scr[i];
    char alt[24], vs[24];
#ifndef HAS_LIST
    // Over the half of the scope the aircraft isn't in, so it stays in view;
    // on the Tab5 the panel places it. A margin, so one crossing the middle
    // doesn't flip it back and forth.
    static bool top;
    static int placed = -1; // aligning redraws even in the same place, so only on a move
    if (s->y > CY + CARD_FLIP) top = true;
    if (s->y < CY - CARD_FLIP) top = false;
    if (placed != top) {
        lv_obj_align(s_card, LV_ALIGN_CENTER, 0, top ? -CARD_OFFSET : CARD_OFFSET);
        placed = top;
    }
#endif

    // Each only when it changes: setting a label redraws it even when the
    // text is the same, and this runs every second
    set_text_if_changed(s_card_title, ac_name(a));
    const country_t *country = country_for_hex(a->hex);
    update_card_flag(country ? country->iso : "");

    char route[64], progress[16];
    fmt_route(a, route, sizeof(route), NULL, 0);
    fmt_progress(a, false, progress, sizeof(progress));
    if (route[0] && progress[0]) {
        strlcat(route, "  " LV_SYMBOL_BULLET "  ", sizeof(route));
        strlcat(route, progress, sizeof(route));
    }
    set_text_if_changed(s_card_route, route);
    lv_obj_set_hidden(s_card_route, !route[0]);

    char airline[64];
    const airline_t *al = fmt_airline(a, country, airline, sizeof(airline));
    set_text_if_changed(s_card_airline, airline);
#ifdef HAS_LOGO
    update_card_logo(al ? al->icao : "");
#endif

    char sub[64];
    fmt_sub(a, true, sub, sizeof(sub));
    set_text_if_changed(s_card_sub, sub);

    // When it left and gets in, then the owner and age, when known, each on
    // a line of its own that moves the figures down
    char times[64], info[112];
    fmt_times(a, false, times, sizeof(times));
    set_text_if_changed(s_card_times, times);
    lv_obj_set_hidden(s_card_times, !times[0]);
    fmt_info(a, al, info, sizeof(info));
    set_text_if_changed(s_card_info, info);
    lv_obj_set_hidden(s_card_info, !info[0]);
    int extra = times[0] ? CARD_LINE : 0;
    lv_obj_set_y(s_card_info, CARD_SUB_Y + 2 * CARD_LINE + extra);
    if (info[0]) extra += CARD_LINE;
    lv_obj_t *cols[] = {s_card_k1, s_card_v1, s_card_k2, s_card_v2};
    for (int k = 0; k < 4; k++) lv_obj_set_y(cols[k], CARD_ROWS_Y + CARD_LINE + extra);
    lv_obj_set_height(s_card, CARD_H + CARD_LINE + extra);

    fmt_alt(a, alt, sizeof(alt));
    fmt_vs(a, vs, sizeof(vs));

    char gs[16] = "-", trk[16] = "-";
    if (a->gs_kt >= 0) snprintf(gs, sizeof(gs), "%.0f kt", a->gs_kt);
    if (a->track_deg >= 0) snprintf(trk, sizeof(trk), "%03.0f\xC2\xB0", a->track_deg);

    char v[96];
    snprintf(v, sizeof(v), "%s\n%s\n%s%s", alt, gs, a->squawk[0] ? a->squawk : "-",
             a->emergency ? "  " LV_SYMBOL_WARNING : "");
    set_text_if_changed(s_card_v1, v);
    // Where to look for it, or (settings) its bearing
    char look[40];
    if (settings()->card_look) {
        fmt_look(a, s, true, look, sizeof(look));
    } else {
        snprintf(look, sizeof(look), "%.1f NM  %03.0f\xC2\xB0", s->dist_nm, s->brg_deg);
    }
    snprintf(v, sizeof(v), "%s\n%s\n%s", vs, trk, look);
    set_text_if_changed(s_card_v2, v);
    lv_obj_set_hidden(s_card, false);
}

// The card opens its aircraft in the big view
static void on_card_click(lv_event_t *e)
{
    if (swiped()) return;
    if (s_sel_hex[0]) open_big(s_sel_hex);
}

void create_card(lv_obj_t *parent)
{
    s_card = lv_obj_create(parent);
    lv_obj_set_size(s_card, CARD_W, CARD_H + CARD_LINE);
    lv_obj_set_style_bg_color(s_card, lv_color_hex(0x0a1813), 0);
    lv_obj_set_style_bg_opa(s_card, LV_OPA_90, 0);
    lv_obj_set_style_border_color(s_card, lv_color_hex(C_TICK), 0);
    lv_obj_set_style_border_width(s_card, 2, 0);
    lv_obj_set_style_radius(s_card, CARD_PAD + 6, 0);
    lv_obj_set_style_pad_all(s_card, CARD_PAD, 0);
    lv_obj_set_scrollable(s_card, false);
    lv_obj_add_event_cb(s_card, on_card_click, LV_EVENT_CLICKED, NULL);

    // The registration country's flag, level with the callsign before it
    s_card_flag = lv_image_create(s_card);
    lv_image_set_pivot(s_card_flag, 0, 0);
    lv_image_set_scale(s_card_flag, FLAG_SCALE);
    lv_obj_set_hidden(s_card_flag, true);
    s_card_title = make_label(s_card, &F_TITLE, C_SELECT);
    // Origin and destination, level with the callsign at the far side
    s_card_route = make_label(s_card, &F_CARD, C_TEXT);
    lv_obj_align(s_card_route, LV_ALIGN_TOP_RIGHT, 0,
                 (lv_font_get_line_height(&F_TITLE) - lv_font_get_line_height(&F_CARD)) / 2);
    lv_obj_set_hidden(s_card_route, true);
    // The flight number and airline, or for anything else the country
    s_card_airline = make_label(s_card, &F_CARD, C_TEXT);
    lv_obj_set_pos(s_card_airline, 0, CARD_SUB_Y);
    s_card_sub = make_label(s_card, &F_CARD, C_TEXT);
    lv_obj_set_pos(s_card_sub, 0, CARD_SUB_Y + CARD_LINE);
    // When it left and gets in, from AeroDataBox
    s_card_times = make_label(s_card, &F_CARD, C_TEXT);
    lv_obj_set_pos(s_card_times, 0, CARD_SUB_Y + 2 * CARD_LINE);
    lv_obj_set_width(s_card_times, CARD_W - 2 * CARD_PAD);
    lv_label_set_long_mode(s_card_times, LV_LABEL_LONG_DOT);
    lv_obj_set_hidden(s_card_times, true);
    // The owner and age; long owners' names end in "..."
    s_card_info = make_label(s_card, &F_CARD, C_DIM);
    lv_obj_set_pos(s_card_info, 0, CARD_SUB_Y + 2 * CARD_LINE);
    lv_obj_set_width(s_card_info, CARD_W - 2 * CARD_PAD);
    lv_label_set_long_mode(s_card_info, LV_LABEL_LONG_DOT);
    lv_obj_set_hidden(s_card_info, true);
#ifdef HAS_LOGO
    // Beside those two lines: logos are made for white, and many vanish on
    // the card's dark green
    s_card_logo_box = lv_obj_create(s_card);
    lv_obj_remove_style_all(s_card_logo_box);
    lv_obj_set_size(s_card_logo_box, LOGO_MAX_W + 12, LOGO_MAX_H + 10);
    lv_obj_set_style_bg_color(s_card_logo_box, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(s_card_logo_box, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_card_logo_box, 8, 0);
    lv_obj_set_scrollable(s_card_logo_box, false);
    lv_obj_set_clickable(s_card_logo_box, false);
    lv_obj_align(s_card_logo_box, LV_ALIGN_TOP_RIGHT, 0,
                 CARD_SUB_Y + (CARD_LINE + lv_font_get_line_height(&F_CARD) - (LOGO_MAX_H + 10)) / 2);
    s_card_logo = lv_image_create(s_card_logo_box);
    lv_obj_center(s_card_logo);
    lv_obj_set_hidden(s_card_logo_box, true);
#endif

    s_card_k1 = make_label(s_card, &F_CARD, C_DIM);
    s_card_v1 = make_label(s_card, &F_CARD, C_SELECT);
    s_card_k2 = make_label(s_card, &F_CARD, C_DIM);
    s_card_v2 = make_label(s_card, &F_CARD, C_SELECT);
    lv_obj_t *cols[] = {s_card_k1, s_card_v1, s_card_k2, s_card_v2};
#ifdef HAS_LIST
    const int *xs = s_panel->card_cols;
#else
    static const int xs[] = CARD_COLS;
#endif
    for (int k = 0; k < 4; k++) {
        lv_obj_set_pos(cols[k], xs[k], CARD_ROWS_Y + CARD_LINE);
        lv_obj_set_style_text_line_space(cols[k], CARD_LINE_SPACE, 0);
    }
    lv_label_set_text(s_card_k1, "ALT\nGS\nSQK");
    lv_label_set_text(s_card_k2, "V/S\nTRK\nRNG");

    lv_obj_set_hidden(s_card, true);
}

