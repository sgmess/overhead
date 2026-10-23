#!/usr/bin/env python3
"""Build main/core/types.c, the names of aircraft types by their ICAO
designator ("B788" is "Boeing 787-8 Dreamliner"), from tar1090-db's list,
which takes them from ICAO Doc 8643.

    python3 tools/make_types.py

The names come in capitals for the maker ("BOEING 787-8 Dreamliner"); words
of four or more letters are put in title case, "DE" becomes "de", and
Airbus's "A-320" becomes "A320". The table is sorted for a binary search.
"""
import gzip
import json
import os
import re
import urllib.request

COMMIT = 'db2414b6b5767379f6d03ada92bb97261c91391c'  # 19 Oct 2026
URL = f'https://raw.githubusercontent.com/wiedehopf/tar1090-db/{COMMIT}/db/icao_aircraft_types2.js'
ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')
OUT = os.path.join(ROOT, 'main', 'core', 'types.c')
# Makers known by their capitals, though four letters or more
KEEP = {'SOCATA', 'NAMC', 'AVIC', 'SIAI', 'CASA', 'COMAC', 'HAMC', 'XAIC', 'SAIC', 'IRKUT', 'ICON', 'FLSA', 'CAIC',
        'HAIG', 'EADS', 'AASI', 'IPTN', 'PZL', 'HESA', 'TAAL', 'SZD', 'SAAC', 'VTOL'}
# Short words that are words, not initials
WORDS = {'AIR': 'Air', 'SUD': 'Sud', 'SKY': 'Sky', 'SUN': 'Sun', 'JET': 'Jet', 'BEE': 'Bee'}
MAX_LEN = 39  # the C array's width, less the terminator


def tidy(name):
    words = []
    for w in name.split(' '):
        if w == 'DE':
            w = 'de'
        elif w in WORDS:
            w = WORDS[w]
        elif w.isalpha() and w.isupper() and len(w) >= 4 and w not in KEEP:
            w = w.capitalize()
        words.append(w)
    name = ' '.join(words)
    name = re.sub(r'^Airbus A-(\d)', r'Airbus A\1', name)
    return name[:MAX_LEN].rstrip()


def main():
    raw = urllib.request.urlopen(URL, timeout=60).read()
    try:
        raw = gzip.decompress(raw)  # served gzipped
    except OSError:
        pass
    types = json.loads(raw)
    rows = sorted((code, tidy(v[0])) for code, v in types.items()
                  if re.fullmatch(r'[A-Z0-9]{2,4}', code) and v and v[0])
    with open(OUT, 'w') as f:
        f.write('// Made by tools/make_types.py from tar1090-db (ICAO Doc 8643) at\n')
        f.write(f'// {COMMIT}: don\'t edit, run it again.\n\n')
        f.write('#include "types.h"\n\n#include <stdlib.h>\n#include <string.h>\n\n')
        f.write('typedef struct {\n    char code[5];\n    char name[40];\n} type_name_t;\n\n')
        f.write(f'static const type_name_t TYPES[{len(rows)}] = {{\n')
        for code, name in rows:
            f.write(f'    {{"{code}", "{name.replace(chr(92), "").replace(chr(34), chr(39))}"}},\n')
        f.write('};\n\n')
        f.write('static int cmp(const void *key, const void *elem)\n{\n'
                '    return strcmp(key, ((const type_name_t *)elem)->code);\n}\n\n')
        f.write('const char *type_name(const char *code)\n{\n'
                '    const type_name_t *t = bsearch(code, TYPES, sizeof(TYPES) / sizeof(TYPES[0]), sizeof(TYPES[0]), cmp);\n'
                '    return t ? t->name : NULL;\n}\n')
    print(f'{len(rows)} types into {os.path.relpath(OUT, ROOT)}')


if __name__ == '__main__':
    main()
