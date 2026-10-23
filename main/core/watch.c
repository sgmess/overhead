#include "watch.h"

#include <ctype.h>
#include <string.h>

// s against the entry e (upper case, len long, prefix when it ended in
// '*'), ignoring case and, for registrations, dashes
static bool field_matches(const char *s, const char *e, size_t len, bool prefix)
{
    size_t k = 0;
    for (; *s && k < len; s++) {
        if (*s == '-') continue;
        if (toupper((unsigned char)*s) != e[k]) return false;
        k++;
    }
    while (*s == '-') s++;
    return k == len && (prefix || !*s);
}

bool watch_match(const aircraft_t *a, const char *list, char *why, size_t n)
{
    char e[24];
    for (const char *p = list; *p;) {
        while (*p == ',' || *p == ' ') p++;
        size_t len = 0;
        for (; *p && *p != ',' && *p != ' '; p++) {
            if (*p == '-') continue; // registrations match with or without
            if (len + 1 < sizeof(e)) e[len++] = (char)toupper((unsigned char)*p);
        }
        e[len] = '\0';
        const bool prefix = len && e[len - 1] == '*';
        if (prefix) e[--len] = '\0';
        if (!len) continue; // "", or "*" alone, which would match everything
        if (field_matches(a->type, e, len, prefix) || field_matches(a->reg, e, len, prefix) ||
            field_matches(a->callsign, e, len, prefix) || field_matches(a->hex, e, len, prefix)) {
            if (why) {
                strncpy(why, e, n - 1);
                why[n - 1] = '\0';
                if (prefix && strlen(why) + 1 < n) strcat(why, "*");
            }
            return true;
        }
    }
    return false;
}
