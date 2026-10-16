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

// "LHR > JFK" for a's callsign, and in places (if not NULL) where those are,
// "London > New York"; "" while it is looked up, when nobody knows, or with
// routes off
void fmt_route(const aircraft_t *a, char *route, size_t size, char *places, size_t places_size)
{
    route[0] = '\0';
    if (places) places[0] = '\0';
    if (!settings()->show_route) return;
    route_t r;
    route_get(a->callsign, a->lat, a->lon, &r);
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
    if (!route_get(a->callsign, a->lat, a->lon, &r) || !route_progress(&r, a->lat, a->lon, &pct, &to_go)) return;
    if (!full) {
        snprintf(out, n, "%d%%", pct);
        return;
    }
    char num[16];
    fmt_thousands(num, sizeof(num), to_go);
    snprintf(out, n, "%d%% of the way  " LV_SYMBOL_BULLET "  %s NM to go", pct, num);
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

// "Wells Fargo Trust Co  •  built 2001, 25 years old": the registered owner,
// unless it is the airline the callsign already names, and when it was built
// (which only the feed knows, and only for US registrations)
void fmt_info(const aircraft_t *a, const airline_t *al, char *out, size_t n)
{
    out[0] = '\0';
    char owner[48];
    if (a->owner[0]) {
        strlcpy(owner, a->owner, sizeof(owner));
    } else {
        details_owner(a->hex, owner, sizeof(owner));
    }
    tidy_name(owner);
    if (owner[0] && !(al && strncasecmp(owner, al->name, strlen(al->name)) == 0)) strlcpy(out, owner, n);
    if (!a->year) return;
    const time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    const int age = tm.tm_year + 1900 - a->year;
    char built[40];
    if (tm.tm_year >= 120 && age >= 2) {
        snprintf(built, sizeof(built), "built %d, %d years old", a->year, age);
    } else {
        snprintf(built, sizeof(built), "built %d", a->year);
    }
    if (out[0]) strlcat(out, "  " LV_SYMBOL_BULLET "  ", n);
    strlcat(out, built, n);
}

// "BA117  •  British Airways", or the country for anything not an airline's;
// returns the airline, if any
const airline_t *fmt_airline(const aircraft_t *a, const country_t *country, char *out, size_t n)
{
    out[0] = '\0';
    const airline_t *al = airline_for_callsign(a->callsign);
    airline_flight_number(a->callsign, out, n);
    if (al) {
        if (out[0]) strlcat(out, "  " LV_SYMBOL_BULLET "  ", n);
        strlcat(out, al->name, n);
    } else if (country) {
        strlcpy(out, country->name, n);
    }
    return al;
}

// "G-STBC  •  B77W", the address when there is nothing else, and "Military"
void fmt_sub(const aircraft_t *a, bool hex, char *sub, size_t n)
{
    sub[0] = '\0';
    if (a->reg[0] && strcmp(a->reg, ac_name(a)) != 0) strlcat(sub, a->reg, n);
    if (a->type[0]) {
        if (sub[0]) strlcat(sub, "  " LV_SYMBOL_BULLET "  ", n);
        strlcat(sub, a->type, n);
    }
    if (hex || !sub[0]) {
        if (sub[0]) strlcat(sub, "  " LV_SYMBOL_BULLET "  ", n);
        strlcat(sub, a->hex, n);
    }
    if (is_military(a)) strlcat(sub, "  " LV_SYMBOL_BULLET "  Military", n);
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

