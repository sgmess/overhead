// main/core/types.c on the PC: names by ICAO designator, by binary search,
// so the table must be in order.

#include "fake.h"
#include "types.c"

int main(void)
{
    printf("Type names\n");
    CHECK(type_name("B788") && strcmp(type_name("B788"), "Boeing 787-8 Dreamliner") == 0, "B788: %s", type_name("B788"));
    CHECK(type_name("A20N") && strcmp(type_name("A20N"), "Airbus A320neo") == 0, "A20N: %s", type_name("A20N"));
    CHECK(type_name("DH8D") && strncmp(type_name("DH8D"), "de Havilland", 12) == 0, "DH8D: %s", type_name("DH8D"));
    CHECK(!type_name("ZZZZ") && !type_name("") && !type_name("GND"), "unknown codes, and airport vehicles: none");
    int found = 0, n = sizeof(TYPES) / sizeof(TYPES[0]);
    for (int i = 0; i < n; i++) found += type_name(TYPES[i].code) == TYPES[i].name;
    CHECK(found == n, "every one of the %d found: the table is in order", n);
    return fake_done();
}
