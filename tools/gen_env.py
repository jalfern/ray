#!/usr/bin/env python3
"""Generate a synthetic equirectangular studio-dome HDR (RGBE).

Design: bright broad dome on the upper hemisphere (metal surfaces need a
broad bright reflection to read as solid metal, not fireflies), one big
soft key lobe overhead, dim floor. Encoded to match this repo's HDR
loader exactly (envmap.cc: value = (byte + 0.5) * 2^(e - 136), shared
per-pixel exponent, raw scanlines).

Usage: tools/gen_env.py OUT.hdr [W H]
"""
import math
import sys


def encode_rgbe(r, g, b):
    mx = max(r, g, b)
    if mx <= 1e-32:
        return b"\x00\x00\x00\x00"
    # biased exponent byte so the max channel lands in [128.5, 255.5]
    e = math.ceil(math.log2(mx) - 7.9944) + 136
    e = min(255, max(1, e))
    scale = 2.0 ** (136 - e)
    enc = lambda v: min(255, max(0, int(round(v * scale - 0.5))))
    return bytes((enc(r), enc(g), enc(b), e))


def dot(a, b):
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]


def lobe(d, c, sigma):
    return math.exp(-(1.0 - dot(d, c)) / (sigma * sigma))


def main():
    out_path = sys.argv[1]
    w = int(sys.argv[2]) if len(sys.argv) > 2 else 512
    h = int(sys.argv[3]) if len(sys.argv) > 3 else w // 2

    # big soft key overhead, slightly -x/+z (upper-back-left of the view)
    key_c = (-0.35, 0.86, 0.37)
    kn = math.sqrt(sum(c * c for c in key_c))
    key_c = tuple(c / kn for c in key_c)

    data = bytearray()
    for y in range(h):
        v = (y + 0.5) / h
        ny = math.cos(v * math.pi)
        ry = math.sqrt(max(0.0, 1.0 - ny * ny))
        for x in range(w):
            a = ((x + 0.5) / w - 0.5) * 2.0 * math.pi
            d = (math.cos(a) * ry, ny, math.sin(a) * ry)
            if ny >= 0.0:
                L = 1.2 + 5.0 * ny                # bright dome, peak ~6.2 up
                L += 22.0 * lobe(d, key_c, 0.55)  # big soft key
            else:
                L = 0.12 + 0.35 * max(0.0, 0.15 + ny)  # dim floor
            # warm-white tint
            data += encode_rgbe(L, L * 0.97, L * 0.92)

    with open(out_path, "wb") as f:
        f.write(b"#?RADIANCE\n")
        f.write(("-Y %d +X %d\n" % (h, w)).encode())
        # No FORMAT line -> the repo loader takes its raw-scanline path
        # (has_format=0 reads width*4 raw RGBE bytes per row).
        f.write(bytes(data))
    print("wrote %s %dx%d" % (out_path, w, h))


if __name__ == "__main__":
    main()
