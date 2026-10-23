#pragma once

// Aircraft types' names by ICAO designator: "B788" is "Boeing 787-8
// Dreamliner". NULL for a designator not on the list. The table is made by
// tools/make_types.py.
const char *type_name(const char *code);
