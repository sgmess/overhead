// main/core/stats.c on the PC: the type book across a clock change, and
// the counts saved and loaded again, from this format and the one before.

#include "fake.h"
#define time(x) fake_time(x)
#define CONFIG_SPIRAM_XIP_FROM_PSRAM 1 // so it saves to flash
#include "stats.c"

#include <stdlib.h>

#define PART (8 * SLOT_SIZE)

// As at boot: the module's state gone, the flash as it was
static void reboot(void)
{
    s_lock = NULL;
    s_day = -1;
    s_slot = -1;
    s_seq = 0;
    s_last_hour = -1;
    s_dirty = false;
    s_saved_us = 0;
    memset(&s_today, 0, sizeof(s_today));
    memset(&s_history, 0, sizeof(s_history));
}

// Local time, as the device's clock would be
static time_t local(int y, int mo, int d, int h)
{
    struct tm tm = {.tm_year = y - 1900, .tm_mon = mo - 1, .tm_mday = d, .tm_hour = h, .tm_isdst = -1};
    return mktime(&tm);
}

static void see(const char *hex, const char *type)
{
    aircraft_t a = {.lat = 51.5, .lon = -0.1, .alt_ft = 30000, .gs_kt = 400};
    strcpy(a.hex, hex);
    strcpy(a.type, type);
    strcpy(a.callsign, "TST1");
    stats_update(&a, 1, 51.5, -0.1, 25);
}

int main(void)
{
    uint8_t *flash = malloc(PART);
    memset(flash, 0xff, PART);
    g_parts[0] = (esp_partition_t){.label = "stats", .size = PART, .mem = flash};

    printf("New Zealand's clocks going forward\n");
    setenv("TZ", "NZST-12NZDT,M9.5.0,M4.1.0/3", 1);
    tzset();
    // The book begins a week before; the clocks go forward on Sunday 27 Sep 2026
    g_now = local(2026, 9, 20, 12);
    reboot();
    see("400001", "A320");
    g_now = local(2026, 9, 27, 10);
    see("400002", "B748");
    CHECK(stats_new_type("B748"), "a type first seen on the 27th is new that day");
    g_now = local(2026, 9, 28, 10);
    see("400003", "A320");
    CHECK(!stats_new_type("B748"), "and not on the 28th, the next date, across the change");
    struct tm a = {.tm_year = 126, .tm_mon = 8, .tm_mday = 27}, b = a;
    b.tm_mday = 28;
    CHECK(day_no(&b) == day_no(&a) + 1, "consecutive dates, consecutive numbers (%u, %u)", day_no(&a), day_no(&b));
    stats_t st;
    stats_get(&st);
    char date[16] = "";
    struct tm first;
    for (int k = 0; k < STATS_RARE_TYPES && st.rare[k].days; k++) {
        if (strcmp(st.rare[k].type, "A320")) continue;
        localtime_r(&st.rare[k].first, &first);
        strftime(date, sizeof(date), "%d %b", &first);
    }
    CHECK(strcmp(date, "20 Sep") == 0, "the A320's first day shown as the right date: %s", date);

    printf("Saved and loaded\n");
    setenv("TZ", "GMT0BST,M3.5.0/1,M10.5.0", 1);
    tzset();
    memset(flash, 0xff, PART);
    g_now = local(2026, 10, 23, 9);
    reboot();
    see("400011", "A388");
    see("400012", "A388");
    see("400013", "B788");
    save();
    reboot();
    g_now += 60;
    see("400013", "B788"); // the first count after the restart loads the save
    stats_get(&st);
    CHECK(st.seen == 3, "today's aircraft back: %d", st.seen);
    CHECK(st.types_today == 2 && !strcmp(st.types[0].type, "A388") && st.types[0].count == 2,
          "today's types back: %d, %s x%d", st.types_today, st.types[0].type, st.types[0].count);
    CHECK(st.types_known == 2, "the book back: %d", st.types_known);

    // A save from before types (version 2): the counts load, without them
    const size_t at = (size_t)s_slot * SLOT_SIZE;
    save_hdr_t h;
    memcpy(&h, flash + at, sizeof(h));
    h.version = 2;
    h.crc = save_crc(&h, (const uint32_t *)(flash + at + sizeof(h)), NULL);
    memcpy(flash + at, &h, sizeof(h));
    memset(s_types, 0, sizeof(*s_types));
    reboot();
    g_now += 60;
    see("400013", "B788");
    stats_get(&st);
    CHECK(st.seen == 3, "a version 2 save still loads: %d aircraft", st.seen);
    return fake_done();
}
