#include "settings_ui.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "backlight.h"
#include "battery.h"
#include "board.h"
#include "bsp/display.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "lvgl.h"
#include "net.h"
#include "openaip.h"
#include "ota.h"
#include "settings.h"
#include "zones.h"

// Sizes per board, following physical size as radar_ui.c does. The round
// panels keep everything inside the middle of the circle.
#if defined(OVERHEAD_BOARD_TAB5)
#define F_ROW lv_font_montserrat_20
#define F_HEAD lv_font_montserrat_16
#define F_TITLE lv_font_montserrat_32
#define PAD 24
#define GAP 14
#define SWITCH_W 64
#define SWITCH_H 34
#define COL_MAX 720 // landscape is 1280 wide: a column, not a sprawl
#define ROUND 0
#elif defined(OVERHEAD_BOARD_P4_34C)
#define F_ROW lv_font_montserrat_16
#define F_HEAD lv_font_montserrat_14
#define F_TITLE lv_font_montserrat_28
#define PAD 20
#define GAP 12
#define SWITCH_W 56
#define SWITCH_H 30
#define COL_MAX 800
#define ROUND 1
#else // S3 2.8C
#define F_ROW lv_font_montserrat_14
#define F_HEAD lv_font_montserrat_12
#define F_TITLE lv_font_montserrat_20
#define PAD 12
#define GAP 8
#define SWITCH_W 44
#define SWITCH_H 24
#define COL_MAX 480
#define ROUND 1
#endif

#define C_BG 0x030806
#define C_LINE 0x1c4535
#define C_TEXT 0xcfe9dc
#define C_DIM 0x7fa996
#define C_ACCENT 0x46ffa8
#define C_PRIMARY 0x2fbf84
#define C_SECONDARY 0x3d7a61

#define IDLE_CLOSE_MS 60000 // nobody touching it: back to the radar
#define BIND_MAX 48

typedef enum {
    B_BOOL,
    B_INT,    // one of values[]
    B_SLIDER, // min..max
    B_TZ,
    B_TAG,    // one bit of tag_fields
    B_ROTATION,
} bind_kind_t;

typedef struct {
    bind_kind_t kind;
    void *field; // in s_ui->edit
    const int *values;
    int n_values;
    int extra; // a value not in values[], offered as the last option; -1 none
    int bit;
    bool preview; // a brightness: show it on the backlight while dragging
    lv_obj_t *value_label;
} bind_t;

// Everything the open screen needs, in PSRAM while it is open: static
// internal RAM is what the Tab5 is short of at boot (see radar_ui_create())
typedef struct {
    settings_t edit; // the screen's working copy, sent whole each change
    bind_t bind[BIND_MAX];
    int n_bind;
    int half_hours[48];
    char hh_names[48][6];
    const char *hh_ptrs[48];
    char tz_opts[2048];
} ui_t;
static ui_t *s_ui;
static lv_obj_t *s_root, *s_col, *s_msgbox;
static lv_obj_t *s_dim_times, *s_dim_level;
static lv_timer_t *s_idle_timer;

// Status, refreshed every second while open
enum { ST_NETWORK, ST_ADDRESS, ST_UP, ST_MEMORY, ST_BATTERY, ST_MAPS, ST_COUNT };
static lv_obj_t *s_status[ST_COUNT];

// Firmware updates
static lv_obj_t *s_ota_check, *s_ota_install, *s_ota_text, *s_ota_bar;

// ---------------------------------------------------------------- applying
//
// Closing happens from the screen's own buttons' events, so the objects go
// once the event is over (the _async calls), not from under it.

static void commit(void)
{
    settings_change(&s_ui->edit, false);
}

static void show_dependent(void)
{
    lv_obj_set_hidden(s_dim_times, s_ui->edit.dim_mode != 2);
    lv_obj_set_hidden(s_dim_level, s_ui->edit.dim_mode == 0);
}

static void close_screen(void)
{
    if (s_msgbox) {
        lv_msgbox_close_async(s_msgbox);
        s_msgbox = NULL;
    }
    if (s_idle_timer) {
        lv_timer_delete(s_idle_timer);
        s_idle_timer = NULL;
    }
    if (s_root) {
        lv_obj_delete_async(s_root);
        s_root = NULL;
    }
}

// Its objects hold pointers into their ui_t until they are gone. Opened
// again before then, s_ui is already the new screen's.
static void on_root_deleted(lv_event_t *e)
{
    ui_t *ui = lv_event_get_user_data(e);
    if (s_ui == ui) {
        s_ui = NULL;
        memset(s_status, 0, sizeof(s_status));
        s_ota_check = s_ota_install = s_ota_text = s_ota_bar = NULL;
    }
    free(ui);
}

static void on_done(lv_event_t *e)
{
    close_screen();
}

static void set_if_changed(lv_obj_t *label, const char *text)
{
    if (label && strcmp(lv_label_get_text(label), text) != 0) lv_label_set_text(label, text);
}

static void fmt_uptime(char *buf, size_t n, int64_t s)
{
    const int d = s / 86400, h = s / 3600 % 24, m = s / 60 % 60;
    if (d) {
        snprintf(buf, n, "%dd %dh %dm", d, h, m);
    } else if (h) {
        snprintf(buf, n, "%dh %dm", h, m);
    } else {
        snprintf(buf, n, "%dm", m);
    }
}

static void update_status(void)
{
    char buf[96];
    char ip[16] = "";
    if (net_get_ip(ip, sizeof(ip))) {
        const int rssi = net_rssi();
        const char *q = rssi > -55 ? "excellent" : rssi > -67 ? "good" : rssi > -78 ? "fair" : "weak";
        snprintf(buf, sizeof(buf), "%s, %s (%d dBm)", settings()->ssid, q, rssi);
        set_if_changed(s_status[ST_NETWORK], buf);
        snprintf(buf, sizeof(buf), "%s  " LV_SYMBOL_BULLET "  " NET_HOSTNAME ".local", ip);
        set_if_changed(s_status[ST_ADDRESS], buf);
    } else {
        snprintf(buf, sizeof(buf), "%s, not connected", settings()->ssid);
        set_if_changed(s_status[ST_NETWORK], buf);
        set_if_changed(s_status[ST_ADDRESS], "-");
    }
    fmt_uptime(buf, sizeof(buf), esp_timer_get_time() / 1000000);
    set_if_changed(s_status[ST_UP], buf);
    snprintf(buf, sizeof(buf), "%u KB internal, %.1f MB PSRAM",
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
             heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1048576.0);
    set_if_changed(s_status[ST_MEMORY], buf);

    battery_status_t b;
    if (s_status[ST_BATTERY] && battery_get(&b)) {
        static const char *const STATES[] = {"No battery", "Checking", "External power", "Charging", "On battery"};
        if (b.state >= BATT_EXTERNAL) {
            snprintf(buf, sizeof(buf), "%s, %d%% (%.2f V)", STATES[b.state], b.percent, b.volts);
        } else {
            snprintf(buf, sizeof(buf), "%s", STATES[b.state]);
        }
        set_if_changed(s_status[ST_BATTERY], buf);
    }

    openaip_status_t o;
    openaip_get_status(&o);
    if (strcmp(o.state, "ok") == 0) {
        snprintf(buf, sizeof(buf), "%d airspaces, %d airfields (%s)", o.airspaces, o.airfields, o.countries);
    } else if (strcmp(o.state, "loading") == 0) {
        snprintf(buf, sizeof(buf), "Loading from OpenAIP");
    } else if (strcmp(o.state, "error") == 0) {
        snprintf(buf, sizeof(buf), "Failed: %s", o.detail);
    } else {
        snprintf(buf, sizeof(buf), "%s", o.detail[0] ? o.detail : "Off");
    }
    set_if_changed(s_status[ST_MAPS], buf);
}

static void update_ota(void)
{
    ota_status_t o;
    ota_get_status(&o);
    char buf[160];
    const bool checking = strcmp(o.state, "checking") == 0;
    const bool installing = strcmp(o.state, "installing") == 0;
    const bool offer = strcmp(o.state, "checked") == 0 && o.has_image &&
                       strcmp(o.latest, esp_app_get_description()->version) != 0;
    lv_obj_set_state(s_ota_check, LV_STATE_DISABLED, checking || installing);
    lv_obj_set_hidden(s_ota_install, !offer);
    lv_obj_set_hidden(s_ota_bar, !installing);
    if (checking) {
        snprintf(buf, sizeof(buf), "Checking GitHub...");
    } else if (installing) {
        snprintf(buf, sizeof(buf), "Installing %s: %d%%. Keep it powered; it restarts when done.", o.latest,
                 o.progress);
        lv_bar_set_value(s_ota_bar, o.progress, LV_ANIM_OFF);
    } else if (strcmp(o.state, "error") == 0) {
        snprintf(buf, sizeof(buf), "%s.", o.detail);
    } else if (strcmp(o.state, "checked") == 0) {
        if (!o.has_image) {
            snprintf(buf, sizeof(buf), "The latest release, %s, has no image for this board.", o.latest);
        } else if (!offer) {
            snprintf(buf, sizeof(buf), "Up to date: %s is the latest release.", o.latest);
        } else {
            char install[48];
            snprintf(install, sizeof(install), "Install %s", o.latest);
            set_if_changed(lv_obj_get_child(s_ota_install, 0), install);
            snprintf(buf, sizeof(buf), "%s was released on %s.", o.latest, o.published);
        }
    } else {
        buf[0] = '\0';
    }
    set_if_changed(s_ota_text, buf);
    lv_obj_set_hidden(s_ota_text, !buf[0]);
}

static void on_idle(lv_timer_t *t)
{
    update_status();
    update_ota();
    // Not while an update is being checked or installed: it shows the progress
    ota_status_t o;
    ota_get_status(&o);
    const bool busy = strcmp(o.state, "checking") == 0 || strcmp(o.state, "installing") == 0;
    if (!busy && lv_display_get_inactive_time(NULL) > IDLE_CLOSE_MS) close_screen();
}

static void on_ota_check(lv_event_t *e)
{
    ota_check();
    update_ota();
}

static void on_ota_confirm(lv_event_t *e)
{
    const bool go = (bool)(intptr_t)lv_event_get_user_data(e);
    lv_msgbox_close_async(s_msgbox);
    s_msgbox = NULL;
    if (go) ota_install();
    update_ota();
}

static void on_ota_install(lv_event_t *e)
{
    ota_status_t o;
    ota_get_status(&o);
    char text[200];
    snprintf(text, sizeof(text),
             "Overhead restarts when it is done. If the new firmware can't reach the network, the next restart "
             "goes back to %s.",
             esp_app_get_description()->version);
    s_msgbox = lv_msgbox_create(NULL);
    char title[48];
    snprintf(title, sizeof(title), "Install %s?", o.latest);
    lv_msgbox_add_title(s_msgbox, title);
    lv_msgbox_add_text(s_msgbox, text);
    lv_obj_t *yes = lv_msgbox_add_footer_button(s_msgbox, "Install");
    lv_obj_t *no = lv_msgbox_add_footer_button(s_msgbox, "Cancel");
    lv_obj_add_event_cb(yes, on_ota_confirm, LV_EVENT_CLICKED, (void *)(intptr_t) true);
    lv_obj_add_event_cb(no, on_ota_confirm, LV_EVENT_CLICKED, (void *)(intptr_t) false);
}

static void on_switch(lv_event_t *e)
{
    bind_t *b = lv_event_get_user_data(e);
    *(bool *)b->field = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    commit();
}

static void on_tag(lv_event_t *e)
{
    bind_t *b = lv_event_get_user_data(e);
    lv_obj_t *box = lv_event_get_target(e);
    int tags = s_ui->edit.tag_fields;
    if (lv_obj_has_state(box, LV_STATE_CHECKED)) {
        tags |= b->bit;
    } else {
        tags &= ~b->bit;
    }
    if (!tags) { // a tag has to show something
        lv_obj_add_state(box, LV_STATE_CHECKED);
        return;
    }
    s_ui->edit.tag_fields = tags;
    commit();
}

static void on_slider(lv_event_t *e)
{
    bind_t *b = lv_event_get_user_data(e);
    const int v = lv_slider_get_value(lv_event_get_target(e));
    if (lv_event_get_code(e) == LV_EVENT_VALUE_CHANGED) {
        lv_label_set_text_fmt(b->value_label, "%d%%", v);
        if (b->preview) backlight_preview(v);
        return;
    }
    *(int *)b->field = v; // released: keep it
    commit();
}

static void on_rotation_answer(lv_event_t *e)
{
    const bool restart = (bool)(intptr_t)lv_event_get_user_data(e);
    lv_msgbox_close_async(s_msgbox);
    s_msgbox = NULL;
    if (restart) {
        commit(); // the settings task sees the new orientation and restarts
    } else {
        s_ui->edit.rotation = settings()->rotation;
        close_screen(); // simplest way to put the choice back as it was
    }
}

static void on_choice(lv_event_t *e)
{
    bind_t *b = lv_event_get_user_data(e);
    const int i = lv_dropdown_get_selected(lv_event_get_target(e));
    if (b->kind == B_TZ) {
        if (i >= ZONE_COUNT) return; // the custom rule, unchanged
        strlcpy(s_ui->edit.tz, ZONES[i].posix, sizeof(s_ui->edit.tz));
        commit();
        return;
    }
    const int v = i < b->n_values ? b->values[i] : b->extra;
    *(int *)b->field = v;
    if (b->kind == B_ROTATION) {
        s_msgbox = lv_msgbox_create(NULL);
        lv_msgbox_add_title(s_msgbox, "Change orientation");
        lv_msgbox_add_text(s_msgbox, "Overhead restarts to turn the screen.");
        lv_obj_t *yes = lv_msgbox_add_footer_button(s_msgbox, "Restart");
        lv_obj_t *no = lv_msgbox_add_footer_button(s_msgbox, "Cancel");
        lv_obj_add_event_cb(yes, on_rotation_answer, LV_EVENT_CLICKED, (void *)(intptr_t) true);
        lv_obj_add_event_cb(no, on_rotation_answer, LV_EVENT_CLICKED, (void *)(intptr_t) false);
        return;
    }
    show_dependent();
    commit();
}

// ---------------------------------------------------------------- building

static bind_t *new_bind(bind_kind_t kind, void *field)
{
    bind_t *b = &s_ui->bind[s_ui->n_bind < BIND_MAX ? s_ui->n_bind++ : BIND_MAX - 1];
    *b = (bind_t){.kind = kind, .field = field, .extra = -1};
    return b;
}

static lv_obj_t *label(lv_obj_t *parent, const char *text, const lv_font_t *font, uint32_t color)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_label_set_text(l, text);
    return l;
}

static lv_obj_t *box(lv_obj_t *parent, lv_flex_flow_t flow)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_width(o, LV_PCT(100));
    lv_obj_set_height(o, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(o, flow);
    lv_obj_set_scrollable(o, false);
    return o;
}

static void section(const char *title)
{
    lv_obj_t *l = label(s_col, title, &F_HEAD, C_ACCENT);
    lv_obj_set_style_pad_top(l, GAP * 2, 0);
    lv_obj_set_style_text_letter_space(l, 1, 0);
}

// A label on the left and its control on the right
static lv_obj_t *row(lv_obj_t *parent, const char *text)
{
    lv_obj_t *r = box(parent, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(r, GAP, 0);
    lv_obj_t *l = label(r, text, &F_ROW, C_TEXT);
    lv_obj_set_flex_grow(l, 1);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    return r;
}

// A key on the left and a value on the right, for the status
static lv_obj_t *add_info(const char *key, const char *value)
{
    lv_obj_t *r = box(s_col, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_column(r, GAP, 0);
    label(r, key, &F_ROW, C_DIM);
    lv_obj_t *v = label(r, value, &F_ROW, C_TEXT);
    lv_label_set_long_mode(v, LV_LABEL_LONG_WRAP);
    lv_obj_set_flex_grow(v, 1);
    lv_obj_set_style_text_align(v, LV_TEXT_ALIGN_RIGHT, 0);
    return v;
}

static lv_obj_t *button(lv_obj_t *parent, const char *text, lv_event_cb_t cb)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    label(b, text, &F_ROW, C_BG);
    return b;
}

static void add_switch(const char *text, bool *field)
{
    lv_obj_t *sw = lv_switch_create(row(s_col, text));
    lv_obj_set_size(sw, SWITCH_W, SWITCH_H);
    if (*field) lv_obj_add_state(sw, LV_STATE_CHECKED);
    lv_obj_add_event_cb(sw, on_switch, LV_EVENT_VALUE_CHANGED, new_bind(B_BOOL, field));
}

static lv_obj_t *dropdown(lv_obj_t *parent, const char *options, int selected, bind_t *b)
{
    lv_obj_t *dd = lv_dropdown_create(parent);
    lv_obj_set_width(dd, LV_PCT(48));
    lv_obj_set_style_text_font(dd, &F_ROW, 0);
    lv_dropdown_set_options(dd, options);
    lv_dropdown_set_selected(dd, selected);
    lv_obj_t *list = lv_dropdown_get_list(dd);
    lv_obj_set_style_text_font(list, &F_ROW, 0);
    lv_obj_add_event_cb(dd, on_choice, LV_EVENT_VALUE_CHANGED, b);
    return dd;
}

// One of values[] (names[] beside them); a value the page saved that isn't
// among them is offered too, as itself
static lv_obj_t *add_choice_in(lv_obj_t *parent, const char *text, int *field, const int *values,
                               const char *const *names, int n, const char *unit)
{
    bind_t *b = new_bind(B_INT, field);
    b->values = values;
    b->n_values = n;
    char opts[640] = "";
    int sel = -1;
    for (int i = 0; i < n; i++) {
        if (i) strlcat(opts, "\n", sizeof(opts));
        strlcat(opts, names[i], sizeof(opts));
        if (values[i] == *field) sel = i;
    }
    if (sel < 0) {
        char extra[24];
        snprintf(extra, sizeof(extra), "\n%d%s", *field, unit);
        strlcat(opts, extra, sizeof(opts));
        b->extra = *field;
        sel = n;
    }
    lv_obj_t *r = row(parent, text);
    dropdown(r, opts, sel, b);
    return r;
}

static lv_obj_t *add_choice(const char *text, int *field, const int *values, const char *const *names, int n,
                            const char *unit)
{
    return add_choice_in(s_col, text, field, values, names, n, unit);
}

static lv_obj_t *add_slider(const char *text, int *field, int min, int max, bool preview)
{
    lv_obj_t *col = box(s_col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(col, GAP, 0);
    lv_obj_t *head = row(col, text);
    bind_t *b = new_bind(B_SLIDER, field);
    b->preview = preview;
    b->value_label = label(head, "", &F_ROW, C_TEXT);
    lv_label_set_text_fmt(b->value_label, "%d%%", *field);
    lv_obj_t *sl = lv_slider_create(col);
    lv_obj_set_width(sl, LV_PCT(94));
    lv_obj_set_style_margin_left(sl, LV_PCT(3), 0);
    lv_slider_set_range(sl, min, max);
    lv_slider_set_value(sl, *field, LV_ANIM_OFF);
    lv_obj_add_event_cb(sl, on_slider, LV_EVENT_VALUE_CHANGED, b);
    lv_obj_add_event_cb(sl, on_slider, LV_EVENT_RELEASED, b);
    return col;
}

static void add_timezone(void)
{
    bind_t *b = new_bind(B_TZ, s_ui->edit.tz);
    char *opts = s_ui->tz_opts;
    const size_t opts_n = sizeof(s_ui->tz_opts);
    opts[0] = '\0';
    const char *current = s_ui->edit.tz[0] ? s_ui->edit.tz : "UTC0";
    int sel = zone_find(current);
    for (int i = 0; i < ZONE_COUNT; i++) {
        if (i) strlcat(opts, "\n", opts_n);
        strlcat(opts, ZONES[i].label, opts_n);
    }
    if (sel < 0) { // typed in on the page: kept until another is picked
        strlcat(opts, "\nCustom, from the web page", opts_n);
        sel = ZONE_COUNT;
    }
    dropdown(row(s_col, "Timezone"), opts, sel, b);
}

static void add_tags(void)
{
    label(s_col, "Data tags show", &F_ROW, C_TEXT);
    lv_obj_t *r = box(s_col, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_column(r, GAP * 2, 0);
    lv_obj_set_style_pad_row(r, GAP, 0);
    static const struct {
        int bit;
        const char *name;
    } TAGS[] = {{TAG_CALLSIGN, "Callsign"}, {TAG_ALT, "Altitude"}, {TAG_SPEED, "Speed"}, {TAG_TYPE, "Type"}};
    for (size_t i = 0; i < sizeof(TAGS) / sizeof(TAGS[0]); i++) {
        lv_obj_t *cb = lv_checkbox_create(r);
        lv_checkbox_set_text(cb, TAGS[i].name);
        lv_obj_set_style_text_font(cb, &F_ROW, 0);
        lv_obj_set_style_text_color(cb, lv_color_hex(C_TEXT), 0);
        if (s_ui->edit.tag_fields & TAGS[i].bit) lv_obj_add_state(cb, LV_STATE_CHECKED);
        bind_t *b = new_bind(B_TAG, &s_ui->edit.tag_fields);
        b->bit = TAGS[i].bit;
        lv_obj_add_event_cb(cb, on_tag, LV_EVENT_VALUE_CHANGED, b);
    }
}

static void build(void)
{
    static const int RANGE_V[] = {0, 1, 2, 3, 4};
    static const char *const RANGE_N[] = {"5 NM", "10 NM", "25 NM", "50 NM", "100 NM"};
    static const int LABELS_V[] = {0, 10, 20, 40, 80, 200};
    static const char *const LABELS_N[] = {"None", "10", "20", "40", "80", "All"};
    static const int TRAIL_V[] = {0, 60, 120, 300, 600};
    static const char *const TRAIL_N[] = {"Off", "1 minute", "2 minutes", "5 minutes", "10 minutes"};
    static const int DIM_V[] = {0, 1, 2};
    static const char *const DIM_N[] = {"Never", "Sunset to sunrise", "Set times"};
    static const int CLOCK_V[] = {0, 1, 2};
    static const char *const CLOCK_N[] = {"UTC", "Local time", "Local and UTC"};
    static const int NM10_V[] = {5, 10, 20, 30, 50, 100};
    static const char *const NM10_N[] = {"0.5 NM", "1 NM", "2 NM", "3 NM", "5 NM", "10 NM"};
    static const int FT_V[] = {1000, 2000, 3000, 5000, 8000, 10000, 15000, 20000};
    static const char *const FT_N[] = {"1,000 ft", "2,000 ft", "3,000 ft", "5,000 ft",
                                       "8,000 ft", "10,000 ft", "15,000 ft", "20,000 ft"};
    static const int FEED_V[] = {0, 1};
    static const char *const FEED_N[] = {"adsb.lol", "adsb.fi"};
    static const int FETCH_V[] = {2, 3, 5, 10, 15, 30, 60};
    static const char *const FETCH_N[] = {"2 s", "3 s", "5 s", "10 s", "15 s", "30 s", "60 s"};
    static const int ROT_V[] = {0, 90, 270};
    static const char *const ROT_N[] = {"Portrait", "Landscape", "Landscape, flipped"};
    static const int MV_V[] = {6100, 6200, 6300, 6400, 6500, 6600, 6700, 6800, 6900, 7000, 7100, 7200, 7300, 7400};
    static const char *const MV_N[] = {"6.1 V", "6.2 V", "6.3 V", "6.4 V", "6.5 V", "6.6 V", "6.7 V",
                                       "6.8 V", "6.9 V", "7.0 V", "7.1 V", "7.2 V", "7.3 V", "7.4 V"};
    int *half_hours = s_ui->half_hours;
    const char **hh_ptrs = s_ui->hh_ptrs;
    for (int i = 0; i < 48; i++) {
        half_hours[i] = i * 30;
        snprintf(s_ui->hh_names[i], sizeof(s_ui->hh_names[i]), "%02d:%02d", i / 2, i % 2 * 30);
        hh_ptrs[i] = s_ui->hh_names[i];
    }

    section("STATUS");
    add_info("Board", BOARD_NAME);
    s_status[ST_NETWORK] = add_info("Network", "");
    s_status[ST_ADDRESS] = add_info("Address", "");
    s_status[ST_UP] = add_info("Up", "");
    s_status[ST_MEMORY] = add_info("Free memory", "");
    battery_status_t batt_now;
    s_status[ST_BATTERY] = battery_get(&batt_now) ? add_info("Battery", "") : NULL;
    s_status[ST_MAPS] = add_info("Map layers", "");
    update_status();

    section("DISPLAY");
    add_slider("Brightness", &s_ui->edit.brightness, 5, 100, true);
    add_choice("Dim at night", &s_ui->edit.dim_mode, DIM_V, DIM_N, 3, "");
    s_dim_times = box(s_col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_dim_times, GAP, 0);
    add_choice_in(s_dim_times, "From", &s_ui->edit.dim_from, half_hours, hh_ptrs, 48, " min");
    add_choice_in(s_dim_times, "Until", &s_ui->edit.dim_to, half_hours, hh_ptrs, 48, " min");
    s_dim_level = add_slider("Night brightness", &s_ui->edit.dim_brightness, 1, 100, true);
    add_choice("Clock", &s_ui->edit.clock, CLOCK_V, CLOCK_N, 3, "");
    add_timezone();
#if BOARD_CAN_ROTATE
    lv_obj_t *r = row(s_col, "Orientation");
    bind_t *b = new_bind(B_ROTATION, &s_ui->edit.rotation);
    b->values = ROT_V;
    b->n_values = 3;
    dropdown(r, "Portrait\nLandscape\nLandscape, flipped",
             s_ui->edit.rotation == 90 ? 1 : s_ui->edit.rotation == 270 ? 2 : 0, b);
    (void)ROT_N;
#else
    (void)ROT_V;
    (void)ROT_N;
#endif

    section("RADAR");
    add_choice("Starting range", &s_ui->edit.range_idx, RANGE_V, RANGE_N, 5, "");
    add_choice("Label the nearest", &s_ui->edit.max_labels, LABELS_V, LABELS_N, 6, "");
    add_tags();
    add_choice("Trails", &s_ui->edit.trail_s, TRAIL_V, TRAIL_N, 5, " s");
    add_switch("Direction line", &s_ui->edit.show_vector);
    add_switch("Animated sweep", &s_ui->edit.sweep);
    add_switch("Aircraft on the ground", &s_ui->edit.show_ground);
    add_switch("Highlight military", &s_ui->edit.show_military);
    add_switch("Routes in the card", &s_ui->edit.show_route);

    section("OVERHEAD ALERT");
    add_switch("Alert", &s_ui->edit.alert);
    add_choice("Within", &s_ui->edit.alert_nm10, NM10_V, NM10_N, 6, "");
    add_choice("Below", &s_ui->edit.alert_ft, FT_V, FT_N, 8, " ft");
#if BOARD_HAS_SPEAKER
    add_switch("Chime", &s_ui->edit.alert_sound);
#endif

    section("MAP LAYERS");
    add_switch("Airspace", &s_ui->edit.show_airspace);
    add_switch("Airfields", &s_ui->edit.show_airfields);
    if (!s_ui->edit.openaip_key[0]) label(s_col, "These need an OpenAIP key, set on the web page.", &F_HEAD, C_DIM);

    section("TRAFFIC FEED");
    add_choice("Primary feed", &s_ui->edit.feed, FEED_V, FEED_N, 2, "");
    add_choice("Update every", &s_ui->edit.fetch_s, FETCH_V, FETCH_N, 7, " s");

    battery_status_t batt;
    if (battery_get(&batt)) {
        section("BATTERY");
        add_switch("Power off before flat", &s_ui->edit.batt_auto_off);
        add_choice("Power off below", &s_ui->edit.batt_shutdown_mv, MV_V, MV_N, 14, " mV");
    }

    section("FIRMWARE");
    char text[160];
    snprintf(text, sizeof(text), "Running %s. Updates come from the latest release of %s on GitHub.",
             esp_app_get_description()->version, CONFIG_OVERHEAD_OTA_REPO);
    lv_obj_t *fw = label(s_col, text, &F_ROW, C_TEXT);
    lv_label_set_long_mode(fw, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(fw, LV_PCT(100));
    lv_obj_t *btns = box(s_col, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_column(btns, GAP, 0);
    lv_obj_set_style_pad_row(btns, GAP, 0);
    s_ota_check = button(btns, "Check for updates", on_ota_check);
    s_ota_install = button(btns, "Install", on_ota_install);
    s_ota_text = label(s_col, "", &F_ROW, C_TEXT);
    lv_label_set_long_mode(s_ota_text, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_ota_text, LV_PCT(100));
    s_ota_bar = lv_bar_create(s_col);
    lv_obj_set_width(s_ota_bar, LV_PCT(100));
    lv_bar_set_range(s_ota_bar, 0, 100);
    update_ota();

    // What this screen can't do, and where to do it
    char ip[16] = "";
    if (net_get_ip(ip, sizeof(ip))) {
        snprintf(text, sizeof(text), "Centre, WiFi and the OpenAIP key: http://%s or http://" NET_HOSTNAME ".local",
                 ip);
    } else {
        snprintf(text, sizeof(text), "Centre, WiFi and the OpenAIP key are on the configuration page.");
    }
    lv_obj_t *info = label(s_col, text, &F_HEAD, C_DIM);
    lv_obj_set_style_pad_top(info, GAP * 2, 0);
    lv_label_set_long_mode(info, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(info, LV_PCT(100));

    lv_obj_t *done = lv_button_create(s_col);
    lv_obj_set_width(done, LV_PCT(ROUND ? 60 : 100));
    lv_obj_set_style_margin_top(done, GAP, 0);
    lv_obj_add_event_cb(done, on_done, LV_EVENT_CLICKED, NULL);
    lv_obj_center(label(done, "Done", &F_ROW, C_BG));

    show_dependent();
}

// ---------------------------------------------------------------- public

void settings_ui_init(void)
{
    lv_display_t *disp = lv_display_get_default();
    lv_theme_t *th = lv_theme_default_init(disp, lv_color_hex(C_PRIMARY), lv_color_hex(C_SECONDARY), true, &F_ROW);
    lv_display_set_theme(disp, th);
}

void settings_ui_open(void)
{
    if (s_root) return;
    s_ui = heap_caps_calloc(1, sizeof(*s_ui), MALLOC_CAP_SPIRAM);
    if (!s_ui) return;
    s_ui->edit = *settings();

    const int w = lv_display_get_horizontal_resolution(NULL), h = lv_display_get_vertical_resolution(NULL);
    s_root = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(s_root);
    lv_obj_set_size(s_root, w, h);
    lv_obj_set_style_bg_color(s_root, lv_color_hex(C_BG), 0);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, 0);
    lv_obj_set_clickable(s_root, true); // nothing reaches the radar under it
    lv_obj_add_event_cb(s_root, on_root_deleted, LV_EVENT_DELETE, s_ui);
    lv_obj_set_flex_flow(s_root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_root, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scroll_dir(s_root, LV_DIR_VER);
#if ROUND
    // The circle: rows stay in its middle, where it is widest
    lv_obj_set_style_pad_hor(s_root, w * 16 / 100, 0);
    lv_obj_set_style_pad_top(s_root, h * 12 / 100, 0);
    lv_obj_set_style_pad_bottom(s_root, h * 18 / 100, 0);
    lv_obj_set_scrollbar_mode(s_root, LV_SCROLLBAR_MODE_OFF);
#else
    lv_obj_set_style_pad_all(s_root, PAD, 0);
#endif

    s_col = lv_obj_create(s_root);
    lv_obj_remove_style_all(s_col);
    lv_obj_set_width(s_col, LV_PCT(100));
    lv_obj_set_style_max_width(s_col, COL_MAX - 2 * PAD, 0);
    lv_obj_set_height(s_col, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s_col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_col, LV_FLEX_ALIGN_START, ROUND ? LV_FLEX_ALIGN_CENTER : LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(s_col, GAP, 0);
    lv_obj_set_scrollable(s_col, false);

    lv_obj_t *head = box(s_col, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(head, ROUND ? LV_FLEX_ALIGN_CENTER : LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    label(head, "Settings", &F_TITLE, C_ACCENT);
#if !ROUND
    lv_obj_t *done = lv_button_create(head);
    lv_obj_add_event_cb(done, on_done, LV_EVENT_CLICKED, NULL);
    label(done, "Done", &F_ROW, C_BG);
#endif
    label(s_col, "Changes apply as you make them.", &F_HEAD, C_DIM);

    build();
    s_idle_timer = lv_timer_create(on_idle, 1000, NULL);
}
