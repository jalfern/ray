#!/usr/bin/env python3
"""Compare the volume-attenuation grids emitted by tools/vol_check.c
(CPU float32) and tools/vol_ref_check.mjs (float64 reference port of the
three.js volumeAttenuation GLSL).

Usage:  python3 tools/vol_diff.py <cpu_grid.txt> <ref_grid.txt>

Each line:  x d cr cg cb Tr Tg Tb   ("inf" allowed for d).
Compares the three transmittance channels with the gate below; keys must
match exactly.
"""
import sys

GATE = 1e-3


def main():
    if len(sys.argv) != 3:
        print(__doc__, file=sys.stderr)
        return 2
    a = [l.split() for l in open(sys.argv[1]) if l.strip()]
    b = [l.split() for l in open(sys.argv[2]) if l.strip()]
    if len(a) != len(b):
        print(f"FAIL: row count mismatch {len(a)} vs {len(b)}")
        return 1
    worst = 0.0
    worst_row = None
    for ra, rb in zip(a, b):
        ka = tuple(float(v) for v in ra[:5])
        kb = tuple(float(v) for v in rb[:5])
        if ka != kb:
            print(f"FAIL: key mismatch {ra[:5]} vs {rb[:5]}")
            return 1
        for i in range(5, 8):
            d = abs(float(ra[i]) - float(rb[i]))
            if d > worst:
                worst, worst_row = d, ra
    ok = worst < GATE
    print(f"rows={len(a)} max_abs_diff={worst:.3e} gate={GATE:g} -> {'PASS' if ok else 'FAIL'}")
    if worst_row:
        print("worst row:", " ".join(worst_row))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
