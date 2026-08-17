#ifndef THIN_FILM_H
#define THIN_FILM_H

#include <math.h>
#include "vector.h"

/* ── KHR_materials_iridescence: thin-film interference ─────────
 *
 *  Verbatim port of the reference model used by three.js (Belcour,
 *  "Approximating Thin-film Interference for Rendered Imagery"):
 *  non-coherent DC term plus two interference orders (m = 1, 2)
 *  evaluated against CIE 1931 XYZ spectral sensitivity curves in
 *  Fourier space. Source of record for the constants:
 *    web_viewer/node_modules/three/src/renderers/shaders/ShaderChunk/
 *    iridescence_fragment.glsl.js (evalSensitivity / evalIridescence)
 *    common.glsl.js (F_Schlick, Epic SIGGRAPH '13 variant)
 *    lights_physical_pars_fragment.glsl.js (Schlick_to_F0)
 *
 *  d_nm is film thickness in nanometers.  cos_v is the clamped |N·V|
 *  (view direction, per the reference).  base_f0 is the base material
 *  F0 (the substrate IOR is derived from it, outside medium = air).
 *  Returns per-channel thin-film reflectance; *f0out receives
 *  Schlick_to_F0(result, 1.0, cos_v).
 *
 *  The MSL port in src/renderer/shaders.metal (tf_eval_iridescence_
 *  metal) must stay line-for-line with this file.
 */

#define TF_PI 3.14159265358979323846f

/* F_Schlick — optimized variant (Epic, SIGGRAPH '13); cos_v in [0,1]. */
static inline float tf_f_schlick(float f0, float f90, float cos_v) {
    float fresnel = exp2f((-5.55473f * cos_v - 6.98316f) * cos_v);
    return f0 * (1.0f - fresnel) + f90 * fresnel;
}

/* IorToFresnel0(film, air) for a single interface. */
static inline float tf_r_f0(float n_a, float n_b) {
    float t = (n_a - n_b) / (n_a + n_b);
    return t * t;
}

/* evalSensitivity — XYZ sensitivity curves in Fourier space. */
static inline V tf_eval_sensitivity(float opd, float phi_r, float phi_g, float phi_b) {
    float phase = 2.0f * TF_PI * opd * 1.0e-9f;
    float p2 = phase * phase;
    float xr = 5.4856e-13f * sqrtf(2.0f * TF_PI * 4.3278e+09f) *
               cosf(1.6810e+06f * phase + phi_r) * expf(-p2 * 4.3278e+09f);
    float xg = 4.4201e-13f * sqrtf(2.0f * TF_PI * 9.3046e+09f) *
               cosf(1.7953e+06f * phase + phi_g) * expf(-p2 * 9.3046e+09f);
    float xb = 5.2481e-13f * sqrtf(2.0f * TF_PI * 6.6121e+09f) *
               cosf(2.2084e+06f * phase + phi_b) * expf(-p2 * 6.6121e+09f);
    xr += 9.7470e-14f * sqrtf(2.0f * TF_PI * 4.5282e+09f) *
          cosf(2.2399e+06f * phase + phi_r) * expf(-p2 * 4.5282e+09f);
    xr /= 1.0685e-7f; xg /= 1.0685e-7f; xb /= 1.0685e-7f;
    /* XYZ -> linear sRGB (Rec.709) */
    float r = 3.2404542f * xr - 1.5371385f * xg - 0.4985314f * xb;
    float g = -0.9692660f * xr + 1.8760108f * xg + 0.0415560f * xb;
    float b = 0.0556434f * xr - 0.2040259f * xg + 1.0572252f * xb;
    return (V){r, g, b};
}

/* evalIridescence — per-channel thin-film reflectance. */
static inline V tf_eval_iridescence(float outside_ior, float eta2,
                                    float cos_v, float d_nm,
                                    V base_f0) {
    /* Force iridescenceIOR -> outsideIOR when thinFilmThickness -> 0.0 */
    float s = d_nm / 0.03f;
    s = (s < 0.0f) ? 0.0f : ((s > 1.0f) ? 1.0f : s);
    s = s * s * (3.0f - 2.0f * s);
    float n_f = outside_ior + (eta2 - outside_ior) * s;

    /* cosTheta2 via Snell's law, with TIR handling */
    float si = outside_ior / n_f;
    float cos2q = 1.0f - si * si * (1.0f - cos_v * cos_v);
    if (cos2q < 0.0f) {
        return (V){1.0f, 1.0f, 1.0f};
    }
    float cos2 = sqrtf(cos2q);

    /* First interface (air / film) */
    float R12 = tf_f_schlick(tf_r_f0(n_f, outside_ior), 1.0f, cos_v);
    float T121 = 1.0f - R12;
    float phi12 = 0.0f;
    if (n_f < outside_ior) phi12 = TF_PI;
    float phi21 = TF_PI - phi12;

    /* Second interface (film / base material) */
    V bf = base_f0;
    bf.x = (bf.x < 0.0f) ? 0.0f : ((bf.x > 0.9999f) ? 0.9999f : bf.x);
    bf.y = (bf.y < 0.0f) ? 0.0f : ((bf.y > 0.9999f) ? 0.9999f : bf.y);
    bf.z = (bf.z < 0.0f) ? 0.0f : ((bf.z > 0.9999f) ? 0.9999f : bf.z);
    float sr = sqrtf(bf.x), sg = sqrtf(bf.y), sb = sqrtf(bf.z);
    float bior_r = (1.0f + sr) / (1.0f - sr);
    float bior_g = (1.0f + sg) / (1.0f - sg);
    float bior_b = (1.0f + sb) / (1.0f - sb);
    float R1_r = tf_r_f0(bior_r, n_f), R1_g = tf_r_f0(bior_g, n_f), R1_b = tf_r_f0(bior_b, n_f);
    float R23_r = tf_f_schlick(R1_r, 1.0f, cos2);
    float R23_g = tf_f_schlick(R1_g, 1.0f, cos2);
    float R23_b = tf_f_schlick(R1_b, 1.0f, cos2);
    float phi23_r = (bior_r < n_f) ? TF_PI : 0.0f;
    float phi23_g = (bior_g < n_f) ? TF_PI : 0.0f;
    float phi23_b = (bior_b < n_f) ? TF_PI : 0.0f;

    /* Phase shift and compound terms */
    float opd = 2.0f * n_f * d_nm * cos2;
    float I_r = 0.0f, I_g = 0.0f, I_b = 0.0f;
    float r123_r, r123_g, r123_b;
    float Cm_r, Cm_g, Cm_b;
    float Rsr = R12 * R23_r; Rsr = (Rsr < 1e-5f) ? 1e-5f : ((Rsr > 0.9999f) ? 0.9999f : Rsr);
    float Rsg = R12 * R23_g; Rsg = (Rsg < 1e-5f) ? 1e-5f : ((Rsg > 0.9999f) ? 0.9999f : Rsg);
    float Rsb = R12 * R23_b; Rsb = (Rsb < 1e-5f) ? 1e-5f : ((Rsb > 0.9999f) ? 0.9999f : Rsb);
    r123_r = sqrtf(Rsr); r123_g = sqrtf(Rsg); r123_b = sqrtf(Rsb);
    float Rs_r = T121 * T121 * R23_r / (1.0f - Rsr);
    float Rs_g = T121 * T121 * R23_g / (1.0f - Rsg);
    float Rs_b = T121 * T121 * R23_b / (1.0f - Rsb);

    /* m = 0 (DC term amplitude) */
    I_r = R12 + Rs_r;
    I_g = R12 + Rs_g;
    I_b = R12 + Rs_b;

    /* m = 1, 2 (pairs of diracs) */
    Cm_r = Rs_r - T121; Cm_g = Rs_g - T121; Cm_b = Rs_b - T121;
    int m;
    for (m = 1; m <= 2; m++) {
        Cm_r *= r123_r; Cm_g *= r123_g; Cm_b *= r123_b;
        V sm = tf_eval_sensitivity((float)m * opd,
                                   (float)m * (phi21 + phi23_r),
                                   (float)m * (phi21 + phi23_g),
                                   (float)m * (phi21 + phi23_b));
        I_r += Cm_r * 2.0f * sm.x;
        I_g += Cm_g * 2.0f * sm.y;
        I_b += Cm_b * 2.0f * sm.z;
    }

    /* Out-of-gamut colors can be produced; clamp negative values to 0. */
    if (I_r < 0.0f) I_r = 0.0f;
    if (I_g < 0.0f) I_g = 0.0f;
    if (I_b < 0.0f) I_b = 0.0f;
    return (V){I_r, I_g, I_b};
}

/* Schlick_to_F0 — analytic inverse of F = F0 + (F90-F0)(1-x)^5. */
static inline V tf_schlick_to_f0(V f, float f90, float cos_v) {
    float x = 1.0f - cos_v;
    x = (x < 0.0f) ? 0.0f : ((x > 1.0f) ? 1.0f : x);
    float x5 = x * x;
    x5 = x5 * x5 * x;
    x5 = (x5 < 0.9999f) ? x5 : 0.9999f;
    return (V){(f.x - f90 * x5) / (1.0f - x5),
               (f.y - f90 * x5) / (1.0f - x5),
               (f.z - f90 * x5) / (1.0f - x5)};
}

#endif
