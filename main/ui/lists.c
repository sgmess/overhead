// The lists of the nearest aircraft: the Tab5's beside the scope, with the
// card above it, and the round boards' list view, a swipe away

#include "radar_priv.h"

#ifdef HAS_LIST
static lv_obj_t *s_rows[LIST_ROWS_MAX];
static lv_obj_t *s_cells[LIST_ROWS_MAX][6];
static char s_row_hex[LIST_ROWS_MAX][8];
#endif

// A list row's cells: callsign, type, altitude in hundreds of feet with
// which way it is going, ground speed, distance and bearing
static void fmt_row(const aircraft_t *a, const ac_screen_t *s, char cells[6][16])
{
    strlcpy(cells[0], ac_name(a), sizeof(cells[0]));
    strlcpy(cells[1], a->type[0] ? a->type : "-", sizeof(cells[1]));
    if (a->on_ground) {
        strlcpy(cells[2], "GND", sizeof(cells[2]));
    } else if (a->alt_ft == AC_ALT_UNKNOWN) {
        strlcpy(cells[2], "---", sizeof(cells[2]));
    } else {
        const char *trend = a->vs_fpm > 300 ? " " LV_SYMBOL_UP : a->vs_fpm < -300 ? " " LV_SYMBOL_DOWN : "";
        snprintf(cells[2], sizeof(cells[2]), "%03d%s", (int)(a->alt_ft + 50) / 100, trend);
    }
    if (a->gs_kt >= 0) {
        snprintf(cells[3], sizeof(cells[3]), "%.0f", a->gs_kt);
    } else {
        strlcpy(cells[3], "-", sizeof(cells[3]));
    }
    snprintf(cells[4], sizeof(cells[4]), "%.1f", s->dist_nm);
    snprintf(cells[5], sizeof(cells[5]), "%03.0f\xC2\xB0", s->brg_deg);
}

// A list row's colours, set only when they change (shown keeps them): an
// emergency red and anything overhead amber all the way across, or else the
// callsign picked out, military, or by altitude
static void color_row(lv_obj_t *const cells[6], lv_color_t shown[2], const aircraft_t *a, const ac_screen_t *s,
                      bool sel)
{
    lv_color_t c[2] = {sel ? lv_color_hex(C_SELECT) : is_military(a) ? lv_color_hex(C_MIL) : alt_color(a),
                       lv_color_hex(C_TEXT)};
    if (a->emergency || s->alert) c[0] = c[1] = lv_color_hex(a->emergency ? C_EMERG : C_WARN);
    for (int p = 0; p < 2; p++) {
        if (lv_color_eq(c[p], shown[p])) continue;
        for (int k = p; k < (p ? 6 : 1); k++) lv_obj_set_style_text_color(cells[k], c[p], 0);
        shown[p] = c[p];
    }
}

static const char *const LIST_HEAD[6] = {"CALLSIGN", "TYPE", "ALT", "GS KT", "NM", "BRG"};

#ifdef HAS_LIST
// The row to mark: the selection, or with the big view up the one it shows
static bool list_marked(const aircraft_t *a)
{
    return s_view == VIEW_BIG ? strcmp(a->hex, s_big_hex) == 0 : is_selected(a);
}

// At 1 Hz across a whole table, redrawing every label would be most of the
// panel, so it only sets what changed.
void update_list(void)
{
    static bool row_sel[LIST_ROWS_MAX];
    static lv_color_t row_colors[LIST_ROWS_MAX][2];
    char cells[6][16];

    for (int r = 0; r < s_panel->list_rows; r++) {
        if (r >= s_order_n) {
            s_row_hex[r][0] = '\0';
            lv_obj_set_hidden(s_rows[r], true);
            continue;
        }
        const aircraft_t *a = &s_ac[s_order[r]];
        const ac_screen_t *s = &s_scr[s_order[r]];
        const bool sel = list_marked(a);
        strlcpy(s_row_hex[r], a->hex, sizeof(s_row_hex[r]));
        lv_obj_set_hidden(s_rows[r], false);

        if (sel != row_sel[r]) {
            lv_obj_set_style_bg_opa(s_rows[r], sel ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
            row_sel[r] = sel;
        }
        color_row(s_cells[r], row_colors[r], a, s, sel);
        fmt_row(a, s, cells);
        for (int k = 0; k < 6; k++) set_text_if_changed(s_cells[r][k], cells[k]);
    }
}
#endif

#ifndef HAS_LIST
// ---------------------------------------------------------------- list view
//
// The round boards have no list beside the scope; this is it, a swipe away.
// Within the middle of the circle, where it is widest.

lv_obj_t *s_rlist;
static lv_obj_t *s_rl_title, *s_rl_rows[RL_ROWS], *s_rl_cells[RL_ROWS][6];
static char s_rl_hex[RL_ROWS][8];

void update_rlist(void)
{
    if (s_view != VIEW_LIST) return;
    static lv_color_t colors[RL_ROWS][2];
    static bool sel_shown[RL_ROWS];
    char cells[6][16], title[48];
    format_count(title, sizeof(title));
    set_text_if_changed(s_rl_title, s_last_update_us ? title : "Waiting for data");
    for (int r = 0; r < RL_ROWS; r++) {
        if (r >= s_order_n) {
            s_rl_hex[r][0] = '\0';
            lv_obj_set_hidden(s_rl_rows[r], true);
            continue;
        }
        const aircraft_t *a = &s_ac[s_order[r]];
        strlcpy(s_rl_hex[r], a->hex, sizeof(s_rl_hex[r]));
        lv_obj_set_hidden(s_rl_rows[r], false);
        const bool sel = is_selected(a);
        if (sel != sel_shown[r]) {
            lv_obj_set_style_bg_opa(s_rl_rows[r], sel ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
            sel_shown[r] = sel;
        }
        color_row(s_rl_cells[r], colors[r], a, &s_scr[s_order[r]], sel);
        fmt_row(a, &s_scr[s_order[r]], cells);
        for (int k = 0; k < 6; k++) set_text_if_changed(s_rl_cells[r][k], cells[k]);
    }
}
#endif

#ifndef HAS_LIST

// A row opens its aircraft in the big view
static void on_rl_row_click(lv_event_t *e)
{
    if (swiped()) return;
    const int r = (int)(intptr_t)lv_event_get_user_data(e);
    if (!s_rl_hex[r][0]) return;
    open_big(s_rl_hex[r]);
}

void create_rlist(lv_obj_t *scr)
{
    static const int xs[6] = RL_COLS;
    s_rlist = lv_obj_create(scr);
    lv_obj_remove_style_all(s_rlist);
    lv_obj_set_size(s_rlist, SCR_W, SCR_W);
    lv_obj_set_style_bg_color(s_rlist, lv_color_hex(C_BG), 0);
    lv_obj_set_style_bg_opa(s_rlist, LV_OPA_COVER, 0);
    lv_obj_set_flex_flow(s_rlist, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_rlist, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollable(s_rlist, false);
    lv_obj_set_clickable(s_rlist, true);
    lv_obj_remove_flag(s_rlist, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(s_rlist, on_gesture, LV_EVENT_GESTURE, NULL);
    lv_obj_add_event_cb(s_rlist, on_scope_long_press, LV_EVENT_LONG_PRESSED, NULL);
    lv_obj_add_event_cb(s_rlist, on_press, LV_EVENT_PRESSED, NULL);
    lv_obj_set_hidden(s_rlist, true);

    s_rl_title = big_label(s_rlist, &F_CLOCK, C_TEXT);
    lv_obj_set_style_margin_bottom(s_rl_title, CARD_PAD / 2, 0);
    lv_obj_t *head = lv_obj_create(s_rlist);
    lv_obj_remove_style_all(head);
    lv_obj_set_size(head, RL_W, lv_font_get_line_height(&F_RL_HEAD) + 6);
    lv_obj_set_clickable(head, false);
    for (int k = 0; k < 6; k++) {
        lv_obj_t *h = make_label(head, &F_RL_HEAD, C_DIM);
        lv_label_set_text(h, LIST_HEAD[k]);
        lv_obj_set_pos(h, xs[k] + 8, 0);
    }
    for (int r = 0; r < RL_ROWS; r++) {
        lv_obj_t *row = lv_obj_create(s_rlist);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, RL_W, RL_ROW_H);
        lv_obj_set_scrollable(row, false);
        lv_obj_set_style_radius(row, 8, 0);
        lv_obj_set_style_bg_color(row, lv_color_hex(0x0e2a20), 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_add_event_cb(row, on_rl_row_click, LV_EVENT_SHORT_CLICKED, (void *)(intptr_t)r);
        for (int k = 0; k < 6; k++) {
            s_rl_cells[r][k] = make_label(row, &F_RL, k == 0 ? C_SELECT : C_TEXT);
            lv_obj_align(s_rl_cells[r][k], LV_ALIGN_LEFT_MID, xs[k] + 8, 0);
        }
        lv_obj_set_hidden(row, true);
        s_rl_rows[r] = row;
    }
}
#endif

#ifdef HAS_LIST
// Tapping a row selects that aircraft; tapping the selected row clears it.
static void on_row_click(lv_event_t *e)
{
    if (swiped()) return;
    int r = (int)(intptr_t)lv_event_get_user_data(e);
    if (!s_row_hex[r][0]) return;
    if (s_view == VIEW_BIG) s_big_auto[0] = '\0'; // what was tapped, ahead of an alert
    select_aircraft(strcmp(s_row_hex[r], s_sel_hex) == 0 ? NULL : s_row_hex[r]);
}
#endif

#ifdef HAS_LIST
// The panel below the scope: the details card on top while something is
// selected, then the nearest aircraft. Rows that don't fit are clipped.
void create_panel(lv_obj_t *scr)
{
    lv_obj_t *panel = make_box(scr);
    lv_obj_set_pos(panel, s_panel->x, s_panel->y);
    lv_obj_set_size(panel, s_panel->w, s_panel->h);
    lv_obj_set_style_pad_all(panel, PANEL_PAD, 0);
    lv_obj_set_style_pad_row(panel, 12, 0);
    // Divider on the side facing the scope
    lv_obj_set_style_border_side(panel, s_panel->x ? LV_BORDER_SIDE_LEFT : LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(C_RING), 0);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);

    create_card(panel); // flex skips it while hidden, so the list moves up

    lv_obj_t *list = make_box(panel);
    lv_obj_set_width(list, LV_PCT(100));
    lv_obj_set_flex_grow(list, 1);

    const int *xs = s_panel->list_cols;
    for (int k = 0; k < 6; k++) {
        lv_obj_t *h = make_label(list, &F_LIST_HEAD, C_DIM);
        lv_label_set_text(h, LIST_HEAD[k]);
        lv_obj_set_pos(h, xs[k] + 8, 0);
    }

    const int head_h = lv_font_get_line_height(&F_LIST_HEAD) + 6;
    for (int r = 0; r < s_panel->list_rows; r++) {
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
