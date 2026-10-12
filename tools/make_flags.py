#!/usr/bin/env python3
"""Build main/flags.bin, the registration-country flags shown in the
aircraft card, from Twemoji's 72x72 flag PNGs (graphics CC-BY 4.0,
https://github.com/jdecked/twemoji).

    python3 tools/make_flags.py DOWNLOAD_DIR

Fetches any PNG not already in DOWNLOAD_DIR, then writes main/flags.bin for
every country in main/countries.c. Each flag's 72x52 area is halved to
36x26 and stored as LVGL RGB565A8: the RGB565 plane, then the alpha plane.

Layout: "FLG1", u16 count, u8 width, u8 height, count two-letter codes,
then count images of width * height * 3 bytes, in the same order.
"""
import os
import re
import struct
import sys
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from png import read_png  # noqa: E402

BASE = 'https://cdn.jsdelivr.net/gh/jdecked/twemoji@15.1.0/assets/72x72/'
TOP, H_SRC = 10, 52  # where the flag sits in the 72x72 emoji
W, H = 36, 26
ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')


def codes():
    text = open(os.path.join(ROOT, 'main', 'countries.c')).read()
    return re.findall(r'\{0x[0-9A-F]+, 0x[0-9A-F]+, "([A-Z]{2})"', text)


def emoji_name(iso):
    return '-'.join('%x' % (0x1F1E6 + ord(c) - ord('A')) for c in iso) + '.png'


def shrink(rows):
    """Halve the flag's area with a 2x2 box filter, colour weighted by alpha"""
    out = []
    for y in range(H):
        line = []
        for x in range(W):
            px = [rows[TOP + 2 * y + dy][2 * x + dx] for dy in (0, 1) for dx in (0, 1)]
            a = sum(p[3] for p in px)
            if a == 0:
                line.append((0, 0, 0, 0))
                continue
            line.append(tuple(sum(p[i] * p[3] for p in px) // a for i in range(3)) + (a // 4,))
        out.append(line)
    return out


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    cache = sys.argv[1]
    os.makedirs(cache, exist_ok=True)
    isos = codes()
    images = []
    for iso in isos:
        path = os.path.join(cache, emoji_name(iso))
        if not os.path.exists(path):
            urllib.request.urlretrieve(BASE + emoji_name(iso), path)
        w, h, rows = read_png(path)
        if (w, h) != (72, 72):
            sys.exit('%s: %dx%d, expected 72x72' % (path, w, h))
        img = shrink(rows)
        color = b''.join(struct.pack('<H', (r >> 3) << 11 | (g >> 2) << 5 | b >> 3)
                         for line in img for r, g, b, _ in line)
        alpha = bytes(p[3] for line in img for p in line)
        images.append(color + alpha)
    blob = b'FLG1' + struct.pack('<HBB', len(isos), W, H) + ''.join(isos).encode() + b''.join(images)
    out = os.path.join(ROOT, 'main', 'flags.bin')
    open(out, 'wb').write(blob)
    print('%d flags, %d bytes, %s' % (len(isos), len(blob), os.path.normpath(out)))


if __name__ == '__main__':
    main()
