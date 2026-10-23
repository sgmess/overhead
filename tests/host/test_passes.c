// main/core/passes.c on the PC: the log of aircraft overhead in a fake
// flash partition, through restarts, round its ring, over another's data,
// and after a write a power cut stopped.

#include "fake.h"
#define time(x) fake_time(x)
#define CONFIG_SPIRAM_XIP_FROM_PSRAM 1 // so it uses flash
#include "passes.c"

#include <math.h>
#include <stdlib.h>

#define SMALL (16 * SECTOR) // 2,048 passes

// As at boot: the module's state gone, the flash as it was
static void reboot(void)
{
    s_lock = NULL;
    s_part = NULL;
    s_base = 0;
    s_recs = s_head = s_count = 0;
    s_seq = 0;
    s_oldest = 0;
    s_day = -1;
    s_n = s_next = 0;
    passes_update(NULL, 0, 51.5, -0.1); // opens the log and reloads today's
}

static aircraft_t over(const char *hex, const char *callsign, int alt)
{
    aircraft_t a = {.lat = 51.5, .lon = -0.1, .alt_ft = alt};
    strcpy(a.hex, hex);
    strcpy(a.callsign, callsign);
    strcpy(a.type, "A388");
    strcpy(a.reg, "G-XLEA");
    return a;
}

// One aircraft passes overhead, then is gone a while
static void pass_over(const char *hex, const char *callsign)
{
    aircraft_t a = over(hex, callsign, 3000);
    passes_update(&a, 1, 51.5, -0.1);
    g_now += 120;
    g_now_us += 120 * 1000000LL;
    passes_update(NULL, 0, 51.5, -0.1);
}

static int sorted_ok, each_n, newer_seen;
static time_t each_last;
static bool overwrite_in_each;
static void each(const pass_t *p, void *ctx)
{
    (void)ctx;
    if (p->at < each_last) sorted_ok = 0;
    each_last = p->at;
    each_n++;
    if (p->name[0] == 'N') newer_seen++;
    if (overwrite_in_each && each_n == 1) {
        // As the fetch task would meanwhile: enough to come round to the start
        for (int k = 0; k < 3 * PER_SECTOR; k++) {
            pass_t n = {.alt_ft = 1, .dist_nm = 1, .at = 1792000000 + k}; // timed earlier, so out of order if read
            snprintf(n.name, sizeof(n.name), "N%d", k % 1000);
            log_write(&n);
        }
    }
}

int main(void)
{
    setenv("TZ", "UTC0", 1);
    tzset();
    g_set.alert_nm10 = 20;
    g_set.alert_ft = 5000;
    g_now = 1792800000; // 23 Oct 2026, 00:00 UTC
    uint8_t *flash = malloc(SMALL);
    memset(flash, 0xff, SMALL);
    g_parts[0] = (esp_partition_t){.label = "passes", .size = SMALL, .mem = flash};

    printf("Through a restart\n");
    reboot();
    pass_over("400001", "BAW1");
    passes_log_t log;
    passes_log_info(&log);
    CHECK(log.count == 1 && log.capacity == SMALL / 32, "logged: %d of %d", log.count, log.capacity);
    reboot();
    static pass_t p[3000];
    const time_t midnight = g_now - g_now % 86400;
    int n = passes_since(midnight, p, 3000);
    CHECK(n == 1 && strcmp(p[0].name, "BAW1") == 0 && strcmp(p[0].reg, "G-XLEA") == 0 && p[0].alt_ft == 3000 &&
              strcmp(p[0].type, "A388") == 0,
          "today's back after it: %d, %s %s %s", n, n ? p[0].name : "", n ? p[0].reg : "", n ? p[0].type : "");
    // Overhead as it restarts: logged by the shutdown handler
    aircraft_t a = over("400002", "BAW2", 2500);
    passes_update(&a, 1, 51.5, -0.1);
    g_shutdown();
    reboot();
    n = passes_since(midnight, p, 3000);
    CHECK(n == 2 && strcmp(p[0].name, "BAW2") == 0, "one overhead at the restart is logged too (%d)", n);
    // The next day starts empty, the log keeps both
    g_now += 86400;
    reboot();
    passes_log_info(&log);
    CHECK(passes_since(midnight + 86400, p, 3000) == 0 && log.count == 2, "the next day: none today, %d in the log",
          log.count);
    n = passes_since(g_now - 2 * 86400, p, PASSES_MAX);
    CHECK(n == 2 && strcmp(p[0].name, "BAW2") == 0, "the last two days: %d, newest first", n);

    printf("Round the ring\n");
    for (int k = 0; k < SMALL / 32 + 300; k++) {
        pass_t q = {.alt_ft = 1000, .dist_nm = 0.5f, .at = g_now + k};
        snprintf(q.name, sizeof(q.name), "T%d", k % 100000);
        log_write(&q);
    }
    passes_log_info(&log);
    CHECK(log.count <= log.capacity && log.count >= log.capacity - PER_SECTOR, "full: %d of %d", log.count,
          log.capacity);
    sorted_ok = 1;
    each_n = 0;
    each_last = 0;
    passes_each(each, NULL);
    CHECK(each_n == log.count && sorted_ok, "the download: %d, oldest first", each_n);
    reboot();
    passes_log_info(&log);
    CHECK(log.count == each_n, "after a restart: %d", log.count);
    n = passes_since(0, p, 3);
    CHECK(n == 3 && strcmp(p[0].name, "T2347") == 0, "the newest first: %s", n ? p[0].name : "");

    printf("A power cut part way through a write\n");
    const int head = s_head;
    flash[head * 32] = 0x12; // a record begun and never finished
    flash[head * 32 + 4] = 0x00;
    reboot();
    CHECK(s_head % PER_SECTOR == 0 && s_head != head, "it starts on the next sector (%d, not %d)", s_head, head);
    const int before = s_count;
    pass_over("400003", "BAW3");
    reboot();
    passes_log_info(&log);
    n = passes_since(0, p, 3000);
    bool found3 = false;
    for (int k = 0; k < n; k++) found3 |= strcmp(p[k].name, "BAW3") == 0;
    CHECK(found3 && log.count >= before - PER_SECTOR, "and logs on: BAW3 among %d, %d logged", n, log.count);

    printf("Gaps, late passes and a full ring (the second review)\n");
    memset(flash, 0xff, SMALL);
    reboot();
    const time_t t0 = g_now;
    pass_t q = {.alt_ft = 1000, .dist_nm = 0.5f};
    for (int k = 0; k < 3; k++) {
        q.at = t0 + k;
        snprintf(q.name, sizeof(q.name), "G%d", k % 10);
        log_write(&q);
    }
    flash[s_head * 32] = 0x12; // a power cut part way through the fourth
    reboot();
    q.at = t0 + 10;
    strcpy(q.name, "G3");
    log_write(&q);
    n = passes_since(0, p, 3000);
    CHECK(n == 4 && strcmp(p[0].name, "G3") == 0 && strcmp(p[3].name, "G0") == 0,
          "a gap from a power cut hides nothing: %d of 4", n);
    each_n = 0;
    passes_each(each, NULL);
    CHECK(each_n == 4, "nor from the download: %d", each_n);

    // Logged as they end, timed at their closest: A, then B from earlier, then C
    memset(flash, 0xff, SMALL);
    reboot();
    const time_t t1 = g_now + 1000;
    const struct {
        const char *name;
        int dt;
    } LATE[] = {{"A", 50}, {"B", 10}, {"C", 60}};
    for (int k = 0; k < 3; k++) {
        q.at = t1 + LATE[k].dt;
        strcpy(q.name, LATE[k].name);
        log_write(&q);
    }
    n = passes_since(t1 + 40, p, 3000);
    CHECK(n == 2 && strcmp(p[0].name, "C") == 0 && strcmp(p[1].name, "A") == 0,
          "since a time: one logged after an earlier one isn't missed (%d), newest first", n);

    // More than RAM's 200 today
    memset(flash, 0xff, SMALL);
    reboot();
    for (int k = 0; k < 201; k++) {
        q.at = midnight + 3600 + k;
        snprintf(q.name, sizeof(q.name), "D%d", k % 1000);
        log_write(&q);
    }
    n = passes_since(midnight, p, 3000);
    CHECK(n == 201, "today: all %d from flash, not RAM's 200", n);

    // A full ring with the next write exactly on a sector boundary: the
    // sector there holds the oldest
    memset(flash, 0xff, SMALL);
    reboot();
    for (int k = 0; k < SMALL / 32; k++) {
        q.at = t0 + k;
        snprintf(q.name, sizeof(q.name), "R%d", k % 100000);
        log_write(&q);
    }
    CHECK(s_head % PER_SECTOR == 0, "the next write on a sector boundary (%d)", s_head);
    sorted_ok = 1;
    each_n = 0;
    each_last = 0;
    passes_each(each, NULL);
    CHECK(sorted_ok && each_n == SMALL / 32, "the download still oldest first: %d", each_n);

    printf("The third review\n");
    // A registration of 8
    memset(flash, 0xff, SMALL);
    reboot();
    pass_t r8 = {.alt_ft = 9000, .dist_nm = 3.4f, .at = t0};
    strcpy(r8.name, "AFL1234");
    strcpy(r8.reg, "RA-96017");
    strcpy(r8.type, "IL96");
    log_write(&r8);
    n = passes_since(0, p, 3000);
    CHECK(n == 1 && strcmp(p[0].reg, "RA-96017") == 0 && fabsf(p[0].dist_nm - 3.4f) < 0.01f,
          "an 8-character registration kept: %s, %.1f NM", n ? p[0].reg : "", n ? p[0].dist_nm : 0);
    // A record from before, as the first build wrote them, still read
    rec_t v1 = {.seq_dist = 5, .at = (uint32_t)t0 - 100, .alt_ft = 4000};
    memcpy(v1.type, "B738", 4);
    memcpy(v1.name, "RYR1", 4);
    memcpy(v1.reg, "EI-DCL", 6);
    v1.reg[7] = 27; // its distance, 2.7 NM, in the last byte
    v1.crc = rec_crc(&v1, CRC_SEED_V1);
    memcpy(flash + 100 * 32, &v1, sizeof(v1));
    reboot();
    n = passes_since(0, p, 3000);
    CHECK(n == 2 && strcmp(p[1].name, "RYR1") == 0 && strcmp(p[1].reg, "EI-DCL") == 0 && fabsf(p[1].dist_nm - 2.7f) < 0.01f,
          "an old record read: %s %s %.1f NM", n > 1 ? p[1].name : "", n > 1 ? p[1].reg : "", n > 1 ? p[1].dist_nm : 0);
    CHECK(s_seq >= 5, "numbering carries on past the old ones (%u)", (unsigned)s_seq);

    // The oldest, after one logged late with an earlier time
    pass_t early = r8;
    early.at = t0 - 5000;
    log_write(&early);
    passes_log_info(&log);
    CHECK(log.oldest == t0 - 5000, "the log's oldest follows a pass logged late");

    // The ring comes round under a download: what it finds first is written
    // over while each() runs, and the newer records mustn't stand in
    memset(flash, 0xff, SMALL);
    reboot();
    for (int k = 0; k < SMALL / 32 - PER_SECTOR; k++) {
        q.at = t0 + k;
        snprintf(q.name, sizeof(q.name), "O%d", k % 10000);
        log_write(&q);
    }
    sorted_ok = 1;
    each_n = 0;
    each_last = 0;
    overwrite_in_each = true;
    passes_each(each, NULL);
    overwrite_in_each = false;
    CHECK(sorted_ok && newer_seen == 0, "no newer record in its place: %d listed, %d newer", each_n, newer_seen);

    printf("In the end of the OpenAIP cache\n");
    const size_t aip = AIPCACHE_LOG_AT + SMALL;
    uint8_t *aipmem = malloc(aip);
    srand(1);
    for (size_t k = 0; k < aip; k++) aipmem[k] = (uint8_t)rand(); // the map's old data
    g_parts[0].size = 0;
    g_parts[1] = (esp_partition_t){.label = "aipcache", .size = aip, .mem = aipmem};
    uint8_t *map = malloc(AIPCACHE_LOG_AT);
    memcpy(map, aipmem, AIPCACHE_LOG_AT);
    reboot();
    passes_log_info(&log);
    CHECK(log.count == 0 && log.capacity == SMALL / 32, "nothing read from the map's data: %d", log.count);
    pass_over("400004", "BAW4");
    pass_over("400005", "BAW5");
    reboot();
    passes_log_info(&log);
    CHECK(log.count == 2, "logged after it: %d", log.count);
    CHECK(memcmp(map, aipmem, AIPCACHE_LOG_AT) == 0, "the map's part untouched");
    return fake_done();
}
