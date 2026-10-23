// Text about an aircraft, for the card, the big view and the lists: route,
// airline, registration and type, owner and age, altitude and climb

#include "radar_priv.h"

// raw with each sep as an arrow: "LHR-JFK" as "LHR > JFK"
static void arrows(const char *raw, char sep, char *out, size_t size)
{
    size_t n = 0;
    out[0] = '\0';
    for (const char *c = raw; *c && n + 6 < size; c++) {
        if (*c == sep) {
            n += snprintf(out + n, size - n, " " LV_SYMBOL_RIGHT " ");
        } else {
            out[n++] = *c;
            out[n] = '\0';
        }
    }
}

// Whether a is the aircraft someone picked: only that one spends
// AeroDataBox's credits
static bool picked(const aircraft_t *a)
{
    return s_sel_hex[0] && strcmp(a->hex, s_sel_hex) == 0;
}

// a's route: AeroDataBox's, for today's flight, else adsb.im's
static bool get_route(const aircraft_t *a, route_t *r)
{
    adb_flight_t f;
    if (adb_flight(a->callsign, a->hex, a->lat, a->lon, picked(a), &f) && f.route.codes[0]) {
        *r = f.route;
        return true;
    }
    return route_get(a->callsign, a->lat, a->lon, r);
}

// "LHR > JFK" for a's callsign, and in places (if not NULL) where those are,
// "London > New York"; "" while it is looked up, when nobody knows, or with
// routes off
void fmt_route(const aircraft_t *a, char *route, size_t size, char *places, size_t places_size)
{
    route[0] = '\0';
    if (places) places[0] = '\0';
    if (!settings()->show_route) return;
    route_t r;
    get_route(a, &r);
    arrows(r.codes, '-', route, size);
    if (places) arrows(r.places, '|', places, places_size);
}

// How far along its route a is: "23%", or in full "23% of the way  •
// 2,610 NM to go"; "" when there is no route or it doesn't fit
void fmt_progress(const aircraft_t *a, bool full, char *out, size_t n)
{
    out[0] = '\0';
    if (!settings()->show_route) return;
    route_t r;
    int pct, to_go;
    if (!get_route(a, &r) || !route_progress(&r, a->lat, a->lon, &pct, &to_go)) return;
    if (!full) {
        snprintf(out, n, "%d%%", pct);
        return;
    }
    char num[16];
    fmt_thousands(num, sizeof(num), to_go);
    snprintf(out, n, "%d%% of the way  " LV_SYMBOL_BULLET "  %s NM to go", pct, num);
}

// "18 min late", "5 min early", "on time", or for a time only scheduled,
// "scheduled"
static void fmt_delay(const adb_end_t *e, char *out, size_t n)
{
    const int d = abs(e->delay_min);
    if (!e->live) {
        strlcpy(out, "scheduled", n);
    } else if (d < 5) {
        strlcpy(out, "on time", n);
    } else if (d < 60) {
        snprintf(out, n, "%d min %s", d, e->delay_min > 0 ? "late" : "early");
    } else {
        snprintf(out, n, "%d h %d min %s", d / 60, d % 60, e->delay_min > 0 ? "late" : "early");
    }
}

// When a's flight leaves and arrives, from AeroDataBox: "Dep 10:42  Arr
// 12:05  •  9 min late", or in full a line for each end with the delay, and
// the terminal and gate it arrives at; "" when unknown
void fmt_times(const aircraft_t *a, bool full, char *out, size_t n)
{
    out[0] = '\0';
    adb_flight_t f;
    if (!adb_flight(a->callsign, a->hex, a->lat, a->lon, picked(a), &f) || (!f.dep.best[0] && !f.arr.best[0])) return;
    char delay[32];
    if (!full) {
        if (f.dep.best[0]) snprintf(out, n, "Dep %s", f.dep.best);
        if (f.arr.best[0]) {
            if (out[0]) strlcat(out, "  ", n);
            strlcat(out, "Arr ", n);
            strlcat(out, f.arr.best, n);
        }
        // How late it will be is what matters once it is up
        fmt_delay(f.arr.best[0] ? &f.arr : &f.dep, delay, sizeof(delay));
        strlcat(out, "  " LV_SYMBOL_BULLET "  ", n);
        strlcat(out, f.diverted ? "diverted" : delay, n);
        return;
    }
    size_t len = 0;
    if (f.dep.best[0]) {
        fmt_delay(&f.dep, delay, sizeof(delay));
        len += snprintf(out + len, n - len, "%s %s, %s", f.departed ? "Departed" : "Departs", f.dep.best, delay);
    }
    if (f.arr.best[0] && len < n) {
        fmt_delay(&f.arr, delay, sizeof(delay));
        len += snprintf(out + len, n - len, "%s%s%s %s, %s", len ? "\n" : "", f.diverted ? "Diverted  " LV_SYMBOL_BULLET "  " : "",
                        f.arrived ? "Arrived" : "Arrives", f.arr.best, delay);
        if (f.arr.terminal[0] && len < n) len += snprintf(out + len, n - len, "  " LV_SYMBOL_BULLET "  Terminal %s", f.arr.terminal);
        if (f.arr.gate[0] && len < n) {
            snprintf(out + len, n - len, "%sgate %s", f.arr.terminal[0] ? ", " : "  " LV_SYMBOL_BULLET "  ", f.arr.gate);
        }
    }
}

// An owner's name as the FAA's register has it, "UNITED AIRLINES INC", as
// "United Airlines Inc"; words without a vowel, "LLC", stay as they are
static void tidy_name(char *s)
{
    for (const char *c = s; *c; c++) {
        if (*c >= 'a' && *c <= 'z') return; // someone has cased it already
    }
    for (char *w = s; *w;) {
        char *end = w;
        bool vowel = false;
        while (*end && *end != ' ') {
            if (strchr("AEIOUY", *end)) vowel = true;
            end++;
        }
        if (vowel) {
            for (char *c = w + 1; c < end; c++) {
                if (*c >= 'A' && *c <= 'Z') *c += 'a' - 'A';
            }
        }
        w = *end ? end + 1 : end;
    }
}

// "Wells Fargo Trust Co  •  built 2001, 25 years old  •  186 seats": the
// registered owner, unless it is the airline the callsign already names,
// when it was built (from the feed, which only knows for US registrations,
// else from AeroDataBox) and its seats
void fmt_info(const aircraft_t *a, const airline_t *al, char *out, size_t n)
{
    adb_aircraft_t ac;
    const bool have_ac = adb_aircraft(a->hex, picked(a), &ac);
    const int year = a->year ? a->year : have_ac ? ac.built : 0;
    char seats[16] = "";
    if (have_ac && ac.seats) snprintf(seats, sizeof(seats), "%d seats", ac.seats);
    out[0] = '\0';
    char owner[48];
    if (a->owner[0]) {
        strlcpy(owner, a->owner, sizeof(owner));
    } else {
        details_owner(a->hex, owner, sizeof(owner));
    }
    tidy_name(owner);
    if (owner[0] && !(al && strncasecmp(owner, al->name, strlen(al->name)) == 0)) strlcpy(out, owner, n);
    if (year) {
        const time_t now = time(NULL);
        struct tm tm;
        localtime_r(&now, &tm);
        const int age = tm.tm_year + 1900 - year;
        char built[40];
        if (tm.tm_year >= 120 && age >= 2) {
            snprintf(built, sizeof(built), "built %d, %d years old", year, age);
        } else {
            snprintf(built, sizeof(built), "built %d", year);
        }
        if (out[0]) strlcat(out, "  " LV_SYMBOL_BULLET "  ", n);
        strlcat(out, built, n);
    }
    if (seats[0]) {
        if (out[0]) strlcat(out, "  " LV_SYMBOL_BULLET "  ", n);
        strlcat(out, seats, n);
    }
}

// "BA117  •  British Airways", or the country for anything not an airline's;
// returns the airline, if any
const airline_t *fmt_airline(const aircraft_t *a, const country_t *country, char *out, size_t n)
{
    out[0] = '\0';
    const airline_t *al = airline_for_callsign(a->callsign);
    // The number it is sold under, "BA 117", when AeroDataBox knows
    adb_flight_t f;
    if (adb_flight(a->callsign, a->hex, a->lat, a->lon, false, &f) && f.number[0]) {
        strlcpy(out, f.number, n);
    } else {
        airline_flight_number(a->callsign, out, n);
    }
    if (al) {
        if (out[0]) strlcat(out, "  " LV_SYMBOL_BULLET "  ", n);
        strlcat(out, al->name, n);
    } else if (country) {
        strlcpy(out, country->name, n);
    }
    return al;
}

// "G-STBC  •  B77W", the address when there is nothing else, and "Military".
// Without the address (the big view, which has room), the type in full,
// "Boeing 777-300ER": AeroDataBox's name for it, else ICAO's (types.h).
void fmt_sub(const aircraft_t *a, bool hex, char *sub, size_t n)
{
    sub[0] = '\0';
    if (a->reg[0] && strcmp(a->reg, ac_name(a)) != 0) strlcat(sub, a->reg, n);
    adb_aircraft_t ac;
    const char *name = hex ? NULL : type_name(a->type);
    const char *type = !hex && adb_aircraft(a->hex, false, &ac) && ac.model[0] ? ac.model : name ? name : a->type;
    if (type[0]) {
        if (sub[0]) strlcat(sub, "  " LV_SYMBOL_BULLET "  ", n);
        strlcat(sub, type, n);
    }
    if (hex || !sub[0]) {
        if (sub[0]) strlcat(sub, "  " LV_SYMBOL_BULLET "  ", n);
        strlcat(sub, a->hex, n);
    }
    if (is_military(a)) strlcat(sub, "  " LV_SYMBOL_BULLET "  Military", n);
}

// Where to look for a from the centre: "14.0 NM SSW, 4° up", the height
// above the horizon allowing for the Earth curving away (with the usual
// refraction, an Earth 7/6 as big) and taking the centre to be at sea
// level. short: "14.0 NM SSW ▲4°", for the card. Just the direction when the
// altitude isn't known or it is on the ground.
void fmt_look(const aircraft_t *a, const ac_screen_t *s, bool brief, char *out, size_t n)
{
    static const char *const POINTS[16] = {"N", "NNE", "NE", "ENE", "E", "ESE", "SE", "SSE",
                                           "S", "SSW", "SW", "WSW", "W", "WNW", "NW", "NNW"};
    const char *dir = POINTS[(int)lroundf(s->brg_deg / 22.5f) % 16];
    if (a->on_ground || a->alt_ft == AC_ALT_UNKNOWN) {
        snprintf(out, n, "%.1f NM %s", s->dist_nm, dir);
        return;
    }
    const double d_ft = s->dist_nm * 6076.12, earth_ft = 20.9e6 * 7 / 6;
    const double up_ft = a->alt_ft - d_ft * d_ft / (2 * earth_ft);
    const int elev = (int)lround(atan2(up_ft, d_ft) * 180 / M_PI);
    if (elev >= 85) {
        snprintf(out, n, "%.1f NM, %s", s->dist_nm, brief ? "overhead" : "straight up");
    } else if (elev < 0) {
        snprintf(out, n, "%.1f NM %s, %s", s->dist_nm, dir, brief ? "below" : "below the horizon");
    } else if (brief) {
        snprintf(out, n, "%.1f NM %s " LV_SYMBOL_UP "%d\xC2\xB0", s->dist_nm, dir, elev);
    } else {
        snprintf(out, n, "%.1f NM %s, %d\xC2\xB0 up", s->dist_nm, dir, elev);
    }
}

// Whether a is on the watchlist: the list itself, any military aircraft, or
// a type first seen today; which, into why ("A388", "Military", "New B39M")
bool watched(const aircraft_t *a, const settings_t *set, char *why, size_t n)
{
    if (set->watch[0] && watch_match(a, set->watch, why, n)) return true;
    if (set->watch_military && a->military) {
        if (why) strlcpy(why, "Military", n);
        return true;
    }
    if (set->watch_new_type && a->type[0] && stats_new_type(a->type)) {
        if (why) snprintf(why, n, "New %s", a->type);
        return true;
    }
    return false;
}

// The weather near the centre in two lines: "EGLL  280° 17 kt  9°" and
// "Q1008  •  Landing 27, departing 09"; false when there is none to show
bool fmt_weather(char *l1, size_t n1, char *l2, size_t n2)
{
    weather_t w;
    weather_get(&w);
    l1[0] = l2[0] = '\0';
    if (!w.valid || !settings()->show_weather) return false;
    char wind[32];
    if (w.wind_dir == WIND_CALM) {
        strlcpy(wind, "calm", sizeof(wind));
    } else {
        char gust[12] = "";
        if (w.gust_kt) snprintf(gust, sizeof(gust), "G%d", w.gust_kt);
        if (w.wind_dir == WIND_VARIABLE) {
            snprintf(wind, sizeof(wind), "variable %d%s kt", w.wind_kt, gust);
        } else {
            snprintf(wind, sizeof(wind), "%03d\xC2\xB0 %d%s kt", w.wind_dir, w.wind_kt, gust);
        }
    }
    char temp[12] = "";
    if (w.has_temp) snprintf(temp, sizeof(temp), "  %d\xC2\xB0", w.temp_c);
    snprintf(l1, n1, "%s  %s%s", w.station, wind, temp);
    char rwy[40] = "";
    if (w.landing && w.landing == w.departing) {
        snprintf(rwy, sizeof(rwy), "runway %02d", w.landing);
    } else if (w.landing && w.departing) {
        snprintf(rwy, sizeof(rwy), "landing %02d, departing %02d", w.landing, w.departing);
    } else if (w.landing) {
        snprintf(rwy, sizeof(rwy), "landing %02d", w.landing);
    } else if (w.departing) {
        snprintf(rwy, sizeof(rwy), "departing %02d", w.departing);
    }
    if (w.qnh) snprintf(l2, n2, "Q%d", w.qnh);
    if (rwy[0]) {
        if (l2[0]) strlcat(l2, "  " LV_SYMBOL_BULLET "  ", n2);
        strlcat(l2, rwy, n2);
    }
    return true;
}

// "3,450 ft", "Ground" or "-"
void fmt_alt(const aircraft_t *a, char *out, size_t n)
{
    if (a->on_ground) {
        strlcpy(out, "Ground", n);
    } else if (a->alt_ft == AC_ALT_UNKNOWN) {
        strlcpy(out, "-", n);
    } else {
        char num[16];
        fmt_thousands(num, sizeof(num), a->alt_ft);
        snprintf(out, n, "%s ft", num);
    }
}

// "+1,200 fpm" or "Level"
void fmt_vs(const aircraft_t *a, char *out, size_t n)
{
    if (a->vs_fpm == 0 || a->on_ground) {
        strlcpy(out, "Level", n);
    } else {
        char num[16];
        fmt_thousands(num, sizeof(num), a->vs_fpm);
        snprintf(out, n, "%s%s fpm", a->vs_fpm > 0 ? "+" : "", num);
    }
}

