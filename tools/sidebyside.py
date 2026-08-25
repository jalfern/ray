import struct, re, zlib, sys

def read_png(path):
    d = open(path, 'rb').read()
    assert d[:8] == b'\x89PNG\r\n\x1a\n'
    pos = 8
    w = h = None
    bitdepth = colortype = None
    idat = b''
    while pos < len(d):
        (ln,) = struct.unpack('>I', d[pos:pos+4])
        typ = d[pos+4:pos+8]
        data = d[pos+8:pos+8+ln]
        if typ == b'IHDR':
            w, h, bitdepth, colortype = struct.unpack('>IIBB', data[:10])
        elif typ == b'IDAT':
            idat += data
        pos += 12 + ln
    assert bitdepth == 8 and colortype in (2, 6), (bitdepth, colortype)
    bpp = 3 if colortype == 2 else 4
    raw = zlib.decompress(idat)
    stride = w * bpp
    out = bytearray(h * stride)
    prev = bytearray(stride)
    p = 0
    for y in range(h):
        f = raw[p]; p += 1
        line = bytearray(raw[p:p+stride]); p += stride
        if f == 1:
            for i in range(bpp, stride):
                line[i] = (line[i] + line[i-bpp]) & 0xff
        elif f == 2:
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 0xff
        elif f == 3:
            for i in range(stride):
                a = line[i-bpp] if i >= bpp else 0
                line[i] = (line[i] + ((a + prev[i]) >> 1)) & 0xff
        elif f == 4:
            for i in range(stride):
                a = line[i-bpp] if i >= bpp else 0
                b = prev[i]
                c = prev[i-bpp] if i >= bpp else 0
                pa = abs(b - c); pb = abs(c - a); pc = abs(a + b - 2*c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[i] = (line[i] + pr) & 0xff
        out[y*stride:(y+1)*stride] = line
        prev = line
    rgb = bytes(out[0:h*stride*3//4*3]) if colortype == 6 else bytes(out)
    if colortype == 6:
        rgb = bytes(b for i, b in enumerate(out) if i % 4 != 3)
    return w, h, rgb

def write_png(path, w, h, rgb):
    def chunk(t, data):
        c = t + data
        return struct.pack('>I', len(data)) + c + struct.pack('>I', zlib.crc32(c))
    rows = b''.join(b'\x00' + rgb[y*w*3:(y+1)*w*3] for y in range(h))
    ihdr = struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)
    open(path, 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', ihdr) + chunk(b'IDAT', zlib.compress(rows)) + chunk(b'IEND', b''))

def scale_nearest(w, h, rgb, tw, th):
    out = bytearray(tw * th * 3)
    for y in range(th):
        sy = min(h - 1, y * h // th)
        for x in range(tw):
            sx = min(w - 1, x * w // tw)
            s = (sy * w + sx) * 3
            d = (y * tw + x) * 3
            out[d:d+3] = rgb[s:s+3]
    return bytes(out)

def read_ppm(path):
    d = open(path, 'rb').read()
    i = d.find(b'P6\n')
    m = re.match(rb'P6\n(\d+) (\d+)\n(\d+)\n', d[i:])
    w, h = int(m.group(1)), int(m.group(2))
    return w, h, d[i+m.end():i+m.end()+w*h*3]

ref_path, new_path, out_path = sys.argv[1], sys.argv[2], sys.argv[3]
rw, rh, rrgb = read_png(ref_path)
nw, nh, nrgb = read_ppm(new_path)
th = max(rh, nh)
tw_r = rw * th // rh
tw_n = nw * th // nh
r2 = scale_nearest(rw, rh, rrgb, tw_r, th)
n2 = scale_nearest(nw, nh, nrgb, tw_n, th)
gap = 8
W = tw_r + tw_n + gap
H = th
canvas = bytearray(W * H * 3)
for y in range(H):
    for x in range(W):
        if x < tw_r:
            src = r2[(y*tw_r + x)*3:(y*tw_r + x)*3+3]
        elif x < tw_r + gap:
            src = b'\x20\x20\x20'
        else:
            xx = x - tw_r - gap
            src = n2[(y*tw_n + xx)*3:(y*tw_n + xx)*3+3]
        canvas[(y*W + x)*3:(y*W + x)*3+3] = src
write_png(out_path, W, H, bytes(canvas))
print('wrote', out_path, W, 'x', H)
