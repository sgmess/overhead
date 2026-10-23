// main/core/watch.c on the PC: which aircraft a watchlist matches.

#include "fake.h"
#include "watch.c"

static bool m(const char *list, const char *type, const char *reg, const char *callsign, const char *hex,
              char *why)
{
    aircraft_t a = {0};
    strcpy(a.type, type);
    strcpy(a.reg, reg);
    strcpy(a.callsign, callsign);
    strcpy(a.hex, hex);
    return watch_match(&a, list, why, 24);
}

int main(void)
{
    printf("Watchlist\n");
    char why[24] = "";
    CHECK(m("A388", "A388", "A6-EVL", "UAE2", "896431", why) && strcmp(why, "A388") == 0, "a type: %s", why);
    CHECK(m("b748, a388", "A388", "", "", "", why) && strcmp(why, "A388") == 0, "any case, the second entry");
    CHECK(m("G-XLEA", "A388", "G-XLEA", "BAW11", "4007f7", why), "a registration with its dash");
    CHECK(m("GXLEA", "A388", "G-XLEA", "BAW11", "4007f7", why), "and without");
    CHECK(m("g-xlea", "A388", "GXLEA", "BAW11", "4007f7", why), "a dash in the entry only");
    CHECK(m("RRR*", "HAWK", "XX177", "RRR1", "43c6f1", why) && strcmp(why, "RRR*") == 0, "a callsign prefix: %s",
          why);
    CHECK(!m("RRR", "HAWK", "XX177", "RRR1", "43c6f1", why), "no '*': the whole callsign");
    CHECK(m("43C6F1", "HAWK", "XX177", "RRR1", "43c6f1", why), "an ICAO address, either case");
    CHECK(!m("A38", "A388", "", "", "", why), "no partial match without '*'");
    CHECK(!m("", "A388", "G-XLEA", "BAW11", "4007f7", why), "an empty list");
    CHECK(!m("*, ,,", "A388", "G-XLEA", "BAW11", "4007f7", why), "'*' alone matches nothing");
    CHECK(!m("B748", "", "", "", "", why), "an aircraft with nothing known");
    CHECK(m("  B748 ,  A388  ", "A388", "", "", "", why), "spaces around entries");
    return fake_done();
}
