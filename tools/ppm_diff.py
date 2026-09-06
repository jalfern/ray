import sys

# CPU/GPU PPM pixel diff.
#   tools/ppm_diff.py a.ppm b.ppm [REGION]
#       Counts pixels where any 8-bit channel differs; reports sum_abs_err,
#       max channel error, p99_9_channel_err, and n_severe. Tolerates the stdout
#       prefix line ray2 writes before the PPM header (scans for "P6\n").
#       Stdlib only.
#       p99_9_channel_err is the 99.9th percentile (nearest-rank) of the
#       PER-CHANNEL absolute error, over the channels that actually differ
#       (error > 0). It is a tail-robust companion to max_channel_err on the
#       same 0-255 per-channel scale: a few firefly channels push the max high
#       without moving the percentile.
#       n_severe is the count of channels with |error| >= 127 (near-full-range
#       divergence). It is the second certification gate: it catches a small
#       high-error region that a sparse-tail percentile is robust to.
#       REGION = "x,y,w,h" or a PPM mask file (same size; a pixel is inside
#       the region if any channel >= 128). When given, also reports the
#       differing pixels and sum_abs_err inside the region, outside it,
#       and the inside share of the total.
#   tools/ppm_diff.py make-mask out.ppm a1.ppm b1.ppm [a2.ppm b2.ppm ...]
#       Writes a 0/255 PPM mask: a pixel is set if any channel differs
#       between a1/b1 or between any subsequent pair.

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

def write_ppm(path, w, h, pix):
    with open(path, 'wb') as f:
        f.write(b'P6\n%d %d\n255\n' % (w, h))
        f.write(pix)

def p999_diff(hist, n):
    # Nearest-rank 99.9th percentile over n differing-channel errors. hist[k]
    # counts differing channels with error == k (k in 1..255). Integer math
    # (999/1000) avoids float edge cases; result is an integer channel error.
    if n == 0:
        return 0
    rank = (999 * n + 999) // 1000
    if rank < 1: rank = 1
    if rank > n: rank = n
    cum = 0
    for k, c in enumerate(hist):
        cum += c
        if cum >= rank:
            return k
    return len(hist) - 1

def diff_counts(a, b, n):
    diff_pixels = 0
    ae_sum = 0
    maxae = 0
    n_severe = 0
    ch_hist = [0] * 256  # per-channel error of differing channels, in [1,255]
    n_diffch = 0
    for i in range(0, n*3, 3):
        d0 = abs(a[i]-b[i]); d1 = abs(a[i+1]-b[i+1]); d2 = abs(a[i+2]-b[i+2])
        s = d0 + d1 + d2
        if s:
            diff_pixels += 1
            ae_sum += s
            m = d0 if d0 > d1 else d1
            if d2 > m: m = d2
            if m > maxae: maxae = m
        for e in (d0, d1, d2):
            if e:
                ch_hist[e] += 1
                n_diffch += 1
                if e >= 127:
                    n_severe += 1
    return diff_pixels, ae_sum, maxae, p999_diff(ch_hist, n_diffch), n_severe

def make_mask(out, inputs):
    mask = None
    wh = None
    for k in range(0, len(inputs), 2):
        w, h, a = load_ppm(inputs[k])
        w2, h2, b = load_ppm(inputs[k+1])
        assert (w, h) == (w2, h2), ((w, h), (w2, h2))
        n = w * h
        if mask is None:
            mask = bytearray(n * 3)
            wh = (w, h)
        for i in range(0, n * 3, 3):
            if a[i] != b[i] or a[i+1] != b[i+1] or a[i+2] != b[i+2]:
                mask[i] = mask[i+1] = mask[i+2] = 255
    write_ppm(out, wh[0], wh[1], bytes(mask))

def parse_region(region, w, h):
    inside = bytearray(w * h)
    if ',' in region:
        x, y, rw, rh = (int(t) for t in region.split(','))
        for py in range(max(0, y), min(h, y + rh)):
            row = py * w
            for px in range(max(0, x), min(w, x + rw)):
                inside[row + px] = 1
    else:
        w3, h3, m = load_ppm(region)
        assert (w3, h3) == (w, h), ((w3, h3), (w, h))
        for i in range(w * h):
            j = i * 3
            if m[j] >= 128 or m[j+1] >= 128 or m[j+2] >= 128:
                inside[i] = 1
    return inside

def main():
    args = sys.argv[1:]
    if args and args[0] == 'make-mask':
        if len(args) < 4:
            sys.exit("usage: ppm_diff.py make-mask out.ppm a.ppm b.ppm [a.ppm b.ppm ...]")
        make_mask(args[1], args[2:])
        return
    if len(args) not in (2, 3):
        sys.exit("usage: ppm_diff.py a.ppm b.ppm [REGION]\n"
                 "       REGION = x,y,w,h or a PPM mask file (inside = any channel >= 128)")
    w1, h1, a = load_ppm(args[0])
    w2, h2, b = load_ppm(args[1])
    assert (w1, h1) == (w2, h2), ((w1, h1), (w2, h2))
    n = w1 * h1
    diff_pixels, ae_sum, maxae, p999c, n_sev = diff_counts(a, b, n)
    print("pixels=%d differing=%d (%.2f%%) sum_abs_err=%d max_channel_err=%d "
          "p99_9_channel_err=%d n_severe=%d" %
          (n, diff_pixels, 100.0*diff_pixels/n, ae_sum, maxae, p999c, n_sev))
    if len(args) == 2:
        return
    inside = parse_region(args[2], w1, h1)
    npix = 0
    for i in inside:
        npix += i
    din = 0; sin = 0
    dout = 0; sout = 0
    for i in range(n):
        j = i * 3
        e = abs(a[j]-b[j]) + abs(a[j+1]-b[j+1]) + abs(a[j+2]-b[j+2])
        if e:
            if inside[i]:
                din += 1; sin += e
            else:
                dout += 1; sout += e
    print("inside:  pixels=%d differing=%d (%.2f%% of region) sum_abs_err=%d" %
          (npix, din, 100.0*din/npix if npix else 0.0, sin))
    print("outside: pixels=%d differing=%d (%.2f%% of region) sum_abs_err=%d" %
          (n - npix, dout, 100.0*dout/(n - npix) if n - npix else 0.0, sout))
    print("inside share of total differing: %.2f%%" %
          (100.0*din/diff_pixels if diff_pixels else 0.0))

main()
