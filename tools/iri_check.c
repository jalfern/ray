/* Grid emitter for the thin-film reference parity check.
 *
 * Prints, per grid point:
 *   <cosV> <d_nm> <film_ior> <film.r> <film.g> <film.b> <f0.r> <f0.g> <f0.b>
 *
 * tools/iri_ref_check.mjs (a float64 port of the exact three.js GLSL chunks)
 * prints the same grid; the harness compares the two. */
#include <stdio.h>
#include "thin_film.h"

int main(void) {
    static const float cosv[6] = {0.15f, 0.30f, 0.50f, 0.75f, 0.95f, 1.0f};
    static const float thicks[6] = {385.0f, 395.0f, 405.0f, 485.0f, 500.0f, 515.0f};
    static const float iors[2] = {1.8f, 2.0f};
    static const V f0s[3] = {
        {1.0f, 1.0f, 1.0f},          /* metal white (basecolor F0 at metallic=1) */
        {0.04f, 0.04f, 0.04f},       /* dielectric F0 */
        {0.5f, 0.4f, 0.3f},          /* tinted dielectric-ish */
    };

    int ci, ti, ii, fi;
    for (ci = 0; ci < 6; ci++)
        for (ti = 0; ti < 6; ti++)
            for (ii = 0; ii < 2; ii++)
                for (fi = 0; fi < 3; fi++) {
                    V f0out;
                    V film = tf_eval_iridescence(1.0f, iors[ii], cosv[ci],
                                                 thicks[ti], f0s[fi]);
                    f0out = tf_schlick_to_f0(film, 1.0f, cosv[ci]);
                    printf("%.4f %.1f %.2f %.9e %.9e %.9e %.9e %.9e %.9e\n",
                           (double)cosv[ci], (double)thicks[ti], (double)iors[ii],
                           (double)film.x, (double)film.y, (double)film.z,
                           (double)f0out.x, (double)f0out.y, (double)f0out.z);
                }
    return 0;
}
