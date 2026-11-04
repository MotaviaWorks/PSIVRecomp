#!/usr/bin/env python3
"""Read pixels from psiv_headless screenshots (stored-deflate PNGs).

    python tools/movies/pixel.py shot.png 290,195 40,60     -> RGB of each pixel
"""
import struct
import sys
import zlib


def load(path):
    data = open(path, "rb").read()
    pos, idat, w = 8, b"", 0
    while pos < len(data):
        n = struct.unpack(">I", data[pos:pos + 4])[0]
        kind = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + n]
        if kind == b"IHDR":
            w, h = struct.unpack(">II", body[:8])
        elif kind == b"IDAT":
            idat += body
        pos += 12 + n
    raw = zlib.decompress(idat)
    stride = 1 + w * 3
    return lambda x, y: tuple(raw[y * stride + 1 + x * 3: y * stride + 4 + x * 3])


if __name__ == "__main__":
    get = load(sys.argv[1])
    for p in sys.argv[2:]:
        x, y = map(int, p.split(","))
        print(p, "#%02X%02X%02X" % get(x, y))
