#include "backlight.h"

#include <math.h>
#include <time.h>

#include "board.h"
#include "esp_timer.h"
#include "settings.h"

#define PREVIEW_S 8
#define DEG2RAD (M_PI / 180.0)

static int s_applied = -1;
static volatile int s_preview = -1;
static volatile int64_t s_preview_until_us;

// The sun's elevation in degrees at (lat, lon), from the NOAA low-precision
// formulas: about 0.01 degrees, far better than "is it dark" needs.
static double sun_elevation(time_t now, double lat, double lon)
{
    const double d = now / 86400.0 - 10957.5; // days since J2000.0
    const double g = (357.529 + 0.98560028 * d) * DEG2RAD;
    const double q = 280.459 + 0.98564736 * d;
    const double l = (q + 1.915 * sin(g) + 0.020 * sin(2 * g)) * DEG2RAD;
    const double e = (23.439 - 0.00000036 * d) * DEG2RAD;
    const double ra = atan2(cos(e) * sin(l), cos(l));
    const double dec = asin(sin(e) * sin(l));
    const double gmst_deg = fmod(280.46061837 + 360.98564736629 * d, 360.0);
    const double ha = gmst_deg * DEG2RAD + lon * DEG2RAD - ra;
    const double sin_el = sin(lat * DEG2RAD) * sin(dec) + cos(lat * DEG2RAD) * cos(dec) * cos(ha);
    return asin(sin_el) / DEG2RAD;
}

bool backlight_night(void)
{
    const settings_t *s = settings();
    const time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    if (tm.tm_year < 120) return false; // no time yet

    if (s->dim_mode == 1) {
        // Sunset and sunrise are when the upper limb crosses the horizon,
        // refraction included
        return sun_elevation(now, s->lat, s->lon) < -0.833;
    }
    if (s->dim_mode == 2) {
        const int m = tm.tm_hour * 60 + tm.tm_min;
        return s->dim_from <= s->dim_to ? m >= s->dim_from && m < s->dim_to
                                        : m >= s->dim_from || m < s->dim_to;
    }
    return false;
}

void backlight_update(bool alert)
{
    const settings_t *s = settings();
    int level = s->brightness;
    if (s_preview >= 0 && esp_timer_get_time() < s_preview_until_us) {
        level = s_preview;
    } else if (!alert && backlight_night()) {
        level = s->dim_brightness;
    }
    if (level != s_applied) {
        board_set_brightness(level);
        s_applied = level;
    }
}

void backlight_preview(int percent)
{
    s_preview = percent < 1 ? 1 : percent > 100 ? 100 : percent;
    s_preview_until_us = esp_timer_get_time() + PREVIEW_S * 1000000LL;
    s_applied = -1; // so the settled level goes back on afterwards
    board_set_brightness(s_preview);
}
