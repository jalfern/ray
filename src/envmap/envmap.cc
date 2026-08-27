#include "envmap.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static void envmap_build_ibl(EnvMap* env);

/* Bilinear over one equirect level, x wraps (periodic longitude),
   y wraps too (matches the legacy envmap_sample).  Shared by the sharp
   and the prefiltered paths so both sample identically within a level. */
static void sample_level_data(const float* data, int w, int h,
                              float u, float v, float* r, float* g, float* b) {
    float fx = u * w - 0.5f;
    float fy = v * h - 0.5f;
    int ix = (int)floorf(fx);
    int iy = (int)floorf(fy);
    float tx = fx - ix;
    float ty = fy - iy;
    ix = (ix + w) % w;
    iy = (iy + h) % h;
    int ix1 = (ix + 1) % w;
    int iy1 = (iy + 1) % h;

    const float* p00 = data + (iy * w + ix) * 3;
    const float* p10 = data + (iy * w + ix1) * 3;
    const float* p01 = data + (iy1 * w + ix) * 3;
    const float* p11 = data + (iy1 * w + ix1) * 3;

    float wx0 = 1.0f - tx, wx1 = tx;
    float wy0 = 1.0f - ty, wy1 = ty;

    *r = (p00[0] * wx0 + p10[0] * wx1) * wy0 + (p01[0] * wx0 + p11[0] * wx1) * wy1;
    *g = (p00[1] * wx0 + p10[1] * wx1) * wy0 + (p01[1] * wx0 + p11[1] * wx1) * wy1;
    *b = (p00[2] * wx0 + p10[2] * wx1) * wy0 + (p01[2] * wx0 + p11[2] * wx1) * wy1;
}

void envmap_sample_procedural(float dx, float dy, float dz, float* r, float* g, float* b) {
    (void)dz;
    float t = dy * 0.5f + 0.5f;
    float horizon = 0.5f + 0.5f * dy;
    float sky_r = 0.3f + 0.5f * horizon;
    float sky_g = 0.4f + 0.6f * horizon;
    float sky_b = 0.6f + 0.4f * horizon;
    float sun = powf(fmaxf(dy, 0.0f), 64.0f) * 4.0f;
    float cloud = powf(fmaxf(0.2f + 0.8f * sinf(dx * 12.0f + dz * 8.0f) * sinf(dz * 10.0f - dx * 6.0f), 0.0f), 2.0f) * 0.3f;
    *r = fminf(sky_r + sun + cloud, 1.0f);
    *g = fminf(sky_g + sun * 0.8f + cloud, 1.0f);
    *b = fminf(sky_b + sun * 0.4f + cloud, 1.0f);
    t = fmaxf(dy, 0.0f);
    *r = *r * (0.3f + 0.7f * t);
    *g = *g * (0.3f + 0.7f * t);
    *b = *b * (0.3f + 0.7f * t);
}

static int hdr_read_line(FILE* f, char* buf, int max_len) {
    /* Returns chars read (>= 0, 0 for a blank line), or -1 at EOF. */
    int i = 0;
    for (;;) {
        int c = fgetc(f);
        if (c == EOF) return i > 0 ? i : -1;
        if (c == '\n') break;
        if (i < max_len - 1) buf[i++] = (char)c;
    }
    buf[i] = '\0';
    return i;
}

EnvMap* envmap_load(const char* filename, float intensity) {
    if (!filename || !filename[0]) return NULL;

    FILE* f = fopen(filename, "rb");
    if (!f) return NULL;

    char buf[512];
    int has_format = 0;

    /* Blank lines are legal in HDR headers (Poly Haven puts one between
       the FORMAT line and the -Y/+Y dimension line); only EOF stops the
       scan. */
    while (hdr_read_line(f, buf, sizeof(buf)) >= 0) {
        if (buf[0] == '\0') continue;
        if (strcmp(buf, "FORMAT=32-bit_rle_rgbe") == 0) has_format = 1;
        if (buf[0] == 'Y' || buf[0] == '-') break;
    }

    int height = 0, width = 0;
    if (sscanf(buf, "-Y %d +X %d", &height, &width) != 2 &&
        sscanf(buf, "+Y %d -X %d", &height, &width) != 2) {
        fclose(f);
        return NULL;
    }

    EnvMap* env = (EnvMap*)calloc(1, sizeof(EnvMap));
    env->w = width;
    env->h = height;
    env->intensity = intensity;
    env->data = (float*)malloc(width * height * 3 * sizeof(float));

    unsigned char* scanline = (unsigned char*)malloc(width * 4);

    for (int y = 0; y < height; y++) {
        int rle = 0;
        if (has_format) {
            int c1 = fgetc(f);
            int c2 = fgetc(f);
            if (c1 == 2 && c2 == 2) {
                rle = 1;
                fgetc(f);
                fgetc(f);
            } else {
                scanline[0] = (unsigned char)c1;
                scanline[1] = (unsigned char)c2;
                if (fread(scanline + 2, 1, width * 4 - 2, f) != (size_t)(width * 4 - 2)) break;
            }
        }

        if (rle) {
            for (int ch = 0; ch < 4; ch++) {
                int pos = 0;
                while (pos < width) {
                    int c = fgetc(f);
                    if (c == EOF) break;
                    if (c > 128) {
                        int count = c - 128;
                        int val = fgetc(f);
                        for (int k = 0; k < count && pos < width; k++)
                            scanline[pos++ * 4 + ch] = (unsigned char)val;
                    } else {
                        for (int k = 0; k < c && pos < width; k++)
                            scanline[pos++ * 4 + ch] = (unsigned char)fgetc(f);
                    }
                }
            }
        } else if (!has_format) {
            for (int x = 0; x < width; x++) {
                scanline[x * 4]     = (unsigned char)fgetc(f);
                scanline[x * 4 + 1] = (unsigned char)fgetc(f);
                scanline[x * 4 + 2] = (unsigned char)fgetc(f);
                scanline[x * 4 + 3] = (unsigned char)fgetc(f);
            }
        }

        float* row = env->data + y * width * 3;
        for (int x = 0; x < width; x++) {
            unsigned char* p = scanline + x * 4;
            float e = powf(2.0f, (int)p[3] - 128 - 8);
            row[x * 3]     = (p[0] + 0.5f) * e * intensity;
            row[x * 3 + 1] = (p[1] + 0.5f) * e * intensity;
            row[x * 3 + 2] = (p[2] + 0.5f) * e * intensity;
        }
    }

    free(scanline);
    fclose(f);
    envmap_build_ibl(env);
    return env;
}

/* Band-0..1 SH coefficients from the equirect image, area-weighted
   (dOmega ~ cos(latitude) d(lat) d(lon); the grid is uniform in (lon,
   acos(dy)) so the per-pixel weight is sin(pi*v)).  Stored as the true
   orthonormal-SH coefficients:
     c00 = sqrt(pi) * mean(L)
     c1m = 2*sqrt(3*pi) * mean(L*n_m)
   so that E(N) = sqrt(pi)*c00 + sqrt(pi/3)*(c1 . N)  (see envmap_irradiance).
   Double accumulation: a studio HDR's bright pixels make float sums of
   1024*512 terms lose precision.  Both backends run this exact code (the
   GPU host also loads through envmap_load), so the coefficients — and the
   SH terms derived from them — are bit-identical CPU vs GPU. */
static void envmap_build_sh(EnvMap* env) {
    const int w = env->w, h = env->h;
    double sw = 0.0;
    double sL[3] = {0, 0, 0};
    double sLn[3][3] = {{0}};
    for (int y = 0; y < h; y++) {
        float v = (y + 0.5f) / h;
        float ny = cosf(v * (float)M_PI);
        double wt = sinf(v * (float)M_PI);
        const float* row = env->data + (size_t)y * w * 3;
        for (int x = 0; x < w; x++) {
            float a = ((x + 0.5f) / w - 0.5f) * 2.0f * (float)M_PI;
            float nx = cosf(a), nz = sinf(a);
            for (int c = 0; c < 3; c++) {
                float L = row[x * 3 + c];
                sw += wt;
                sL[c] += wt * L;
                sLn[c][0] += wt * L * nx;
                sLn[c][1] += wt * L * ny;
                sLn[c][2] += wt * L * nz;
            }
        }
    }
    if (sw <= 0.0) return;
    double inv = 1.0 / sw;
    const double SQRT_PI = 1.7724538509055160274;
    const double TWO_SQRT_3PI = 6.1399602476789308561;
    for (int c = 0; c < 3; c++) {
        env->sh[c * 4 + 0] = (float)(SQRT_PI * sL[c] * inv);
        env->sh[c * 4 + 1] = (float)(TWO_SQRT_3PI * sLn[c][0] * inv);
        env->sh[c * 4 + 2] = (float)(TWO_SQRT_3PI * sLn[c][1] * inv);
        env->sh[c * 4 + 3] = (float)(TWO_SQRT_3PI * sLn[c][2] * inv);
    }
}

/* Box-downsampled mip chain, level 0 == data.  Longitude wraps (equirect
   is periodic in x); latitude clamps (no pixel above/below the poles).
   A plain box chain, not a GGX importance prefilter — adequate for the
   smooth studio HDRs this repo uses; the roughness->lod mapping is what
   three.js-style IBL needs, the filter shape is a refinement for later. */
static void envmap_build_mips(EnvMap* env) {
    const int w0 = env->w, h0 = env->h;
    int levels = 1;
    for (int w = w0, h = h0; w > 1 || h > 1; ) {
        levels++;
        if (w > 1) w >>= 1;
        if (h > 1) h >>= 1;
    }
    env->num_mips = levels;
    env->mips = (float**)calloc(levels, sizeof(float*));
    env->mip_w = (int*)calloc(levels, sizeof(int));
    env->mip_h = (int*)calloc(levels, sizeof(int));
    env->mips[0] = env->data;
    env->mip_w[0] = w0;
    env->mip_h[0] = h0;
    for (int l = 1; l < levels; l++) {
        int pw = env->mip_w[l - 1], ph = env->mip_h[l - 1];
        int w = pw > 1 ? pw >> 1 : 1;
        int h = ph > 1 ? ph >> 1 : 1;
        env->mip_w[l] = w;
        env->mip_h[l] = h;
        const float* prev = env->mips[l - 1];
        float* cur = (float*)malloc((size_t)w * h * 3 * sizeof(float));
        for (int y = 0; y < h; y++) {
            int py = y * 2; if (py >= ph) py = ph - 1;
            int py1 = py + 1; if (py1 >= ph) py1 = ph - 1;
            for (int x = 0; x < w; x++) {
                int px = (x * 2) % pw;
                int px1 = (px + 1) % pw;
                for (int c = 0; c < 3; c++) {
                    cur[(y * w + x) * 3 + c] = 0.25f *
                        (prev[(py * pw + px) * 3 + c] + prev[(py * pw + px1) * 3 + c] +
                         prev[(py1 * pw + px) * 3 + c] + prev[(py1 * pw + px1) * 3 + c]);
                }
            }
        }
        env->mips[l] = cur;
    }
}

static void envmap_build_ibl(EnvMap* env) {
    if (!env || !env->data) return;
    envmap_build_sh(env);
    envmap_build_mips(env);
}

void envmap_free(EnvMap* env) {
    if (env) {
        if (env->mips) {
            for (int l = 0; l < env->num_mips; l++)
                if (env->mips[l] && env->mips[l] != env->data)
                    free(env->mips[l]);
            free(env->mips);
            free(env->mip_w);
            free(env->mip_h);
        }
        free(env->data);
        free(env);
    }
}

void envmap_sample(const EnvMap* env, float dx, float dy, float dz,
                   float* r, float* g, float* b) {
    if (!env || !env->data) {
        envmap_sample_procedural(dx, dy, dz, r, g, b);
        return;
    }

    float u = atan2f(dz, dx) * (0.5f / (float)M_PI) + 0.5f;
    float v = acosf(fmaxf(fminf(dy, 1.0f), -1.0f)) * (1.0f / (float)M_PI);
    sample_level_data(env->data, env->w, env->h, u, v, r, g, b);
}

void envmap_sample_prefiltered(const EnvMap* env, float dx, float dy, float dz,
                               float roughness, float* r, float* g, float* b) {
    if (!env || !env->data || env->num_mips < 2) {
        envmap_sample(env, dx, dy, dz, r, g, b);
        return;
    }
    float u = atan2f(dz, dx) * (0.5f / (float)M_PI) + 0.5f;
    float v = acosf(fmaxf(fminf(dy, 1.0f), -1.0f)) * (1.0f / (float)M_PI);

    float lod;
    if (roughness < 0.0f) lod = 0.0f;
    else if (roughness > 1.0f) lod = (float)(env->num_mips - 1);
    else lod = roughness * (float)(env->num_mips - 1);
    int l0 = (int)floorf(lod);
    if (l0 > env->num_mips - 1) l0 = env->num_mips - 1;
    float f = lod - (float)l0;
    int l1 = l0 + 1;
    if (l1 > env->num_mips - 1) l1 = env->num_mips - 1;

    float r0, g0, b0, r1, g1, b1;
    sample_level_data(env->mips[l0], env->mip_w[l0], env->mip_h[l0], u, v, &r0, &g0, &b0);
    sample_level_data(env->mips[l1], env->mip_w[l1], env->mip_h[l1], u, v, &r1, &g1, &b1);
    *r = r0 + (r1 - r0) * f;
    *g = g0 + (g1 - g0) * f;
    *b = b0 + (b1 - b0) * f;
}

void envmap_irradiance(const EnvMap* env, float nx, float ny, float nz,
                       float* r, float* g, float* b) {
    /* E(N) = sqrt(pi)*c00 + sqrt(pi/3)*(c1 . N) — the closed-form
       hemispherical integral of the l<=1 SH approximation (band 0:
       constant -> pi*mean over the hemisphere's cos-weighted integral;
       band 1: linear -> (2*pi/3) coefficient fold-out). */
    const float SQRT_PI = 1.7724538509055160f;
    const float SQRT_PI_3 = 1.0233267079464890f;
    if (!env || !env->data) {
        *r = 0.0f; *g = 0.0f; *b = 0.0f;
        return;
    }
    const float* sh = env->sh;
    *r = SQRT_PI * sh[0]  + SQRT_PI_3 * (sh[1] * nx + sh[2] * ny + sh[3] * nz);
    *g = SQRT_PI * sh[4]  + SQRT_PI_3 * (sh[5] * nx + sh[6] * ny + sh[7] * nz);
    *b = SQRT_PI * sh[8]  + SQRT_PI_3 * (sh[9] * nx + sh[10] * ny + sh[11] * nz);
}
