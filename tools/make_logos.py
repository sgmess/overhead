#!/usr/bin/env python3
"""Build main/ui/logos.bin, the airline logos shown in the aircraft card, from
pics.avs.io's 240x80 logos (by IATA code) for each airline in
main/core/airlines.c. The logos are their owners' trademarks.

    python3 tools/make_logos.py DOWNLOAD_DIR

Fetches any PNG not already in DOWNLOAD_DIR. Each logo is drawn on white,
trimmed to what is drawn, scaled to fit 138x40 and stored as RGB565: the
card shows it on a white badge.

Layout: "LOG1", u16 count, then per logo the ICAO code (3 bytes, one
padding), u16 width, u16 height and u32 offset from the start of the file,
then the images, each width * height * 2 bytes.
"""
import os
import re
import struct
import sys
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from png import read_png  # noqa: E402

BASE = 'https://pics.avs.io/240/80/'
BOX_W, BOX_H = 138, 40
ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')
# Airlines whose callsigns don't carry their IATA code, but whose logo is
SAME_LOGO = {'SHT': 'BA'}


def airlines():
    text = open(os.path.join(ROOT, 'main', 'core', 'airlines.c')).read()
    return re.findall(r'\{"([A-Z]{3})", "([A-Z0-9]{0,2})"', text)


def on_white(rows):
    return [[tuple((c * a + 255 * (255 - a)) // 255 for c in (r, g, b)) for r, g, b, a in line] for line in rows]


def trim(img):
    """The rectangle around everything that isn't white, or None"""
    ink = lambda p: min(p) < 235
    ys = [y for y, line in enumerate(img) if any(ink(p) for p in line)]
    if not ys:
        return None
    xs = [x for x in range(len(img[0])) if any(ink(img[y][x]) for y in ys)]
    return [line[xs[0]:xs[-1] + 1] for line in img[ys[0]:ys[-1] + 1]]


def scale(img, tw, th):
    """Area-average resample to tw x th"""
    h, w = len(img), len(img[0])
    sx, sy = w / tw, h / th
    out = []
    for y in range(th):
        y0, y1 = y * sy, (y + 1) * sy
        line = []
        for x in range(tw):
            x0, x1 = x * sx, (x + 1) * sx
            acc, area = [0.0, 0.0, 0.0], 0.0
            for yy in range(int(y0), min(h, int(y1 + 0.999))):
                wy = min(y1, yy + 1) - max(y0, yy)
                for xx in range(int(x0), min(w, int(x1 + 0.999))):
                    wgt = wy * (min(x1, xx + 1) - max(x0, xx))
                    if wgt <= 0:
                        continue
                    for i in range(3):
                        acc[i] += img[yy][xx][i] * wgt
                    area += wgt
            line.append(tuple(int(v / area + 0.5) for v in acc))
        out.append(line)
    return out


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    cache = sys.argv[1]
    os.makedirs(cache, exist_ok=True)
    entries = []
    for icao, iata in airlines():
        code = SAME_LOGO.get(icao, iata)
        if not code:
            continue
        path = os.path.join(cache, code + '.png')
        if not os.path.exists(path):
            urllib.request.urlretrieve(BASE + code + '.png', path)
        if os.path.getsize(path) < 100:  # no logo for this code
            continue
        w, h, rows = read_png(path)
        img = trim(on_white(rows))
        if not img:
            continue
        s = min(BOX_W / len(img[0]), BOX_H / len(img))
        tw, th = max(1, round(len(img[0]) * s)), max(1, round(len(img) * s))
        img = scale(img, tw, th)
        data = b''.join(struct.pack('<H', (r >> 3) << 11 | (g >> 2) << 5 | b >> 3) for line in img for r, g, b in line)
        entries.append((icao, tw, th, data))
    head = 6 + 12 * len(entries)
    blob, offset = [b'LOG1' + struct.pack('<H', len(entries))], head
    for icao, w, h, data in entries:
        blob.append(icao.encode() + b'\0' + struct.pack('<HHI', w, h, offset))
        offset += len(data)
    blob += [e[3] for e in entries]
    blob = b''.join(blob)
    out = os.path.join(ROOT, 'main', 'ui', 'logos.bin')
    open(out, 'wb').write(blob)
    print('%d logos, %d bytes, %s' % (len(entries), len(blob), os.path.normpath(out)))


if __name__ == '__main__':
    main()
