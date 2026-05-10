#include "denoiser.h"
#include "../shading/shading.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <float.h>

#define EPS 1e-4f

typedef struct { float x, y, z; } V3;

static V3 v3(float x, float y, float z) { V3 r; r.x=x; r.y=y; r.z=z; return r; }
static V3 add3(V3 a, V3 b) { return v3(a.x+b.x, a.y+b.y, a.z+b.z); }
static V3 sub3(V3 a, V3 b) { return v3(a.x-b.x, a.y-b.y, a.z-b.z); }
static V3 mul3(V3 a, float s) { return v3(a.x*s, a.y*s, a.z*s); }
static float dot3(V3 a, V3 b) { return a.x*b.x + a.y*b.y + a.z*b.z; }
static float len3(V3 a) { return sqrtf(dot3(a, a)); }
static V3 norm3(V3 a) { float l = len3(a); return l > 1e-8f ? mul3(a, 1.0f/l) : v3(0,0,0); }
static V3 cross3(V3 a, V3 b) { return v3(a.y*b.z-a.z*b.y, a.z*b.x-a.x*b.z, a.x*b.y-a.y*b.x); }

static int g_hit_sphere(V3 o, V3 d, V3 c, float r, float* t) {
    V3 oc = sub3(o, c);
    float a = dot3(d, d);
    float b = 2.0f * dot3(oc, d);
    float cc = dot3(oc, oc) - r*r;
    float delta = b*b - 4*a*cc;
    if (delta < 0) return 0;
    float sd = sqrtf(delta);
    float t1 = (-b - sd) / (2.0f * a);
    float t2 = (-b + sd) / (2.0f * a);
    *t = (t1 > EPS) ? t1 : t2;
    return *t > EPS;
}

static int g_hit_floor(V3 o, V3 d, float* t) {
    if (fabsf(d.y) < EPS) return 0;
    *t = -o.y / d.y;
    return *t > EPS;
}

static int g_hit_tri(V3 o, V3 d, const float v0[3], const float v1[3], const float v2[3],
                     float* t, float* u, float* v) {
    V3 e1 = sub3(v3(v1[0],v1[1],v1[2]), v3(v0[0],v0[1],v0[2]));
    V3 e2 = sub3(v3(v2[0],v2[1],v2[2]), v3(v0[0],v0[1],v0[2]));
    V3 pv = cross3(d, e2);
    float det = dot3(e1, pv);
    if (fabsf(det) < EPS) return 0;
    float inv_det = 1.0f / det;
    V3 tv = sub3(o, v3(v0[0],v0[1],v0[2]));
    *u = dot3(tv, pv) * inv_det;
    if (*u < 0 || *u > 1) return 0;
    V3 qv = cross3(tv, e1);
    *v = dot3(d, qv) * inv_det;
    if (*v < 0 || *u + *v > 1) return 0;
    *t = dot3(e2, qv) * inv_det;
    return *t > EPS;
}

static int g_hit_mesh_linear(V3 o, V3 d, TriGpu* tris, int num_tris,
                              float* t, V3* hit_n, float* out_uv) {
    float best_t = 1e9f;
    int hit = 0;
    float best_u = 0, best_v = 0;
    TriGpu* best_tri = NULL;

    for (int i = 0; i < num_tris; i++) {
        float ti, u, v;
        if (g_hit_tri(o, d, tris[i].v0, tris[i].v1, tris[i].v2, &ti, &u, &v) && ti < best_t) {
            best_t = ti; hit = 1; best_tri = &tris[i]; best_u = u; best_v = v;
        }
    }

    if (hit) {
        *t = best_t;
        float w = 1.0f - best_u - best_v;
        float nx = w * best_tri->n0[0] + best_u * best_tri->n1[0] + best_v * best_tri->n2[0];
        float ny = w * best_tri->n0[1] + best_u * best_tri->n1[1] + best_v * best_tri->n2[1];
        float nz = w * best_tri->n0[2] + best_u * best_tri->n1[2] + best_v * best_tri->n2[2];
        float len = sqrtf(nx*nx + ny*ny + nz*nz);
        if (len > EPS) { nx /= len; ny /= len; nz /= len; }
        *hit_n = v3(nx, ny, nz);
        if (dot3(*hit_n, d) > 0) *hit_n = mul3(*hit_n, -1);
        if (out_uv) {
            out_uv[0] = w * best_tri->t0[0] + best_u * best_tri->t1[0] + best_v * best_tri->t2[0];
            out_uv[1] = w * best_tri->t0[1] + best_u * best_tri->t1[1] + best_v * best_tri->t2[1];
        }
    }
    return hit;
}

GBuffer* trace_gbuffer(const Scene* scene) {
    int w = scene->width, h = scene->height;
    GBuffer* gbuf = (GBuffer*)malloc(sizeof(GBuffer));
    gbuf->normal_x = (float*)calloc(w * h, sizeof(float));
    gbuf->normal_y = (float*)calloc(w * h, sizeof(float));
    gbuf->normal_z = (float*)calloc(w * h, sizeof(float));
    gbuf->depth = (float*)malloc(w * h * sizeof(float));
    gbuf->albedo_r = (float*)calloc(w * h, sizeof(float));
    gbuf->albedo_g = (float*)calloc(w * h, sizeof(float));
    gbuf->albedo_b = (float*)calloc(w * h, sizeof(float));

    V3 cam = v3(scene->camera_pos.x, scene->camera_pos.y, scene->camera_pos.z);
    V3 tgt = v3(scene->camera_target.x, scene->camera_target.y, scene->camera_target.z);
    V3 fwd = norm3(sub3(tgt, cam));
    V3 right = norm3(cross3(v3(0,1,0), fwd));
    V3 up = cross3(fwd, right);
    float asp = (float)w / h;

    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            float ux = (2.0f * (x + 0.5f) / w - 1.0f) * asp;
            float uy = 1.0f - 2.0f * (y + 0.5f) / h;
            V3 rd = norm3(add3(add3(fwd, mul3(right, ux)), mul3(up, uy)));

            float best_t = 1e9f;
            V3 hit_n = v3(0, 1, 0);
            V3 hit_col = v3(0.1f, 0.1f, 0.2f);

            for (int i = 0; i < scene->num_spheres; i++) {
                V3 c = v3(scene->spheres[i].pos.x, scene->spheres[i].pos.y, scene->spheres[i].pos.z);
                float t;
                if (g_hit_sphere(cam, rd, c, scene->spheres[i].radius, &t) && t < best_t) {
                    best_t = t;
                    V3 p = add3(cam, mul3(rd, t));
                    hit_n = norm3(sub3(p, c));
                    hit_col = v3(scene->spheres[i].color.x, scene->spheres[i].color.y, scene->spheres[i].color.z);
                }
            }

            for (int m = 0; m < scene->num_meshes; m++) {
                if (!scene->meshes[m].tris || scene->meshes[m].num_tris <= 0) continue;
                float tm;
                V3 nm;
                float uv[2];
                if (g_hit_mesh_linear(cam, rd, scene->meshes[m].tris, scene->meshes[m].num_tris,
                                      &tm, &nm, uv) && tm < best_t) {
                    best_t = tm;
                    hit_n = nm;
                    hit_col = v3(scene->meshes[m].color.x, scene->meshes[m].color.y, scene->meshes[m].color.z);
                }
            }

            float tf;
            if (scene->has_floor && g_hit_floor(cam, rd, &tf) && tf < best_t) {
                best_t = tf;
                hit_n = v3(0, 1, 0);
                V3 p = add3(cam, mul3(rd, tf));
                int ck = ((int)(p.x) + (int)(p.z)) & 1;
                hit_col = ck ? v3(0.08f, 0.12f, 0.25f) : v3(0.25f, 0.4f, 0.7f);
            }

            int idx = y * w + x;
            gbuf->normal_x[idx] = hit_n.x;
            gbuf->normal_y[idx] = hit_n.y;
            gbuf->normal_z[idx] = hit_n.z;
            gbuf->depth[idx] = best_t < 1e8f ? best_t : 1e9f;
            gbuf->albedo_r[idx] = hit_col.x;
            gbuf->albedo_g[idx] = hit_col.y;
            gbuf->albedo_b[idx] = hit_col.z;
        }
    }

    return gbuf;
}

void denoise(Image* img, const GBuffer* gbuf, int width, int height, float strength) {
    if (!img || !gbuf || strength <= 0) return;

    int radius = 4;
    float sigma_s = 3.0f;
    float sigma_n = 0.5f / fmaxf(strength, 0.01f);
    float sigma_d = 2.0f / fmaxf(strength, 0.01f);

    float* out = (float*)calloc(width * height * 3, sizeof(float));

    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            int ci = y * width + x;
            float sum_r = 0, sum_g = 0, sum_b = 0, sum_w = 0;

            float cnx = gbuf->normal_x[ci];
            float cny = gbuf->normal_y[ci];
            float cnz = gbuf->normal_z[ci];
            float cd = gbuf->depth[ci];

            int y0 = y - radius, y1 = y + radius;
            int x0 = x - radius, x1 = x + radius;
            if (y0 < 0) y0 = 0; if (y1 >= height) y1 = height - 1;
            if (x0 < 0) x0 = 0; if (x1 >= width) x1 = width - 1;

            for (int yy = y0; yy <= y1; yy++) {
                for (int xx = x0; xx <= x1; xx++) {
                    int ni = yy * width + xx;
                    float dx = (float)(xx - x);
                    float dy = (float)(yy - y);

                    float ws = expf(-0.5f * (dx*dx + dy*dy) / (sigma_s * sigma_s));

                    float nd = gbuf->normal_x[ni] * cnx + gbuf->normal_y[ni] * cny + gbuf->normal_z[ni] * cnz;
                    float wn = powf(fmaxf(nd, 0.0f), sigma_n * 4.0f);

                    float dd = fabsf(gbuf->depth[ni] - cd);
                    float wd = expf(-0.5f * dd * dd / (sigma_d * sigma_d * (cd * 0.001f + 1.0f)));

                    float w = ws * wn * wd;

                    size_t p_off = (yy * width + xx) * 3;
                    float pr = img->data[p_off] / 255.0f;
                    float pg = img->data[p_off+1] / 255.0f;
                    float pb = img->data[p_off+2] / 255.0f;

                    sum_r += pr * w;
                    sum_g += pg * w;
                    sum_b += pb * w;
                    sum_w += w;
                }
            }

            if (sum_w > 1e-6f) {
                out[ci*3]   = sum_r / sum_w;
                out[ci*3+1] = sum_g / sum_w;
                out[ci*3+2] = sum_b / sum_w;
            } else {
                size_t off = ci * 3;
                out[off]   = img->data[off] / 255.0f;
                out[off+1] = img->data[off+1] / 255.0f;
                out[off+2] = img->data[off+2] / 255.0f;
            }
        }
    }

    for (int i = 0; i < width * height; i++) {
        size_t off = i * 3;
        img->data[off]   = (uint8_t)(fminf(fmaxf(out[off],   0.0f), 1.0f) * 255.0f);
        img->data[off+1] = (uint8_t)(fminf(fmaxf(out[off+1], 0.0f), 1.0f) * 255.0f);
        img->data[off+2] = (uint8_t)(fminf(fmaxf(out[off+2], 0.0f), 1.0f) * 255.0f);
    }

    free(out);
}

void free_gbuffer(GBuffer* gbuf) {
    if (!gbuf) return;
    free(gbuf->normal_x);
    free(gbuf->normal_y);
    free(gbuf->normal_z);
    free(gbuf->depth);
    free(gbuf->albedo_r);
    free(gbuf->albedo_g);
    free(gbuf->albedo_b);
    free(gbuf);
}
