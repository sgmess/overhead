#pragma once

// Shared between the parts of the radar screen, and only them: radar_ui.c
// (state, overlay, views and touch, the public API), scope.c, card.c,
// big_view.c, lists.c and describe.c.

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "airlines.h"
#include "backlight.h"
#include "board.h"
#include "bsp/display.h"
#include "countries.h"
#include "details.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "flags.h"
#include "logos.h"
#include "lvgl.h"
#include "misc/cache/lv_cache.h" // lv_image_cache_drop(), not in lvgl.h
#include "openaip.h"
#include "radar_ui.h"
#include "route.h"
#include "settings.h"
#include "settings_ui.h"
#include "splash.h"
#include "trails.h"

// The scope is the top SCR_W x SCR_W square: the whole panel on the round
// boards, the upper part of the Tab5's portrait 720x1280 with the traffic
// list below it.
#define SCR_W BSP_LCD_H_RES
#define CX (SCR_W / 2)
#define CY (SCR_W / 2)
#define CARD_OFFSET (SCR_W * 18 / 100) // round boards: the card's centre from the scope's
#define CARD_FLIP 16                   // and how far past the middle moves it
#define LONG_PRESS_MS 800              // held this long, still, for the settings
#define PRESS_SLOP 24                  // and moving no further than this
#define DEG2RAD ((float)M_PI / 180.0f)

// Sizes per board. They follow physical size, not resolution: pixel pitch is
// 0.107 mm on the P4 3.4C, 0.148 mm on the 2.8C and 0.0865 mm on the Tab5.
#if defined(OVERHEAD_BOARD_TAB5) // 720x1280 panel, scope 720x720 plus the list
#define HAS_LIST 1
#define LIST_ROWS_MAX 15
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
#define CARD_W (s_panel->w - 2 * PANEL_PAD)
#define CARD_H 200
#define CARD_PAD 16
#define CARD_SUB_Y 40
#define CARD_ROWS_Y 76
#define CARD_LINE_SPACE 6
#define CARD_LINE 28 // the airline line, which pushes the rest down
#define FLAG_SCALE 256 // 36x26
#define HAS_LOGO 1 // the airline's logo on a badge at the card's right
#define F_BIG lv_font_montserrat_48 // the big view: callsign, altitude, distance
#define F_BIG_MID lv_font_montserrat_32
#define F_BIG_TEXT lv_font_montserrat_24
#define BIG_POINTER 2.6f // the bearing arrow, times the aircraft arrow
#define BIG_PAD_TOP 56
#define BIG_PAD_SIDE 40
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
#define ROW_H 44
#define BATT_W 46 // battery outline, about 4 x 2 mm on the Tab5
#define BATT_H 22
#define CORNER_X 18 // clock, range, count and battery in the scope square's corners
#define CORNER_Y 14
#define QR_SIZE 240
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
#define CARD_LINE 26
#define FLAG_SCALE 256
#define HAS_LOGO 1
#define F_BIG lv_font_montserrat_48
#define F_BIG_MID lv_font_montserrat_28
#define F_BIG_TEXT lv_font_montserrat_20
#define BIG_POINTER 2.4f
#define BIG_PAD_TOP 110 // round: kept to where the circle is wide
#define BIG_PAD_SIDE 110
#define RL_ROWS 10 // the list view, for want of the Tab5's list
#define RL_ROW_H 42
#define RL_W 530
#define RL_COLS {0, 130, 220, 320, 390, 460}
#define F_RL lv_font_montserrat_20
#define F_RL_HEAD lv_font_montserrat_14
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
#define QR_SIZE 220
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
#define CARD_LINE 20
#define FLAG_SCALE 180 // 25x18 beside the smaller title
#define F_BIG lv_font_montserrat_28
#define F_BIG_MID lv_font_montserrat_20
#define F_BIG_TEXT lv_font_montserrat_14
#define BIG_POINTER 1.6f
#define BIG_PAD_TOP 66
#define BIG_PAD_SIDE 66
#define RL_ROWS 8
#define RL_ROW_H 30
#define RL_W 330
#define RL_COLS {0, 84, 136, 200, 248, 290}
#define F_RL lv_font_montserrat_14
#define F_RL_HEAD lv_font_montserrat_12
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
#define QR_SIZE 130
#endif

#define SWEEP_SEGS 8
#define SWEEP_PERIOD_MS 4000
#define SWEEP_TICK_MS 33
#define MAX_EXTRAPOLATE_S 60
#define DROP_STALE_S 90
#define TRAIL_MAX_AC 50  // the nearest; each trail is up to TRAIL_PTS lines a frame
#define TRAIL_MIN_PX 4   // closer points add nothing but draw time
#define ALERT_REARM_S 120 // an aircraft gone this long chimes again when it returns
#define ALERT_MEMORY 16

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
#define C_FIELD 0x8fb8e8
#define C_MIL 0xff6ad5
#define C_ALERT_INK 0x1a1000

typedef struct {
    int16_t x, y;
    float dist_nm;
    float brg_deg;
    bool visible;
    bool labelled;
    bool alert;         // within the overhead alert's radius and ceiling
    uint8_t trail_n;    // points in s_trail_xy, oldest first
    lv_area_t trail_box; // around the trail, for skipping it outside the clip
} ac_screen_t;

// What fills the scope's square. A swipe left goes to the next, right back.
enum { VIEW_RADAR, VIEW_BIG, VIEW_LIST };
#ifdef HAS_LIST
#define VIEW_COUNT 2 // the Tab5's list is beside the scope already
#else
#define VIEW_COUNT 3
#endif

typedef struct {
    char hex[8];
    int64_t seen_us;
} alert_memory_t;

#ifdef HAS_LIST
// The scope square is the same either way up; the list panel goes below it
// in portrait and to its right in landscape (720x560 vs 560x720).
typedef struct {
    int x, y, w, h;
    int card_cols[4];
    int list_rows;
    int list_cols[6];
} panel_layout_t;
static const panel_layout_t PANEL_PORTRAIT = {
    0, SCR_W, SCR_W, BSP_LCD_V_RES - SCR_W, {0, 70, 340, 410}, 11, {0, 170, 270, 390, 490, 600},
};
static const panel_layout_t PANEL_LANDSCAPE = {
    SCR_W, 0, BSP_LCD_V_RES - SCR_W, SCR_W, {0, 64, 268, 332}, 15, {0, 140, 222, 322, 404, 476},
};
#endif

// ---------------------------------------------------------------- shared state
//
// Each defined in the file named. In PSRAM where it is large: see
// radar_ui_create().

// radar_ui.c: the traffic, where each is on screen, and what is chosen
extern aircraft_t *s_ac;
extern ac_screen_t *s_scr;
extern int16_t *s_trail_slot;
extern int s_count;
extern int s_visible;
extern int s_in_range; // visible, plus those the filter hides
extern int *s_order;   // visible, taggable aircraft, nearest first
extern int s_order_n;
extern double s_lat0, s_lon0, s_coslat0;
extern volatile int s_range_idx;
extern char s_sel_hex[8];
extern int s_view;
extern char s_big_auto[8]; // just started alerting: the big view shows it first
extern bool s_big_auto_emerg;
extern char s_big_hex[8]; // the one the big view shows
extern int64_t s_last_update_us;
extern int s_alert_n, s_alert_best; // aircraft alerting, and the nearest
extern int s_emerg_n, s_emerg_best; // in an emergency, and the nearest
extern bool s_blink;
extern alert_memory_t s_alerted[ALERT_MEMORY], s_emerg_alerted[ALERT_MEMORY];
#ifdef HAS_LIST
extern const panel_layout_t *s_panel;
#endif

// scope.c
extern aero_map_t *s_aero;
extern int s_segs_range; // range index the airspace segments were built for; -1: stale
extern int s_bg_range;   // and the background image; -1: stale
extern lv_obj_t *s_sweep[SWEEP_SEGS];
extern lv_timer_t *s_sweep_timer;

// card.c, big_view.c, lists.c: what the views hang from
extern lv_obj_t *s_card;
extern lv_obj_t *s_big;
extern lv_obj_t *s_rlist; // the round boards' list view

// ---------------------------------------------------------------- shared functions

// radar_ui.c
lv_color_t alt_color(const aircraft_t *a);
void fmt_thousands(char *buf, size_t n, int v);
int find_aircraft(const char *hex);
bool is_selected(const aircraft_t *a);
bool is_military(const aircraft_t *a);
bool note_alert(alert_memory_t *mem, const aircraft_t *a, int64_t now);
void big_auto(const char *hex, bool emerg);
bool filtered_out(const aircraft_t *a, const settings_t *set);
void set_text_if_changed(lv_obj_t *label, const char *text);
void format_count(char *buf, size_t n);
void select_aircraft(const char *hex);
void show_view(int view);
bool swiped(void);
void on_press(lv_event_t *e);
void on_scope_long_press(lv_event_t *e);
void on_gesture(lv_event_t *e);
lv_obj_t *make_label(lv_obj_t *parent, const lv_font_t *font, uint32_t color);
lv_obj_t *make_box(lv_obj_t *parent);

// scope.c
void layout(void);
void create_background(lv_obj_t *scr);
void render_background(void);
void on_scope_draw(lv_event_t *e);
void draw_arrow_scaled(lv_layer_t *layer, int x, int y, float track_rad, lv_color_t color, float scale);
void start_trails(void);
void show_sweep(bool on);
void on_sweep(lv_timer_t *t);

// describe.c
void fmt_route(const aircraft_t *a, char *route, size_t size, char *places, size_t places_size);
void fmt_progress(const aircraft_t *a, bool full, char *out, size_t n);
void fmt_info(const aircraft_t *a, const airline_t *al, char *out, size_t n);
const airline_t *fmt_airline(const aircraft_t *a, const country_t *country, char *out, size_t n);
void fmt_sub(const aircraft_t *a, bool hex, char *sub, size_t n);
void fmt_alt(const aircraft_t *a, char *out, size_t n);
void fmt_vs(const aircraft_t *a, char *out, size_t n);

// card.c
void create_card(lv_obj_t *parent);
void update_card(void);

// big_view.c
void create_big(lv_obj_t *scr);
void update_big(void);
void open_big(const char *hex);
lv_obj_t *big_label(lv_obj_t *parent, const lv_font_t *font, uint32_t color);

// lists.c
void create_panel(lv_obj_t *scr); // the Tab5's, with the card
void update_list(void);
void create_rlist(lv_obj_t *scr); // the round boards'
void update_rlist(void);
