#ifndef ENVMAP_H
#define ENVMAP_H

/* Phase 2 IBL: the loaded env also carries
   - a prefiltered mip chain (box-downsampled levels, level 0 == data),
     used to sample the env blurred by a surface's roughness along a
     specular/refracted ray; and
   - band-0..1 spherical-harmonic diffuse irradiance coefficients
     (sh[c][4] = {c00, c1x, c1y, c1z} for c in {r,g,b}, orthonormal
     SH basis, equirect area-weighted at load).
   Both are exactly zero-cost consumers of env->data: scenes without a
   loaded env keep the legacy 0.15 ambient and sharp-env misses. */
typedef struct {
    float* data;  // w * h * 3, row-major RGB (level 0)
    int w, h;
    float intensity;
    float** mips;   // [num_mips] contiguous w_l x h_l x 3 float arrays
    int* mip_w;
    int* mip_h;
    int num_mips;
    float sh[12];   // [3][4]: {c00, c1x, c1y, c1z} per channel
} EnvMap;

EnvMap* envmap_load(const char* filename, float intensity);
void envmap_free(EnvMap* env);
void envmap_sample(const EnvMap* env, float dx, float dy, float dz, float* r, float* g, float* b);
void envmap_sample_procedural(float dx, float dy, float dz, float* r, float* g, float* b);

/* Prefiltered env sample: roughness in [0,1] maps to the mip chain
   (0 = level 0 sharp, 1 = last level); linear-mip-linear between the
   bracketing levels.  Falls back to the sharp sample without a chain. */
void envmap_sample_prefiltered(const EnvMap* env, float dx, float dy, float dz,
                               float roughness, float* r, float* g, float* b);

/* Band-0..1 diffuse irradiance E(N) = sqrt(pi)*c00 + sqrt(pi/3)*(c1.N),
   the closed-form hemispherical integral of the l<=1 SH approximation. */
void envmap_irradiance(const EnvMap* env, float nx, float ny, float nz,
                       float* r, float* g, float* b);

#endif
