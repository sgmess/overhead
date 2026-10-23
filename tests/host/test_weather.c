// main/sources/weather.c on the PC: reading METARs, choosing the nearest
// station, and the runway in use from the traffic.

#include "fake.h"
#include "weather.c"

#include <math.h>
#include <stdlib.h>

// As aviationweather.gov sent them on 23 Oct 2026, the first trimmed
static const char NEAR[] =
    "[{\"icaoId\":\"EGLL\",\"obsTime\":1792786800,\"temp\":9,\"dewp\":4,\"wdir\":280,\"wspd\":17,"
    "\"altim\":1008,\"rawOb\":\"METAR EGLL 232020Z AUTO 28017KT 9999 NCD 09/04 Q1008\",\"lat\":51.477,"
    "\"lon\":-0.461,\"elev\":26,\"name\":\"London/Heathrow Intl, EN, GB\"},"
    "{\"icaoId\":\"EGLC\",\"obsTime\":1792786800,\"temp\":10,\"dewp\":4,\"wdir\":\"VRB\",\"wspd\":3,\"wgst\":15,"
    "\"altim\":1007,\"rawOb\":\"METAR EGLC 232020Z VRB03G15KT\",\"lat\":51.505,\"lon\":0.055,\"elev\":5,"
    "\"name\":\"London City Arpt, EN, GB\"},"
    "{\"icaoId\":\"EGWU\",\"wdir\":0,\"wspd\":0,\"lat\":51.549,\"lon\":-0.417,\"name\":\"Northolt, EN, GB\"},"
    "{\"icaoId\":\"BADX\",\"lat\":1e999,\"lon\":0}]";

static aircraft_t ac(const char *hex, double lat, double lon, int alt, float track, int vs)
{
    aircraft_t a = {.lat = lat, .lon = lon, .alt_ft = alt, .track_deg = track, .vs_fpm = vs, .gs_kt = 150};
    strcpy(a.hex, hex);
    return a;
}

int main(void)
{
    s_resp = malloc(sizeof(*s_resp));
    s_arr = calloc(SAMPLES, sizeof(*s_arr));
    s_dep = calloc(SAMPLES, sizeof(*s_dep));
    s_lock = xSemaphoreCreateMutex();
    weather_t w;
    double lat, lon;
    int elev;

    printf("Reading METARs\n");
    g_http = (struct fake_http){.body = NEAR, .status = 200};
    CHECK(fetch("", 51.47, -0.45, &w, &lat, &lon, &elev) && strcmp(w.station, "EGLL") == 0,
          "the nearest to Heathrow: %s", w.station);
    CHECK(strcmp(w.name, "London/Heathrow Intl") == 0 && w.wind_dir == 280 && w.wind_kt == 17 && !w.gust_kt &&
              w.has_temp && w.temp_c == 9 && w.qnh == 1008 && elev == 85,
          "wind %d/%d, %d C, Q%d, %d ft", w.wind_dir, w.wind_kt, w.temp_c, w.qnh, elev);
    CHECK(fetch("", 51.51, 0.06, &w, &lat, &lon, &elev) && strcmp(w.station, "EGLC") == 0 &&
              w.wind_dir == WIND_VARIABLE && w.wind_kt == 3 && w.gust_kt == 15,
          "London City: variable %d, gusting %d", w.wind_kt, w.gust_kt);
    CHECK(fetch("", 51.55, -0.42, &w, &lat, &lon, &elev) && strcmp(w.station, "EGWU") == 0 &&
              w.wind_dir == WIND_CALM && !w.has_temp && !w.qnh,
          "Northolt: calm, no temperature or pressure");
    CHECK(strstr(g_http.last_url, "bbox=") != NULL, "nearest: asked by area");
    fetch("EGLL", 0, 0, &w, &lat, &lon, &elev);
    CHECK(strstr(g_http.last_url, "ids=EGLL") != NULL, "a set station: asked by name");
    g_http = (struct fake_http){.body = "[]", .status = 200};
    CHECK(!fetch("", 0, 0, &w, &lat, &lon, &elev) && strstr(w.detail, "No station"), "none near: %s", w.detail);
    g_http = (struct fake_http){.status = 500};
    CHECK(!fetch("EGLL", 0, 0, &w, &lat, &lon, &elev), "an error: none");

    printf("The runway in use\n");
    s_st_known = true;
    s_st_lat = 51.477;
    s_st_lon = -0.461;
    s_st_elev_ft = 85;
    // Landing west: east of the airfield, descending, heading 270ish
    aircraft_t list[] = {ac("400001", 51.47, -0.38, 1800, 268, -700), ac("400002", 51.47, -0.35, 2500, 271, -800),
                         // Taking off west: climbing out beyond it
                         ac("400003", 51.48, -0.52, 1500, 272, 2000), ac("400004", 51.48, -0.53, 2200, 270, 1800),
                         // Ignored: too high, too far, on the ground, level
                         ac("400005", 51.47, -0.40, 6000, 90, -700), ac("400006", 51.9, -0.4, 1500, 90, -700),
                         ac("400007", 51.47, -0.45, 1500, 90, 0)};
    list[6].on_ground = false;
    g_now_us = 1000000000;
    weather_traffic(list, 1);
    CHECK(s_wx.landing == 0, "one arrival isn't enough");
    weather_traffic(list, 7);
    CHECK(s_wx.landing == 27 && s_wx.departing == 27, "landing %02d, departing %02d", s_wx.landing, s_wx.departing);
    // The wind changes: arrivals from the west now, and the old ones age out
    g_now_us += SAMPLE_KEEP_US + 1000000;
    aircraft_t east[] = {ac("400011", 51.47, -0.55, 1800, 88, -700), ac("400012", 51.47, -0.57, 2500, 92, -800)};
    weather_traffic(east, 2);
    CHECK(s_wx.landing == 9 && s_wx.departing == 0, "after the change: landing %02d, departures unknown",
          s_wx.landing);
    // Arrivals that disagree say nothing
    aircraft_t mixed[] = {ac("400021", 51.47, -0.38, 1800, 270, -700)};
    weather_traffic(mixed, 1);
    CHECK(s_wx.landing == 0, "tracks that disagree: unknown");
    return fake_done();
}
