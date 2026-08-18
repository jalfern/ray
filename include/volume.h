#ifndef VOLUME_H
#define VOLUME_H

#include <math.h>
#include "vector.h"

/* ── KHR_materials_volume: Beer-Lambert attenuation ─────────────
 *
 *  Reference model: three.js volumeAttenuation (source of record:
 *    web_viewer/node_modules/three/src/renderers/shaders/ShaderChunk/
 *    transmission_pars_fragment.glsl.js
 *
 *    vec3 volumeAttenuation( const in float transmissionDistance,
 *        const in vec3 attenuationColor, const in float attenuationDistance ) {
 *        if ( isinf( attenuationDistance ) ) { return vec3( 1.0 ); }
 *        else {
 *            vec3 attenuationCoefficient = -log( attenuationColor ) / attenuationDistance;
 *            vec3 transmittance = exp( - attenuationCoefficient * transmissionDistance );
 *            return transmittance;
 *        }
 *    }
 *
 *  Per the KHR_materials_volume spec, σ_t = -log(c)/d with
 *  T(x) = exp(-σ_t x).  This tracer uses the *actual ray-traced*
 *  distance through the medium (the spec's instruction to ray
 *  tracers; three.js instead passes the material's thickness
 *  parameter, a rasterizer approximation).  dist is the geometric
 *  path length traveled inside the medium so far (one segment).
 *
 *  att_d == +inf (the KHR default) short-circuits to (1,1,1);
 *  att_c == (1,1,1) makes every exponent exactly 0, so the result
 *  is exactly (1,1,1) and byte-identical renders are preserved
 *  (the control gate).
 *
 *  The MSL port in src/renderer/shaders.metal (vol_transmittance_)
 *  must stay operation-for-operation with this file.
 */

/* Beer-Lambert transmittance for a path of length dist through a
   medium with attenuationColor (linear RGB) and attenuationDistance. */
static inline V vol_transmittance(float dist, float att_r, float att_g,
                                  float att_b, float att_d) {
    if (isinf(att_d)) return (V){1.0f, 1.0f, 1.0f};
    float sr = -logf(att_r) / att_d;
    float sg = -logf(att_g) / att_d;
    float sb = -logf(att_b) / att_d;
    return (V){expf(-(sr * dist)), expf(-(sg * dist)), expf(-(sb * dist))};
}

/* 1 if any channel's attenuation coefficient is non-zero (i.e. the
   medium actually absorbs something), 0 otherwise.  Used for the
   open-ended case: a ray leaving through the environment travels an
   infinite distance, so T(inf) = 0 whenever σ != 0. */
static inline int vol_sigma_nonzero(float att_r, float att_g, float att_b,
                                    float att_d) {
    if (isinf(att_d)) return 0;
    if (att_r == 1.0f && att_g == 1.0f && att_b == 1.0f) return 0;
    return 1;
}

#endif
