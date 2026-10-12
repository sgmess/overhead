#include "zones.h"

#include <string.h>

const zone_t ZONES[] = {
    {"UTC", "UTC", "UTC0"},
    {"Europe/London", "London, Edinburgh (GMT/BST)", "GMT0BST,M3.5.0/1,M10.5.0"},
    {"Europe/Dublin", "Dublin (GMT/IST)", "IST-1GMT0,M10.5.0,M3.5.0/1"},
    {"Europe/Lisbon", "Lisbon, Canaries (WET/WEST)", "WET0WEST,M3.5.0/1,M10.5.0"},
    {"Europe/Paris", "Paris, Berlin, Madrid, Rome (CET/CEST)", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Athens", "Athens, Helsinki, Kyiv (EET/EEST)", "EET-2EEST,M3.5.0/3,M10.5.0/4"},
    {"Europe/Istanbul", "Istanbul (+03)", "<+03>-3"},
    {"Europe/Moscow", "Moscow (MSK)", "MSK-3"},
    {"Asia/Jerusalem", "Jerusalem (IST/IDT)", "IST-2IDT,M3.4.4/26,M10.5.0"},
    {"Asia/Dubai", "Dubai (+04)", "<+04>-4"},
    {"Asia/Karachi", "Karachi (PKT)", "PKT-5"},
    {"Asia/Kolkata", "India (IST)", "IST-5:30"},
    {"Asia/Bangkok", "Bangkok, Jakarta (+07)", "<+07>-7"},
    {"Asia/Singapore", "Singapore, Kuala Lumpur (+08)", "<+08>-8"},
    {"Asia/Hong_Kong", "Hong Kong (HKT)", "HKT-8"},
    {"Asia/Shanghai", "China (CST)", "CST-8"},
    {"Asia/Tokyo", "Tokyo (JST)", "JST-9"},
    {"Asia/Seoul", "Seoul (KST)", "KST-9"},
    {"Australia/Perth", "Perth (AWST)", "AWST-8"},
    {"Australia/Darwin", "Darwin (ACST)", "ACST-9:30"},
    {"Australia/Adelaide", "Adelaide (ACST/ACDT)", "ACST-9:30ACDT,M10.1.0,M4.1.0/3"},
    {"Australia/Brisbane", "Brisbane (AEST)", "AEST-10"},
    {"Australia/Sydney", "Sydney, Melbourne (AEST/AEDT)", "AEST-10AEDT,M10.1.0,M4.1.0/3"},
    {"Pacific/Auckland", "Auckland (NZST/NZDT)", "NZST-12NZDT,M9.5.0,M4.1.0/3"},
    {"Pacific/Honolulu", "Hawaii (HST)", "HST10"},
    {"America/Anchorage", "Alaska (AKST/AKDT)", "AKST9AKDT,M3.2.0,M11.1.0"},
    {"America/Los_Angeles", "Pacific time (PST/PDT)", "PST8PDT,M3.2.0,M11.1.0"},
    {"America/Phoenix", "Arizona (MST)", "MST7"},
    {"America/Denver", "Mountain time (MST/MDT)", "MST7MDT,M3.2.0,M11.1.0"},
    {"America/Chicago", "Central time (CST/CDT)", "CST6CDT,M3.2.0,M11.1.0"},
    {"America/Mexico_City", "Mexico City (CST)", "CST6"},
    {"America/New_York", "Eastern time (EST/EDT)", "EST5EDT,M3.2.0,M11.1.0"},
    {"America/Halifax", "Atlantic time (AST/ADT)", "AST4ADT,M3.2.0,M11.1.0"},
    {"America/St_Johns", "Newfoundland (NST/NDT)", "NST3:30NDT,M3.2.0,M11.1.0"},
    {"America/Sao_Paulo", "Sao Paulo, Buenos Aires (-03)", "<-03>3"},
    {"Africa/Lagos", "Lagos (WAT)", "WAT-1"},
    {"Africa/Johannesburg", "Johannesburg (SAST)", "SAST-2"},
    {"Africa/Nairobi", "Nairobi (EAT)", "EAT-3"},
};
const int ZONE_COUNT = sizeof(ZONES) / sizeof(ZONES[0]);

int zone_find(const char *posix)
{
    for (int i = 0; i < ZONE_COUNT; i++) {
        if (strcmp(ZONES[i].posix, posix) == 0) return i;
    }
    return -1;
}

int zone_find_iana(const char *iana)
{
    for (int i = 0; i < ZONE_COUNT; i++) {
        if (strcmp(ZONES[i].iana, iana) == 0) return i;
    }
    return -1;
}
