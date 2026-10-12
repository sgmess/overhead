"""A minimal PNG reader (8-bit greyscale, RGB and RGBA, palettes of 1-8
bits, no interlacing): enough for the Twemoji flags, with no image library
needed."""
import struct
import zlib


def read_png(path):
    """(width, height, rows of (r, g, b, a) tuples)"""
    data = open(path, 'rb').read()
    if data[:8] != b'\x89PNG\r\n\x1a\n':
        raise ValueError(path + ': not a PNG')
    pos, idat, plte, trns = 8, b'', None, None
    while pos < len(data):
        n, kind = struct.unpack('>I4s', data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + n]
        pos += 12 + n
        if kind == b'IHDR':
            w, h, depth, ctype, _, _, interlace = struct.unpack('>IIBBBBB', body)
            if interlace or (depth != 8 and not (ctype == 3 and depth in (1, 2, 4))):
                raise ValueError(path + ': only 8-bit, or a palette of 1-8 bits, non-interlaced')
        elif kind == b'PLTE':
            plte = [tuple(body[i:i + 3]) for i in range(0, len(body), 3)]
        elif kind == b'tRNS':
            trns = body
        elif kind == b'IDAT':
            idat += body
    bpp = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[ctype]  # bytes per pixel, for the filters
    raw = zlib.decompress(idat)
    stride = (w * depth + 7) // 8 if ctype == 3 else w * bpp
    rows, prev = [], bytearray(stride)
    for y in range(h):
        f = raw[y * (stride + 1)]
        line = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for i in range(stride):
            a = line[i - bpp] if i >= bpp else 0
            b = prev[i]
            c = prev[i - bpp] if i >= bpp else 0
            if f == 1:
                line[i] = (line[i] + a) & 255
            elif f == 2:
                line[i] = (line[i] + b) & 255
            elif f == 3:
                line[i] = (line[i] + (a + b) // 2) & 255
            elif f == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        prev = line
        px = []
        for x in range(w):
            if ctype == 3:
                bit = x * depth
                i = (line[bit // 8] >> (8 - depth - bit % 8)) & ((1 << depth) - 1)
                r, g, b = plte[i]
                px.append((r, g, b, trns[i] if trns and i < len(trns) else 255))
                continue
            v = line[x * bpp:(x + 1) * bpp]
            if ctype == 6:
                px.append(tuple(v))
            elif ctype == 2:
                px.append((v[0], v[1], v[2], 255))
            elif ctype == 4:
                px.append((v[0], v[0], v[0], v[1]))
            else:
                px.append((v[0], v[0], v[0], 255))
        rows.append(px)
    return w, h, rows
