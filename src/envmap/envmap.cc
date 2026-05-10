#include "envmap.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

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
    int i = 0;
    while (i < max_len - 1) {
        int c = fgetc(f);
        if (c == EOF || c == '\n') break;
        buf[i++] = (char)c;
    }
    buf[i] = '\0';
    return i > 0;
}

EnvMap* envmap_load(const char* filename, float intensity) {
    if (!filename || !filename[0]) return NULL;

    FILE* f = fopen(filename, "rb");
    if (!f) return NULL;

    char buf[512];
    int has_format = 0;

    while (hdr_read_line(f, buf, sizeof(buf))) {
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
    return env;
}

void envmap_free(EnvMap* env) {
    if (env) {
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

    float fx = u * env->w - 0.5f;
    float fy = v * env->h - 0.5f;
    int ix = (int)floorf(fx);
    int iy = (int)floorf(fy);
    float tx = fx - ix;
    float ty = fy - iy;
    ix = (ix + env->w) % env->w;
    iy = (iy + env->h) % env->h;
    int ix1 = (ix + 1) % env->w;
    int iy1 = (iy + 1) % env->h;

    float* p00 = env->data + (iy * env->w + ix) * 3;
    float* p10 = env->data + (iy * env->w + ix1) * 3;
    float* p01 = env->data + (iy1 * env->w + ix) * 3;
    float* p11 = env->data + (iy1 * env->w + ix1) * 3;

    float wx0 = 1.0f - tx, wx1 = tx;
    float wy0 = 1.0f - ty, wy1 = ty;

    *r = (p00[0] * wx0 + p10[0] * wx1) * wy0 + (p01[0] * wx0 + p11[0] * wx1) * wy1;
    *g = (p00[1] * wx0 + p10[1] * wx1) * wy0 + (p01[1] * wx0 + p11[1] * wx1) * wy1;
    *b = (p00[2] * wx0 + p10[2] * wx1) * wy0 + (p01[2] * wx0 + p11[2] * wx1) * wy1;
}
