/* Grid emitter for the KHR_materials_volume parity check.
 *
 * Prints, per grid point:
 *   <x> <d> <c.r> <c.g> <c.b> <T.r> <T.g> <T.b>
 *
 * where x is the path length in the medium, d the attenuationDistance
 * ("inf" for the KHR default), c the attenuationColor, and T the
 * transmittance.  tools/vol_ref_check.mjs (a float64 port of the exact
 * three.js volumeAttenuation GLSL) prints the same grid; compare them
 * with tools/vol_diff.py.
 *
 * Keys (x, d, c) are printed from the intended literal values (double);
 * only the transmittance is computed in float32 via include/volume.h,
 * which is the side under test. */
#include <stdio.h>
#include <math.h>
#include "volume.h"

#define NX 7
#define ND 6
#define NC 4

static const double xd[NX] = {0.0, 0.005, 0.01, 0.02, 0.05, 0.2, 1.0};
static const double dd[ND] = {INFINITY, 10.0, 1.0, 0.1, 0.05, 0.005};
static const double cd[NC][3] = {
    {1.0, 1.0, 1.0},            /* default — must give T = 1 exactly */
    {0.9, 0.6, 0.3},
    {0.4, 0.7, 0.9},
    {0.7, 0.45, 0.25},          /* IridescenceLamp_absorption test */
};

int main(void) {
    int xi, di, ci;
    for (xi = 0; xi < NX; xi++)
        for (di = 0; di < ND; di++)
            for (ci = 0; ci < NC; ci++) {
                V t = vol_transmittance((float)xd[xi],
                                        (float)cd[ci][0], (float)cd[ci][1], (float)cd[ci][2],
                                        (float)dd[di]);
                char dbuf[32];
                if (isinf((double)dd[di]))
                    snprintf(dbuf, sizeof dbuf, "inf");
                else
                    snprintf(dbuf, sizeof dbuf, "%.10e", dd[di]);
                printf("%.10e %s %.10e %.10e %.10e %.10e %.10e %.10e\n",
                       xd[xi], dbuf,
                       cd[ci][0], cd[ci][1], cd[ci][2],
                       (double)t.x, (double)t.y, (double)t.z);
            }
    return 0;
}
