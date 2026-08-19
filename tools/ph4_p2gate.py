import importlib.util
import math

spec = importlib.util.spec_from_file_location("p2", "tools/ph4_p2.py")
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)


def loadppm(p):
    d = open(p, 'rb').read()
    i = d.index(b'P6\n')
    j = d.index(b'\n', i + 3)
    k = d.index(b'\n', j + 1)
    w, h = map(int, d[i + 3:j].split())
    return d[k + 1:k + 1 + w * h * 3], w, h


W = 768
pre, _, _ = loadppm('/tmp/ph4/pre_lamp_cpu.ppm')
post, _, _ = loadppm('/tmp/ph4/post_lamp_cpu.ppm')
nog, _, _ = loadppm('/tmp/ph4/g1pre_no_glass_cpu.ppm')
X = 384


def L(b, x, y):
    i = (y * W + x) * 3
    return b[i] + b[i + 1] + b[i + 2]


def z_ns(y):
    dx, dy, dz = m.ray_dir(X, y)
    if dy >= 0:
        return None
    z = m.CZ + (-m.CY / dy) * dz
    return z if z > -2.3 else None


def z_model(y, regime):
    dx, dy, dz = m.ray_dir(X, y)
    if dy >= 0:
        return None
    oy, oz, fy2, fz2 = m.trace(X, y, regime)
    if fy2 >= 0:
        return None
    z = oz + (-oy / fy2) * fz2
    return z if z > -2.3 else None


def dy_pred(y, regime, ystep=16):
    za = z_model(y, regime)
    zb = z_ns(y)
    if za is None or zb is None:
        return 'none', 0.0
    za1 = z_model(y - ystep, regime)
    za2 = z_model(y + ystep, regime)
    if za1 is None or za2 is None:
        return 'none', 0.0
    dzdy = (za2 - za1) / (2.0 * ystep)
    if abs(dzdy) < 5e-4:
        return 'CAUSTIC', za - zb
    zn1 = z_ns(y - ystep)
    zn2 = z_ns(y + ystep)
    if zn1 is None or zn2 is None:
        return 'none', 0.0
    dzn = (zn2 - zn1) / (2.0 * ystep)
    return (za - zb) / dzn, za - zb


def col_shift(b, y0, win=9, sh=160, step=2):
    a = [L(b, X, y0 + kk * step) for kk in range(win)]
    am = sum(a) / win
    aa = [v - am for v in a]
    na = math.sqrt(sum(v * v for v in aa)) or 1.0
    best, bestc = 0, -2.0
    for d in range(-sh, sh + 1):
        ys = [y0 + d + kk * step for kk in range(win)]
        if any(yy < 0 or yy >= 1024 for yy in ys):
            continue
        g = [L(nog, X, yy) for yy in ys]
        gm = sum(g) / win
        gg = [v - gm for v in g]
        ng = math.sqrt(sum(v * v for v in gg)) or 1.0
        c = sum(p * q for p, q in zip(aa, gg)) / (na * ng)
        if c > bestc:
            bestc, best = c, d
    return best, bestc


def fdl(v, dz):
    if v == 'CAUSTIC':
        return 'CAUSTIC(dz=%+.2f)' % dz
    if v == 'none':
        return '   none   '
    return '%+5.0f (dz=%+.2f)' % (v, dz)


print('row   zns      zleg      dYleg          zphys      dYphys         | mPRE (c)   mPOST (c)')
for y in range(600, 961, 20):
    dl, dsl = dy_pred(y, 'legacy')
    dp, dsp = dy_pred(y, 'physical')
    mpre, cpre = col_shift(pre, y)
    mpost, cpost = col_shift(post, y)
    zbl = z_model(y, 'legacy')
    zbp = z_model(y, 'physical')
    print('%4d  %8.3f  %s | %-18s | %8.3f | %-18s | %+4d (%.2f)  %+4d (%.2f)' % (
        y,
        z_ns(y) if z_ns(y) is not None else -9.0,
        '%7.3f' % zbl if zbl is not None else '    -  ',
        fdl(dl, dsl),
        zbp if zbp is not None else -9.0,
        fdl(dp, dsp),
        mpre, cpre, mpost, cpost))
