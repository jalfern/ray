#include "shading.h"
#include <math.h>
#include <string.h>

#define EPS 1e-4f

MaterialProps get_material_props(const char* material_name) {
    MaterialProps mat;
    if (strcmp(material_name, "plastic") == 0) {
        mat.specular_strength = 0.4f;
        mat.specular_exponent = 32.0f;
    } else {
        mat.specular_strength = 0.8f;
        mat.specular_exponent = 64.0f;
    }
    return mat;
}

V floor_color(V p) {
    int ix = (int)floorf(p.x);
    int iz = (int)floorf(p.z);
    return ((ix + iz) & 1) ? (V){0.08f, 0.12f, 0.25f} : (V){0.25f, 0.4f, 0.7f};
}

static float hash3(float x, float y, float z) {
    float n = sinf(x * 127.1f + y * 311.7f + z * 74.7f) * 43758.5453f;
    return n - floorf(n);
}

static float smoothstep(float t) {
    return t * t * (3.0f - 2.0f * t);
}

static float vnoise(float x, float y, float z) {
    int ix = (int)floorf(x), iy = (int)floorf(y), iz = (int)floorf(z);
    float fx = x - ix, fy = y - iy, fz = z - iz;
    fx = smoothstep(fx); fy = smoothstep(fy); fz = smoothstep(fz);
    float c000 = hash3(ix, iy, iz), c100 = hash3(ix+1, iy, iz);
    float c010 = hash3(ix, iy+1, iz), c110 = hash3(ix+1, iy+1, iz);
    float c001 = hash3(ix, iy, iz+1), c101 = hash3(ix+1, iy, iz+1);
    float c011 = hash3(ix, iy+1, iz+1), c111 = hash3(ix+1, iy+1, iz+1);
    float a = c000 + (c100 - c000) * fx;
    float b = c010 + (c110 - c010) * fx;
    float a2 = c001 + (c101 - c001) * fx;
    float b2 = c011 + (c111 - c011) * fx;
    return a + (b - a) * fy + (a2 - a + (b2 - a2 - b + a) * fy) * fz;
}

V eval_texture_uv(V uv, V primary_color, const TextureData* tex) {
    if (!tex || tex->type == 0) return primary_color;
    float s = tex->scale;
    V c1 = primary_color;
    V c2 = tex->color2;

    switch (tex->type) {
        case 1: {
            int u = (int)floorf(uv.x * s);
            int v = (int)floorf(uv.y * s);
            return ((u + v) & 1) ? c1 : c2;
        }
        case 2: {
            float cx = floorf(uv.x * s) + 0.5f;
            float cy = floorf(uv.y * s) + 0.5f;
            float dx = uv.x * s - cx;
            float dy = uv.y * s - cy;
            return (dx*dx + dy*dy < 0.12f) ? c1 : c2;
        }
        case 3: {
            float n = vnoise(uv.x * s * 0.5f, uv.y * s * 0.5f, 0.0f);
            float marble = sinf((uv.x + uv.y) * s * 1.5f + n * 3.0f) * 0.5f + 0.5f;
            return (V){c1.x * marble + c2.x * (1.0f - marble),
                       c1.y * marble + c2.y * (1.0f - marble),
                       c1.z * marble + c2.z * (1.0f - marble)};
        }
        case 4: {
            float dx = uv.x - 0.5f, dy = uv.y - 0.5f;
            float dist = sqrtf(dx*dx + dy*dy) * s * 2.0f;
            float ring = sinf(dist * (float)M_PI * 2.0f) * 0.5f + 0.5f;
            return (V){c1.x * ring + c2.x * (1.0f - ring),
                       c1.y * ring + c2.y * (1.0f - ring),
                       c1.z * ring + c2.z * (1.0f - ring)};
        }
        default:
            return c1;
    }
}

V eval_texture(V p, V primary_color, const TextureData* tex) {
    if (!tex || tex->type == 0) return primary_color;

    float s = tex->scale;
    V c1 = primary_color;
    V c2 = tex->color2;

    switch (tex->type) {
        case 1: {
            int ix = (int)floorf(p.x * s);
            int iy = (int)floorf(p.y * s);
            int iz = (int)floorf(p.z * s);
            return ((ix + iy + iz) & 1) ? c1 : c2;
        }
        case 2: {
            float cx = floorf(p.x * s) + 0.5f;
            float cy = floorf(p.y * s) + 0.5f;
            float cz = floorf(p.z * s) + 0.5f;
            float dx = p.x * s - cx;
            float dy = p.y * s - cy;
            float dz = p.z * s - cz;
            float dist = sqrtf(dx*dx + dy*dy + dz*dz);
            return (dist < 0.35f) ? c1 : c2;
        }
        case 3: {
            float n = vnoise(p.x * s * 0.5f, p.y * s * 0.5f, p.z * s * 0.5f);
            float marble = sinf((p.x + p.z) * s * 1.5f + n * 3.0f) * 0.5f + 0.5f;
            return (V){c1.x * marble + c2.x * (1.0f - marble),
                       c1.y * marble + c2.y * (1.0f - marble),
                       c1.z * marble + c2.z * (1.0f - marble)};
        }
        case 4: {
            float dist = sqrtf(p.x*p.x + p.z*p.z) * s;
            float ring = sinf(dist * (float)M_PI * 2.0f) * 0.5f + 0.5f;
            return (V){c1.x * ring + c2.x * (1.0f - ring),
                       c1.y * ring + c2.y * (1.0f - ring),
                       c1.z * ring + c2.z * (1.0f - ring)};
        }
        default:
            return c1;
    }
}
