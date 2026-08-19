import math
W, H = 768, 1024

def loadppm(p):
    d = open(p, 'rb').read()
    i = d.index(b'P6\n'); j = d.index(b'\n', i + 3); k = d.index(b'\n', j + 1)
    w, h = map(int, d[i + 3:j].split())
    return d[k + 1:k + 1 + w * h * 3], w, h

CY, CZ = 0.26, 1.15
ty, tz = 0.24, 0.0
fy, fz = ty - CY, tz - CZ
_fl = math.hypot(fy, fz)
fy, fz = fy / _fl, fz / _fl
tan_y = 0.221695
tan_x = tan_y * (W / H)

def ray_dir(x, y):
    ux = 2 * x / W - 1
    vy = 1 - 2 * y / H
    dx = ux * tan_x
    dy = fy + vy * tan_y
    dz = fz + vy * tan_y
    l = math.sqrt(dx * dx + dy * dy + dz * dz)
    return dx / l, dy / l, dz / l

SY, SZ, RO, RI, IOR = 0.1262, 0.0, 0.09804, 0.09390, 1.6
SKIP = 5e-4
_calls = 0

def circ(dy, dz, oy, oz, t_min):
    global _calls
    out = []
    for r in (RO, RI):
        b = 2 * ((oy - SY) * dy + (oz - SZ) * dz)
        c = (oy - SY) ** 2 + (oz - SZ) ** 2 - r * r
        disc = b * b - 4 * c
        if disc < 0:
            continue
        q = math.sqrt(disc)
        for t in ((-b - q) / 2, (-b + q) / 2):
            if t > t_min:
                out.append((t, r))
    return sorted(out)

def trace(x, y, regime, log=None):
    global _calls
    dx, dy, dz = ray_dir(x, y)
    oy, oz = CY, CZ
    mc = 12
    while mc:
        _calls += 1
        hits = circ(dy, dz, oy, oz, SKIP)
        if not hits:
            if log is not None and _calls == 1:
                log.append("nohits-1st")
            break
        t, r = hits[0]
        px, pz = oy + dy * t, oz + dz * t
        rprev = math.hypot(oy - SY, oz - SZ)
        if rprev > RO:
            side = 'air>wall'
        elif rprev < RI:
            side = 'hollo>wall'
        else:
            side = 'glass>out'
        ny, nz = (px - SY) / r, (pz - SZ) / r
        dv = dy * ny + dz * nz
        if dv < 0:
            ny, nz = -ny, -nz
            dv = -dv
        cos_i = min(1.0, dv)
        if side == 'glass>out':
            eta = IOR if regime == 'physical' else 1.0 / IOR
        else:
            eta = 1.0 / IOR
        k = 1.0 - eta * eta * (1.0 - cos_i * cos_i)
        if k <= 0:
            if log:
                log.append("TIR")
            break
        et = math.sqrt(k)
        ndx = eta * dx
        ndy = eta * dy + (et - eta * cos_i) * ny
        ndz = eta * dz + (et - eta * cos_i) * nz
        l = math.sqrt(ndx * ndx + ndy * ndy + ndz * ndz)
        nndx, nddy, nddz = ndx / l, ndy / l, ndz / l
        if log is not None:
            log.append("r=%.4f %s ci=%.3f eta=%.3f" % (r, side, cos_i, eta))
        oy = oy + dy * t + nddy * SKIP
        oz = oz + dz * t + nddz * SKIP
        dx, dy, dz = nndx, nddy, nddz
        mc -= 1
    return oy, oz, dy, dz

def floor_z(x, y, regime, log=None):
    dx, dy, dz = ray_dir(x, y)
    if dy >= 0:
        return None
    oy, oz, fy2, fz2 = trace(x, y, regime, log)
    if fy2 >= 0:
        return None
    return oz + (-oy / fy2) * fz2

if __name__ == '__main__':
    pre, pw, ph = loadppm('/tmp/ph4/pre_lamp_cpu.ppm')
    post, _, _ = loadppm('/tmp/ph4/post_lamp_cpu.ppm')

    def luma(b, x, y):
        i = (y * W + x) * 3
        return b[i] + b[i + 1] + b[i + 2]

    for x in (316, 384, 452):
        for reg in ("legacy", "physical"):
            log = []
            z = floor_z(x, 900, reg, log)
            print("x=%d %-8s z=%s  seq: %s" % (x, reg, None if z is None else round(z, 4), " | ".join(log)))

    ROW = 900
    DIRECT = list(range(20, 200, 4)) + list(range(600, 750, 4))
    cols = [x for x in range(200, 601, 4) if floor_z(x, ROW, 'physical') is not None]
    dz_pairs = sorted((floor_z(x, ROW, 'legacy'), luma(pre, x, ROW))
                      for x in DIRECT if floor_z(x, ROW, 'legacy') is not None)

    def Iat(z):
        if z < dz_pairs[0][0]:
            return dz_pairs[0][1]
        for i in range(len(dz_pairs) - 1):
            z0, l0 = dz_pairs[i]
            z1, l1 = dz_pairs[i + 1]
            if z0 <= z <= z1:
                t = (z - z0) / (z1 - z0) if z1 > z0 else 0
                return l0 + t * (l1 - l0)
        return dz_pairs[-1][1]

    def fit(b, regime):
        errs = []
        for x in cols:
            z = floor_z(x, ROW, regime)
            if z is None:
                continue
            errs.append(luma(b, x, ROW) - Iat(z))
        n = len(errs)
        mean = sum(errs) / n
        var = sum((e - mean) ** 2 for e in errs)
        return mean, var / n, n

    print("n_direct=%d z=[%.4f,%.4f] n_cols=%d" % (len(dz_pairs), dz_pairs[0][0], dz_pairs[-1][0], len(cols)))
    for label, b in [("PRE ", pre), ("POST", post)]:
        for regime in ["legacy", "physical"]:
            m, v, n = fit(b, regime)
            print("%s vs %-8s: offset=%+8.2f rms=%8.2f (n=%d)" % (label, regime, m, math.sqrt(v), n))
    rows = []
    for x in cols:
        za = floor_z(x, ROW, 'legacy')
        zb = floor_z(x, ROW, 'physical')
        if za is not None and zb is not None:
            rows.append((x, za, zb, za - zb))
    if rows:
        szz = [r[3] for r in rows]
        print("shift (z_legacy - z_physical): mean=%+.4f n=%d range=[%+.4f,%+.4f]"
              % (sum(szz) / len(szz), len(szz), min(szz), max(szz)))
        for x, za, zb, s in rows[::15]:
            print("  x=%3d  z_leg=%+.4f  z_phys=%+.4f  shift=%+.4f" % (x, za, zb, s))
