#!/usr/bin/env python3
"""Convert a PPM (P6) file to PNG, stdlib only.

Tolerates the GPU/28T prefix line ray2 writes before the P6 magic when a
scene has no "output" key. Usage: tools/ppm2png.py IN.ppm OUT.png
"""
import re, struct, sys, zlib


def parse_ppm(data):
    i = data.find(b"P6\n")
    if i < 0:
        raise ValueError("no P6 magic")
    m = re.match(rb"P6\n(\d+) (\d+)\n(\d+)\n", data[i:])
    if not m:
        raise ValueError("bad P6 header")
    w, h, mv = int(m.group(1)), int(m.group(2)), int(m.group(3))
    p = i + m.end()
    if mv != 255:
        raise ValueError("maxval != 255")
    need = w * h * 3
    if len(data) - p < need:
        raise ValueError("truncated raster")
    return w, h, data[p:p + need]


def encode_png(w, h, rgb):
    def chunk(typ, payload):
        c = typ + payload
        return struct.pack(">I", len(payload)) + c + struct.pack(">I", zlib.crc32(c) & 0xffffffff)
    rows = b"".join(b"\x00" + rgb[y * w * 3:(y + 1) * w * 3] for y in range(h))
    ihdr = struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr)
            + chunk(b"IDAT", zlib.compress(rows, 6)) + chunk(b"IEND", b""))


if __name__ == "__main__":
    with open(sys.argv[1], "rb") as f:
        w, h, rgb = parse_ppm(f.read())
    with open(sys.argv[2], "wb") as f:
        f.write(encode_png(w, h, rgb))
    print(f"{w}x{h} -> {sys.argv[2]}")
