import sys

# CPU/GPU PPM pixel diff: tools/ppm_diff.py cpu.ppm gpu.ppm
# Counts pixels where any 8-bit channel differs; reports sum_abs_err and
# max channel error. Tolerates the stdout prefix line ray2 writes before
# the PPM header (scans for "P6\n"). Stdlib only.

def load_ppm(path):
    data = open(path, 'rb').read()
    i = data.find(b'P6\n')
    idx = i + 3
    vals = []
    while len(vals) < 3:
        while idx < len(data) and data[idx:idx+1].isspace():
            idx += 1
        start = idx
        while idx < len(data) and not data[idx:idx+1].isspace():
            idx += 1
        vals.append(data[start:idx])
    idx += 1
    w, h, maxv = int(vals[0]), int(vals[1]), int(vals[2])
    pix = data[idx:idx + w*h*3]
    assert len(pix) == w*h*3, (len(pix), w*h*3)
    return w, h, pix

w1, h1, a = load_ppm(sys.argv[1])
w2, h2, b = load_ppm(sys.argv[2])
assert (w1, h1) == (w2, h2), ((w1, h1), (w2, h2))
n = w1*h1
diff_pixels = 0
ae_sum = 0
maxae = 0
for i in range(0, n*3, 3):
    d0 = abs(a[i]-b[i]); d1 = abs(a[i+1]-b[i+1]); d2 = abs(a[i+2]-b[i+2])
    if d0 or d1 or d2:
        diff_pixels += 1
        ae_sum += d0 + d1 + d2
        m = d0 if d0 > d1 else d1
        if d2 > m: m = d2
        if m > maxae: maxae = m
print("pixels=%d differing=%d (%.2f%%) sum_abs_err=%d max_channel_err=%d" %
      (n, diff_pixels, 100.0*diff_pixels/n, ae_sum, maxae))
