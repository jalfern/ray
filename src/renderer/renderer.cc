#include "renderer.h"
#include "../vector/vector.h"
#include "../shading/shading.h"
#include "../denoiser/denoiser.h"
#include "../envmap/envmap.h"
#include "bvh.h"
#include "thin_film.h"
#include "volume.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <thread>
#include <vector>
#include <float.h>

/* ── Debug counters ────────────────────────────────────────── */
static int g_debug_frame_done = 0;

static int g_hit_tri_tests[256] = {0};
static int g_hit_tri_hits[256] = {0};

#define EPS 1e-4f
#define AA_SAMPLES 16
#define MAX_DEPTH 4

/* KHR_materials_volume: the absorbing medium a ray currently travels
   through.  ior 1.0 = air.  Single-slot model — the glass path already
   hardcodes the outside medium as air, so only one medium is tracked
   at a time (nested volumes are not resolved). */
typedef struct {
    float ior;
    float cr, cg, cb;    /* attenuationColor, linear RGB */
    float att_dist;      /* attenuationDistance, may be +inf */
} Medium;

static Medium med_air(void) {
    return (Medium){1.0f, 1.0f, 1.0f, 1.0f, INFINITY};
}

static int mat_name_to_type(const char* name) {
    if (strcmp(name, "glass") == 0) return MAT_GLASS;
    if (strcmp(name, "plastic") == 0) return MAT_PLASTIC;
    if (strcmp(name, "emissive") == 0) return MAT_EMISSIVE;
    if (strcmp(name, "metallic") == 0) return MAT_METALLIC;
    if (strcmp(name, "subsurface") == 0) return MAT_SUBSURFACE;
    return MAT_GLASS;
}

static int hit_sphere(V o, V d, V c, float r, float *t) {
    V oc = sub(o, c);
    float a = dot(d, d);
    float b = 2.0f * dot(oc, d);
    float cc = dot(oc, oc) - r*r;
    float delta = b*b - 4*a*cc;
    if (delta < 0) return 0;
    float sd = sqrtf(delta);
    float t1 = (-b - sd) / (2.0f * a);
    float t2 = (-b + sd) / (2.0f * a);
    *t = (t1 > EPS) ? t1 : t2;
    return *t > EPS;
}

static int hit_any_sphere(V o, V d, float *t, V *hit_normal, int *hit_idx,
                          SphereData* spheres, int num_spheres) {
    float best_t = 1e9f;
    int hit = 0;
    *hit_idx = -1;
    for (int i = 0; i < num_spheres; i++) {
        float t_i;
        if (hit_sphere(o, d, spheres[i].c, spheres[i].r, &t_i) && t_i < best_t) {
            best_t = t_i; hit = 1; *hit_idx = i;
        }
    }
    if (hit) {
        *t = best_t;
        V p = add(o, mul(d, best_t));
        *hit_normal = norm(sub(p, spheres[*hit_idx].c));
    }
    return hit;
}

static int hit_tri(V o, V d, float v0[3], float v1[3], float v2[3], float *t,
                   float *u, float *v, int debug_mesh, int debug_tri) {
    V e1 = (V){v1[0]-v0[0], v1[1]-v0[1], v1[2]-v0[2]};
    V e2 = (V){v2[0]-v0[0], v2[1]-v0[1], v2[2]-v0[2]};
    V pv = cross(d, e2);
    float det = dot(e1, pv);
    float len1_sq = e1.x*e1.x + e1.y*e1.y + e1.z*e1.z;
    float len2_sq = e2.x*e2.x + e2.y*e2.y + e2.z*e2.z;
    float det_eps = 1e-7f * (len1_sq + len2_sq + 1e-12f) * 0.5f;
    if (debug_mesh >= 0 && g_hit_tri_tests[debug_mesh] < 10) {
        fprintf(stderr, "[hit_tri_debug] mesh=%d tri_idx=%d  "
                "v0=(%.10e,%.10e,%.10e) v1=(%.10e,%.10e,%.10e) v2=(%.10e,%.10e,%.10e)  "
                "e1=(%.10e,%.10e,%.10e) e2=(%.10e,%.10e,%.10e)  det=%.10e  |det|=%e  det_eps=%e",
                debug_mesh, debug_tri,
                v0[0], v0[1], v0[2], v1[0], v1[1], v1[2], v2[0], v2[1], v2[2],
                e1.x, e1.y, e1.z, e2.x, e2.y, e2.z,
                det, fabsf(det), det_eps);
        if (fabsf(det) < det_eps) {
            fprintf(stderr, "  REJECT: |det|<det_eps\n");
        } else {
            float inv_det = 1.0f / det;
            V tv = sub(o, (V){v0[0], v0[1], v0[2]});
            *u = dot(tv, pv) * inv_det;
            if (*u < 0 || *u > 1) {
                fprintf(stderr, "  REJECT: u=%.10e out of [0,1]\n", *u);
            } else {
                V qv = cross(tv, e1);
                *v = dot(d, qv) * inv_det;
                if (*v < 0 || *u + *v > 1) {
                    fprintf(stderr, "  REJECT: v=%.10e u+v=%.10e out of range\n", *v, *u + *v);
                } else {
                    *t = dot(e2, qv) * inv_det;
                    if (*t > EPS) {
                        fprintf(stderr, "  HIT: t=%.10e u=%.10e v=%.10e\n", *t, *u, *v);
                    } else {
                        fprintf(stderr, "  REJECT: t=%.10e <= EPS\n", *t);
                    }
                }
            }
        }
    }
    if (fabsf(det) < det_eps) return 0;
    float inv_det = 1.0f / det;
    V tv = sub(o, (V){v0[0], v0[1], v0[2]});
    *u = dot(tv, pv) * inv_det;
    if (*u < 0 || *u > 1) return 0;
    V qv = cross(tv, e1);
    *v = dot(d, qv) * inv_det;
    if (*v < 0 || *u + *v > 1) return 0;
    *t = dot(e2, qv) * inv_det;
    return *t > EPS;
}

static int bbox_hit(V o, V d, const float* bmin, const float* bmax) {
    float tmin = 0, tmax = 1e9f;
    for (int a = 0; a < 3; a++) {
        float inv = 1.0f / (&d.x)[a];
        float t0 = ((&bmin[0])[a] - (&o.x)[a]) * inv;
        float t1 = ((&bmax[0])[a] - (&o.x)[a]) * inv;
        if (inv < 0) { float tmp = t0; t0 = t1; t1 = tmp; }
        tmin = fmaxf(tmin, t0);
        tmax = fminf(tmax, t1);
        if (tmax < tmin) return 0;
    }
    return 1;
}

static int hit_mesh_bvh(V o, V d, float *t, V *hit_normal, float* out_uv,
                          TriGpu* tris, BvhNode* nodes, int /*num_nodes*/, int mesh_idx,
                          int* side_out) {
    float best_t = 1e9f;
    int hit = 0;
    float best_u = 0, best_v = 0;
    TriGpu* best_tri = NULL;

    int stack[64];
    int sp = 0;
    stack[sp++] = 0;

    while (sp > 0) {
        int ni = stack[--sp];
        BvhNode* node = &nodes[ni];
        if (!bbox_hit(o, d, node->bbox_min, node->bbox_max)) continue;

        if (node->left >= 0) {
            if (sp + 2 > 64) continue;
            stack[sp++] = node->left;
            stack[sp++] = node->right;
        } else {
            for (int i = node->tri_start; i < node->tri_end; i++) {
                float ti, u, v;
                if (mesh_idx >= 0 && mesh_idx < 256) g_hit_tri_tests[mesh_idx]++;
                int dbg_mesh = (mesh_idx == 1) ? 1 : -1;
                int dbg_tri = (mesh_idx == 1) ? i : -1;
                if (hit_tri(o, d, tris[i].v0, tris[i].v1, tris[i].v2, &ti, &u, &v, dbg_mesh, dbg_tri) && ti < best_t) {
                    best_t = ti; hit = 1; best_tri = &tris[i]; best_u = u; best_v = v;
                    if (mesh_idx >= 0 && mesh_idx < 256) g_hit_tri_hits[mesh_idx]++;
                }
            }
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
        /* Side of the shell from the stored (pre-flip) normal: 1 = the ray
           hits the front of the shell (outward normal faces the ray),
           0 = it hits the inside (backface).  Volume entry/exit is decided
           from this sign, because the returned normal is always flipped to
           face the ray below. */
        if (side_out) *side_out = (nx*d.x + ny*d.y + nz*d.z) < 0 ? 1 : 0;
        *hit_normal = (V){nx, ny, nz};
        if (dot(*hit_normal, d) > 0) *hit_normal = mul(*hit_normal, -1);
        out_uv[0] = w * best_tri->t0[0] + best_u * best_tri->t1[0] + best_v * best_tri->t2[0];
        out_uv[1] = w * best_tri->t0[1] + best_u * best_tri->t1[1] + best_v * best_tri->t2[1];
    }
    return hit;
}

static int hit_mesh_bvh_any(V o, V d, float max_t,
                              TriGpu* tris, BvhNode* nodes, int /*num_nodes*/, int mesh_idx) {
    int stack[64];
    int sp = 0;
    stack[sp++] = 0;
    while (sp > 0) {
        int ni = stack[--sp];
        BvhNode* node = &nodes[ni];
        if (!bbox_hit(o, d, node->bbox_min, node->bbox_max)) continue;
        if (node->left >= 0) {
            if (sp + 2 > 64) continue;
            stack[sp++] = node->left;
            stack[sp++] = node->right;
        } else {
            for (int i = node->tri_start; i < node->tri_end; i++) {
                float t, u, v;
                if (mesh_idx >= 0 && mesh_idx < 256) g_hit_tri_tests[mesh_idx]++;
                int dbg_mesh = (mesh_idx == 1) ? 1 : -1;
                int dbg_tri = (mesh_idx == 1) ? i : -1;
                if (hit_tri(o, d, tris[i].v0, tris[i].v1, tris[i].v2, &t, &u, &v, dbg_mesh, dbg_tri) &&
                    t < max_t && t > EPS) {
                    if (mesh_idx >= 0 && mesh_idx < 256) g_hit_tri_hits[mesh_idx]++;
                    return 1;
                }
            }
        }
    }
    return 0;
}

static int hit_floor(V o, V d, float *t) {
    if (fabsf(d.y) < EPS) return 0;
    *t = -o.y / d.y;
    return *t > EPS;
}

/* sRGB to linear per channel. */
static float srgb_to_linear(float c) {
    return c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f);
}

/* Sample an RGBA8 texture with bilinear filtering and wrap-repeat. */
static V sample_texture(ImageTexture* tex, float u, float v) {
    u = u - floorf(u);
    v = v - floorf(v);
    float fx = u * tex->width - 0.5f;
    float fy = v * tex->height - 0.5f;
    int ix = (int)floorf(fx);
    int iy = (int)floorf(fy);
    float rx = fx - ix;
    float ry = fy - iy;
    int x0 = (ix + tex->width * 1024) % tex->width;
    int y0 = (iy + tex->height * 1024) % tex->height;
    int x1 = (x0 + 1) % tex->width;
    int y1 = (y0 + 1) % tex->height;
    unsigned char* t = tex->data;
    float c00r = srgb_to_linear(t[(y0 * tex->width + x0) * 4 + 0] / 255.0f);
    float c00g = srgb_to_linear(t[(y0 * tex->width + x0) * 4 + 1] / 255.0f);
    float c00b = srgb_to_linear(t[(y0 * tex->width + x0) * 4 + 2] / 255.0f);
    float c10r = srgb_to_linear(t[(y0 * tex->width + x1) * 4 + 0] / 255.0f);
    float c10g = srgb_to_linear(t[(y0 * tex->width + x1) * 4 + 1] / 255.0f);
    float c10b = srgb_to_linear(t[(y0 * tex->width + x1) * 4 + 2] / 255.0f);
    float c01r = srgb_to_linear(t[(y1 * tex->width + x0) * 4 + 0] / 255.0f);
    float c01g = srgb_to_linear(t[(y1 * tex->width + x0) * 4 + 1] / 255.0f);
    float c01b = srgb_to_linear(t[(y1 * tex->width + x0) * 4 + 2] / 255.0f);
    float c11r = srgb_to_linear(t[(y1 * tex->width + x1) * 4 + 0] / 255.0f);
    float c11g = srgb_to_linear(t[(y1 * tex->width + x1) * 4 + 1] / 255.0f);
    float c11b = srgb_to_linear(t[(y1 * tex->width + x1) * 4 + 2] / 255.0f);
    float r = (1-ry)*((1-rx)*c00r + rx*c10r) + ry*((1-rx)*c01r + rx*c11r);
    float g = (1-ry)*((1-rx)*c00g + rx*c10g) + ry*((1-rx)*c01g + rx*c11g);
    float b = (1-ry)*((1-rx)*c00b + rx*c10b) + ry*((1-rx)*c01b + rx*c11b);
    return (V){r, g, b};
}

/* Iridescence thickness map: linear data in the GREEN channel (matches
   three.js, which reads `.g`; KHR_materials_iridescence's sample model
   stores its data there).  Bilinear, wrap-repeat, no transfer function. */
static float sample_iri_thickness(ImageTexture* tex, float u, float v) {
    u = u - floorf(u);
    v = v - floorf(v);
    float fx = u * tex->width - 0.5f;
    float fy = v * tex->height - 0.5f;
    int ix = (int)floorf(fx);
    int iy = (int)floorf(fy);
    float rx = fx - ix;
    float ry = fy - iy;
    int x0 = (ix + tex->width * 1024) % tex->width;
    int y0 = (iy + tex->height * 1024) % tex->height;
    int x1 = (x0 + 1) % tex->width;
    int y1 = (y0 + 1) % tex->height;
    unsigned char* t = tex->data;
    float g00 = t[(y0 * tex->width + x0) * 4 + 1] / 255.0f;
    float g10 = t[(y0 * tex->width + x1) * 4 + 1] / 255.0f;
    float g01 = t[(y1 * tex->width + x0) * 4 + 1] / 255.0f;
    float g11 = t[(y1 * tex->width + x1) * 4 + 1] / 255.0f;
    return (1-ry)*((1-rx)*g00 + rx*g10) + ry*((1-rx)*g01 + rx*g11);
}

static V tone_map(V c, float exposure) {
    V s = mul(c, exposure);
    return (V){s.x / (1.0f + s.x), s.y / (1.0f + s.y), s.z / (1.0f + s.z)};
}

static V area_light_sample(V light_pos, float light_size, int sample_idx) {
    if (light_size <= 0) return light_pos;
    int sx = sample_idx & 3;
    int sy = (sample_idx >> 2) & 3;
    float angle = 2.0f * (float)M_PI * (sx + 0.5f) / 4.0f;
    float r = light_size * sqrtf((sy + 0.5f) / 4.0f);
    return add(light_pos, (V){r * cosf(angle), 0, r * sinf(angle)});
}

static float tri_area(TriGpu* tri) {
    float e1x = tri->v1[0] - tri->v0[0];
    float e1y = tri->v1[1] - tri->v0[1];
    float e1z = tri->v1[2] - tri->v0[2];
    float e2x = tri->v2[0] - tri->v0[0];
    float e2y = tri->v2[1] - tri->v0[1];
    float e2z = tri->v2[2] - tri->v0[2];
    float cx = e1y * e2z - e1z * e2y;
    float cy = e1z * e2x - e1x * e2z;
    float cz = e1x * e2y - e1y * e2x;
    return 0.5f * sqrtf(cx*cx + cy*cy + cz*cz);
}

static V sample_emissive_sphere(V c, float r, V* normal, int sample_idx, int ei) {
    int sx = (sample_idx + ei * 7) & 3;
    int sy = ((sample_idx >> 2) + ei * 3) & 3;
    float theta = 2.0f * (float)M_PI * (sx + 0.5f) / 4.0f;
    float phi = acosf(1.0f - 2.0f * (sy + 0.5f) / 4.0f);
    float x = sinf(phi) * cosf(theta);
    float y = cosf(phi);
    float z = sinf(phi) * sinf(theta);
    *normal = (V){x, y, z};
    return add(c, mul(*normal, r));
}

static V sample_emissive_mesh(TriGpu* tris, float* tri_cdf, int num_tris, float total_area,
                               V* normal, float* out_dist, int sample_idx, int ei, V from) {
    int rnd = (sample_idx * 257 + ei * 101 + 53) & 0xFFFF;
    float r = (float)rnd / 65536.0f * total_area;
    int lo = 0, hi = num_tris;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (tri_cdf[mid + 1] <= r) lo = mid + 1;
        else hi = mid;
    }
    int ti = lo;
    if (ti >= num_tris) ti = num_tris - 1;

    int sx = (sample_idx + ei * 11) & 3;
    int sy = ((sample_idx >> 2) + ei * 5) & 3;
    float su = (sx + 0.5f) / 4.0f;
    float sv = (sy + 0.5f) / 4.0f;
    if (su + sv > 1.0f) { su = 1.0f - su; sv = 1.0f - sv; }

    float px = tris[ti].v0[0] + su * (tris[ti].v1[0] - tris[ti].v0[0]) + sv * (tris[ti].v2[0] - tris[ti].v0[0]);
    float py = tris[ti].v0[1] + su * (tris[ti].v1[1] - tris[ti].v0[1]) + sv * (tris[ti].v2[1] - tris[ti].v0[1]);
    float pz = tris[ti].v0[2] + su * (tris[ti].v1[2] - tris[ti].v0[2]) + sv * (tris[ti].v2[2] - tris[ti].v0[2]);

    float w = 1.0f - su - sv;
    float nx = w * tris[ti].n0[0] + su * tris[ti].n1[0] + sv * tris[ti].n2[0];
    float ny = w * tris[ti].n0[1] + su * tris[ti].n1[1] + sv * tris[ti].n2[1];
    float nz = w * tris[ti].n0[2] + su * tris[ti].n1[2] + sv * tris[ti].n2[2];
    float len = sqrtf(nx*nx + ny*ny + nz*nz);
    if (len > 1e-8f) { nx /= len; ny /= len; nz /= len; }
    *normal = (V){nx, ny, nz};
    /* Flip normal toward the shaded point so winding doesn't silently kill emission. */
    V to_p = norm(sub(from, (V){px, py, pz}));
    if (dot(*normal, to_p) < 0) *normal = mul(*normal, -1);

    V to = sub((V){px, py, pz}, from);
    *out_dist = sqrtf(dot(to, to));
    return (V){px, py, pz};
}

static int emissive_visible(V p, V light_pos, float light_dist,
                            SphereData* spheres, int num_spheres,
                            MeshObjData* meshes, int num_meshes,
                            int skip_sphere, int skip_mesh) {
    V to_light = sub(light_pos, p);
    float ld = light_dist > 0 ? light_dist : sqrtf(dot(to_light, to_light));
    V ray_dir = norm(to_light);
    V ray_o = add(p, mul(ray_dir, 1e-4f));
    for (int i = 0; i < num_spheres; i++) {
        if (i == skip_sphere) continue;
        float t_hit;
        if (hit_sphere(ray_o, ray_dir, spheres[i].c, spheres[i].r, &t_hit)) {
            if (t_hit < ld - 1e-4f && t_hit > 1e-4f) return 0;
        }
    }
    for (int m = 0; m < num_meshes; m++) {
        if (m == skip_mesh) continue;
        if (meshes[m].num_bvh_nodes > 0 &&
            hit_mesh_bvh_any(ray_o, ray_dir, ld - 1e-4f,
                             meshes[m].tris, meshes[m].bvh_nodes, meshes[m].num_bvh_nodes, m))
            return 0;
    }
    return 1;
}

static float in_shadow(V p, LightData light, SphereData* spheres, int num_spheres,
                       MeshObjData* meshes, int num_meshes, int sample_idx,
                       int skip_sphere, int skip_mesh) {
    V light_pos = area_light_sample(light.pos, light.size, sample_idx);
    V to_light = sub(light_pos, p);
    float light_dist = sqrtf(dot(to_light, to_light));
    V ray_dir = norm(to_light);
    V ray_o = add(p, mul(ray_dir, EPS));

    for (int i = 0; i < num_spheres; i++) {
        if (i == skip_sphere) continue;
        float t_hit;
        if (hit_sphere(ray_o, ray_dir, spheres[i].c, spheres[i].r, &t_hit)) {
            if (t_hit < light_dist && t_hit > EPS) return 1;
        }
    }
    for (int m = 0; m < num_meshes; m++) {
        if (m == skip_mesh) continue;
        if (meshes[m].num_bvh_nodes > 0 &&
            hit_mesh_bvh_any(ray_o, ray_dir, light_dist,
                             meshes[m].tris, meshes[m].bvh_nodes, meshes[m].num_bvh_nodes, m))
            return 1;
    }
    return 0;
}

static V trace_ray(V o, V d, int depth, SphereData* spheres, int num_spheres,
                   MeshObjData* meshes, int num_meshes,
                   LightData* lights, int num_lights,
                   EmissiveSurf* emissive, int num_emissive,
                   int sample_idx, EnvMap* env,
                   ImageTexture* textures, int num_textures,
                   Medium med) {
    if (depth > MAX_DEPTH) return (V){0,0,0};

    float ts, tf;
    V sn;
    int si = -1;
    int hs = hit_any_sphere(o, d, &ts, &sn, &si, spheres, num_spheres);

    float tm = 1e9f;
    V mn = {0,0,0};
    float m_uv[2] = {0,0};
    int mi = -1;
    int m_side = 1;  /* shell front(1)/back(0) side of the winning mesh hit */
    for (int i = 0; i < num_meshes; i++) {
        V hit_n;
        float tmi;
        float uv[2];
        int this_side = 1;
        if (meshes[i].num_bvh_nodes > 0 &&
            hit_mesh_bvh(o, d, &tmi, &hit_n, uv, meshes[i].tris, meshes[i].bvh_nodes, meshes[i].num_bvh_nodes, i, &this_side) && tmi < tm) {
            tm = tmi; mi = i; mn = hit_n; m_uv[0] = uv[0]; m_uv[1] = uv[1];
            /* Capture `side` only for the WINNING mesh hit — hit_mesh_bvh
               writes *side_out whenever this mesh has any hit, so without
               this the last-processed mesh (not the nearest one) would win. */
            m_side = this_side;
        }
    }
    int hm = (mi >= 0);

    int hf = hit_floor(o, d, &tf);

    int hit_type = 0;
    int side = 1;   /* spheres always report their near face (a front hit) */
    float t_hit;
    V hit_n;
    float sphere_col[3] = {1,1,1};
    float sphere_ref = 0, sphere_ior = 1.5f, sphere_rough = 1.0f;
    float sphere_metallic = 0.0f, sphere_ao = 1.0f;
    int sphere_mat = 0;

    if (hs && (!hf || ts < tf) && (!hm || ts < tm)) {
        hit_type = 1; t_hit = ts; hit_n = sn;
        sphere_col[0] = spheres[si].col.x;
        sphere_col[1] = spheres[si].col.y;
        sphere_col[2] = spheres[si].col.z;
        sphere_ref = spheres[si].ref;
        sphere_ior = spheres[si].ior;
        sphere_rough = spheres[si].roughness;
        sphere_mat = spheres[si].mat_type;
        sphere_metallic = (sphere_mat == MAT_METALLIC) ? 1.0f : 0.0f;
    } else if (hm && (!hf || tm < tf)) {
        hit_type = 2; t_hit = tm; hit_n = mn; side = m_side;
        sphere_col[0] = meshes[mi].col.x;
        sphere_col[1] = meshes[mi].col.y;
        sphere_col[2] = meshes[mi].col.z;
        sphere_ref = meshes[mi].ref;
        sphere_ior = meshes[mi].ior;
        sphere_rough = meshes[mi].roughness;
        sphere_metallic = meshes[mi].metallic;
        sphere_mat = meshes[mi].mat_type;
    } else if (hf) {
        hit_type = 3; t_hit = tf;
    }

    if (hit_type == 0) {
        /* A ray leaving through the environment while inside an
           absorbing medium travels an infinite distance: T(inf) = 0. */
        if (med.ior > 1.0f &&
            vol_sigma_nonzero(med.cr, med.cg, med.cb, med.att_dist))
            return (V){0,0,0};
        float er, eg, eb;
        envmap_sample(env, d.x, d.y, d.z, &er, &eg, &eb);
        return (V){er, eg, eb};
    }

    V p = add(o, mul(d, t_hit));

    if (hit_type == 3) {
        V n_floor = (V){0, 1, 0};
        V base = floor_color(p);
        V lit = mul(base, 0.15f);
        for (int li = 0; li < num_lights; li++) {
            V light_pos = lights[li].pos;
            V light_dir = norm(sub(light_pos, p));
            int sidx = (sample_idx << 2) | li;
            float sf = in_shadow(p, lights[li], spheres, num_spheres,
                                 meshes, num_meshes, sidx, -1, -1);
            float diff = fmaxf(0.0f, dot(n_floor, light_dir));
            float lf = sf ? 0.2f : 1.0f;
            lit = add(lit, mul(base, diff * lf));
        }
        for (int ei = 0; ei < num_emissive; ei++) {
            V lp, ln;
            float ldist, pdf;
            if (emissive[ei].type == 0) {
                lp = sample_emissive_sphere(emissive[ei].c, emissive[ei].r, &ln, sample_idx, ei);
                V dl = sub(lp, p);
                ldist = sqrtf(dot(dl, dl));
                pdf = 2.0f / emissive[ei].area;
            } else {
                lp = sample_emissive_mesh(emissive[ei].tris, emissive[ei].tri_cdf,
                                           emissive[ei].num_tris, emissive[ei].total_area,
                                           &ln, &ldist, sample_idx, ei, p);
                pdf = 1.0f / emissive[ei].total_area;
            }
            if (ldist < 1e-4f) continue;
            V wi = norm(sub(lp, p));
            float cos_surf = fmaxf(0.0f, dot(n_floor, wi));
            if (cos_surf <= 0) continue;
            float cos_light = fmaxf(0.0f, dot(ln, mul(wi, -1)));
            if (cos_light <= 0) continue;
            float G = cos_surf * cos_light / fmaxf(ldist * ldist, 1e-3f);
            int vis = emissive_visible(p, lp, ldist, spheres, num_spheres,
                                        meshes, num_meshes,
                                        emissive[ei].type == 0 ? emissive[ei].src_idx : -1,
                                        emissive[ei].type == 1 ? emissive[ei].src_idx : -1);
            if (vis) {
                V le = emissive[ei].emitted;
                V em_contrib = mul(le, G / pdf);
                lit = add(lit, (V){base.x * em_contrib.x, base.y * em_contrib.y, base.z * em_contrib.z});
            }
        }
        return lit;
    }

    V n = hit_n;
    V sc = (V){sphere_col[0], sphere_col[1], sphere_col[2]};
    int mat = sphere_mat;

    if (hit_type == 1) {
        sc = eval_texture(p, sc, &spheres[si].tex);
    } else if (hit_type == 2) {
        V uv = (V){m_uv[0], m_uv[1], 0};
        if (meshes[mi].tex_index >= 0 && meshes[mi].tex_index < num_textures && textures) {
            sc = sample_texture(&textures[meshes[mi].tex_index], m_uv[0], m_uv[1]);
        } else {
            sc = (m_uv[0] != 0 || m_uv[1] != 0) ? eval_texture_uv(uv, sc, &meshes[mi].tex)
                                                 : eval_texture(p, sc, &meshes[mi].tex);
        }
        /* Override material params from ORM texture (linear data): G = roughness, B = metallic, R = AO. */
        if (meshes[mi].orm_tex_index >= 0 && meshes[mi].orm_tex_index < num_textures && textures) {
            ImageTexture* orm = &textures[meshes[mi].orm_tex_index];
            float u = m_uv[0] - floorf(m_uv[0]);
            float v = m_uv[1] - floorf(m_uv[1]);
            float fx = u * orm->width - 0.5f;
            float fy = v * orm->height - 0.5f;
            int ix = (int)floorf(fx), iy = (int)floorf(fy);
            float rx = fx - ix, ry = fy - iy;
            int x0 = (ix + orm->width * 1024) % orm->width;
            int y0 = (iy + orm->height * 1024) % orm->height;
            int x1 = (x0 + 1) % orm->width;
            int y1 = (y0 + 1) % orm->height;
            unsigned char* d = orm->data;
            int p00 = (y0 * orm->width + x0) * 4, p10 = (y0 * orm->width + x1) * 4;
            int p01 = (y1 * orm->width + x0) * 4, p11 = (y1 * orm->width + x1) * 4;
            float r00 = d[p00] / 255.0f, g00 = d[p00 + 1] / 255.0f, b00 = d[p00 + 2] / 255.0f;
            float r10 = d[p10] / 255.0f, g10 = d[p10 + 1] / 255.0f, b10 = d[p10 + 2] / 255.0f;
            float r01 = d[p01] / 255.0f, g01 = d[p01 + 1] / 255.0f, b01 = d[p01 + 2] / 255.0f;
            float r11 = d[p11] / 255.0f, g11 = d[p11 + 1] / 255.0f, b11 = d[p11 + 2] / 255.0f;
            float orm_g = (1-ry)*((1-rx)*g00 + rx*g10) + ry*((1-rx)*g01 + rx*g11);
            float orm_b = (1-ry)*((1-rx)*b00 + rx*b10) + ry*((1-rx)*b01 + rx*b11);
            float orm_r = (1-ry)*((1-rx)*r00 + rx*r10) + ry*((1-rx)*r01 + rx*r11);
            sphere_rough = sphere_rough * orm_g;
            sphere_metallic = sphere_metallic * orm_b;
            sphere_ao = orm_r;
        }
    }

    if (mat == MAT_EMISSIVE) return sc;

    /* Merged plastic+metallic PBR params (per pixel):
       kd = 1 - metallic, F0 = mix(0.04, basecolor, metallic).
       F0 is sc when metallic=1 and 0.04 when metallic=0. */
    float kd = 1.0f - sphere_metallic;
    V f0 = add(mul(sc, sphere_metallic), mul((V){0.04f, 0.04f, 0.04f}, kd));

    /* KHR_materials_iridescence: thin-film interference tint on the
       specular/reflection lobes, view-direction angle blend per the
       reference model.  Entering hits only — the film coats the outer
       surface (a glass exit ray sees no film).  Diffuse/ambient terms
       are untouched (specular/reflection tint only). */
    V film = (V){0, 0, 0};
    float film_w = 0.0f;
    V f0mix = f0;
    if (hit_type == 2 && meshes[mi].iri_factor > 0.0f &&
        (mat != MAT_GLASS || dot(n, d) < 0.0f)) {
        float tv = 1.0f; /* three.js: no thickness map -> maximum thickness */
        if (meshes[mi].iri_tex_index >= 0 && meshes[mi].iri_tex_index < num_textures &&
            textures) {
            tv = sample_iri_thickness(&textures[meshes[mi].iri_tex_index],
                                      m_uv[0], m_uv[1]);
        }
        float d_nm = meshes[mi].iri_thin_min +
                     (meshes[mi].iri_thin_max - meshes[mi].iri_thin_min) * tv;
        float cv = fabsf(dot(n, norm(sub(o, p))));
        if (cv > 1.0f) cv = 1.0f;
        V bf0 = f0;
        if (mat == MAT_GLASS) {
            /* Dielectric substrate F0 from the glass IOR (three.js feeds the
               material's specularColor; r0 ~ 0.054 for IOR 1.6). */
            float gr = (1.0f - sphere_ior) / (1.0f + sphere_ior);
            gr *= gr;
            bf0 = mul(sc, gr);
        }
        film_w = meshes[mi].iri_factor;
        if (film_w > 1.0f) film_w = 1.0f;
        film = tf_eval_iridescence(1.0f, meshes[mi].iri_ior, cv, d_nm, bf0);
        V film_f0 = tf_schlick_to_f0(film, 1.0f, cv);
        f0mix = add(mul(f0, 1.0f - film_w), mul(film_f0, film_w));
    }

    V lit = {0, 0, 0};
    for (int li = 0; li < num_lights; li++) {
        V light_pos = lights[li].pos;
        V light_dir = norm(sub(light_pos, p));
        int sidx = (sample_idx << 2) | li;
        float sf = in_shadow(p, lights[li], spheres, num_spheres,
                             meshes, num_meshes, sidx,
                             hit_type == 1 ? si : -1,
                             hit_type == 2 ? mi : -1);

        float diff = fmaxf(0.0f, dot(n, light_dir));
        V view = norm(sub(o, p));
        V half = norm(add(light_dir, view));

        float spec_exp = 2.0f + 510.0f * (1.0f - sphere_rough) * (1.0f - sphere_rough);
        float spec_str = (mat == MAT_GLASS) ? 0.8f : 0.4f;
        float spec = powf(fmaxf(0.0f, dot(n, half)), spec_exp);
        float lf = sf ? 0.0f : 1.0f;

        /* AO (ORM.R) attenuates the ambient and diffuse terms only; the
           specular lobe and the F0-weighted mirror are untouched. */
        if (mat == MAT_SUBSURFACE) {
            float bdiff = fmaxf(0.0f, dot(mul(n, -1), light_dir));
            lit = add(lit, add(mul(sc, diff * lf * 0.7f * sphere_ao),
                              add(mul(sc, bdiff * lf * 0.3f * sphere_ao),
                                  mul(sc, spec * spec_str * lf))));
        } else if (mat == MAT_GLASS) {
            /* Specular weight takes the film response per channel when present. */
            V glw = mul(sc, spec_str);
            if (film_w > 0.0f) glw = add(mul(glw, 1.0f - film_w), mul(film, film_w));
            lit = add(lit, add(mul(sc, diff * lf * sphere_ao), mul(glw, spec * lf)));
        } else {
            /* Merged plastic+metallic PBR: diffuse * (1-metallic) * AO, specular F0. */
            lit = add(lit, add(mul(mul(sc, kd), diff * lf * sphere_ao), mul(f0mix, spec * spec_str * lf)));
        }
    }

    for (int ei = 0; ei < num_emissive; ei++) {
        int skip_sph = -1, skip_mesh = -1;
        V lp, ln;
        float ldist, pdf;
        if (emissive[ei].type == 0) {
            int esi = emissive[ei].src_idx;
            if (hit_type == 1 && si == esi) continue;
            skip_sph = esi;
            lp = sample_emissive_sphere(emissive[ei].c, emissive[ei].r, &ln, sample_idx, ei);
            V dl = sub(lp, p);
            ldist = sqrtf(dot(dl, dl));
            pdf = 2.0f / emissive[ei].area;
        } else {
            int emi = emissive[ei].src_idx;
            if (hit_type == 2 && mi == emi) continue;
            skip_mesh = emi;
            lp = sample_emissive_mesh(emissive[ei].tris, emissive[ei].tri_cdf,
                                       emissive[ei].num_tris, emissive[ei].total_area,
                                       &ln, &ldist, sample_idx, ei, p);
            pdf = 1.0f / emissive[ei].total_area;
        }

        if (ldist < 1e-4f) continue;
        V wi = norm(sub(lp, p));
        float cos_surf = fmaxf(0.0f, dot(n, wi));
        if (cos_surf <= 0) continue;
        float cos_light = fmaxf(0.0f, dot(ln, mul(wi, -1)));
        if (cos_light <= 0) continue;
        float G = cos_surf * cos_light / fmaxf(ldist * ldist, 1e-3f);
        int vis = emissive_visible(p, lp, ldist, spheres, num_spheres,
                                    meshes, num_meshes, skip_sph, skip_mesh);
        if (vis) {
            V le = emissive[ei].emitted;
            V em_contrib = mul(le, G / pdf);
            V emd = mul(sc, kd * sphere_ao);
            lit = add(lit, (V){emd.x * em_contrib.x, emd.y * em_contrib.y, emd.z * em_contrib.z});
        }
    }

    V ambient = mul(sc, 0.15f * sphere_ao);
    V base_color = add(ambient, lit);

    if (mat == MAT_SUBSURFACE) return base_color;

    float cos_i = dot(n, d);
    int entering = cos_i < 0;
    V n_adj = entering ? n : mul(n, -1);
    cos_i = entering ? -cos_i : cos_i;

    /* KHR_materials_volume: light that has traveled t_hit inside a
       medium loses that segment's Beer-Lambert energy before any
       downstream contribution; charged once per in-medium hit on both
       the reflection and the refraction.  Tseg is exactly (1,1,1) when
       there is no medium (or its attenuation is the default), leaving
       these bakes byte-identical. */
    V Tseg = (V){1.0f, 1.0f, 1.0f};
    if (med.ior > 1.0f)
        Tseg = vol_transmittance(t_hit, med.cr, med.cg, med.cb, med.att_dist);
    V refl_dir = sub(d, mul(n_adj, 2.0f * dot(d, n_adj)));
    V refl_origin = add(p, mul(refl_dir, EPS));
    V refl_col = trace_ray(refl_origin, refl_dir, depth + 1,
                           spheres, num_spheres, meshes, num_meshes,
                           lights, num_lights, emissive, num_emissive,
                           sample_idx, env, textures, num_textures,
                           med);
    refl_col = (V){refl_col.x * Tseg.x, refl_col.y * Tseg.y, refl_col.z * Tseg.z};

    if (mat == MAT_PLASTIC || mat == MAT_METALLIC) {
        /* Unified PBR mirror: reflection weighted per-channel by F0
           (the old basecolor-tinted metal mirror at metallic=1). */
        return add(base_color, (V){refl_col.x * f0mix.x, refl_col.y * f0mix.y, refl_col.z * f0mix.z});
    }

    float reflectivity = sphere_ref;
    float ior = sphere_ior;

    /* Hit geometry: on a mesh face the `entering` flag (cos_i < 0 of the
       flipped normal) is ALWAYS true — the returned normal is flipped to
       face the ray, so cos_i <= 0.  The legacy n1/n2 therefore read every
       mesh crossing with eta = 1/ior: correct at the FRONT (air -> glass)
       but wrong at the BACK (glass -> air), where physics wants
       eta = ior.  With the wrong back eta the exit bends toward the normal
       like an entry and reconverges inside the shell — the in-glass walk.
       (There is no entry defect and no legacy TIR: eta < 1 everywhere
       makes k < 0 impossible.)  The pre-flip stored-normal sign
       (hit_mesh_bvh's side_out) is the true front/back test: front
       eta = 1/ior, back eta = ior.  n_refr needs no side dependence: on the
       mesh n_adj is the flipped normal itself, which is the correct
       refraction normal for both crossings (the n_refr = n assignment in the
       back case is bit-identical on that flag and is retained for the
       measure-zero grazing case where entering could read false).  Sphere
       normals are not pre-flipped, so their n1/n2 stays the genuine state
       test and is untouched. */
    V n_refr = n_adj;
    float eta;
    if (hit_type == 2) {
        if (side)
            eta = 1.0f / ior;
        else {
            n_refr = n;
            eta = ior;
        }
    } else {
        float n1 = entering ? 1.0f : ior;
        float n2 = entering ? ior : 1.0f;
        eta = n1 / n2;
    }
    float k = 1.0f - eta * eta * (1.0f - cos_i * cos_i);

    /* Downstream medium for the refracted ray.  A volume boundary is a
       closed (convex) shell and the hit normal is always flipped to face
       the ray, so front/back detection uses the stored mesh normal: a
       front-face crossing lands inside the volume (medium = this
       material); a back-face crossing exits it (air).  KHR: the
       thicknessFactor > 0 switch selects volumetric behavior — the medium
       is INTENTIONALLY gated on vol_th > 0 (only volumes carry a medium;
       the corrected eta/n_refr and the origin push above apply to ALL mesh
       glass). */
    Medium refr_med = med;
    if (hit_type == 2 && meshes[mi].vol_th > 0.0f) {
        if (side)
            refr_med = (Medium){sphere_ior, meshes[mi].att_r, meshes[mi].att_g,
                                meshes[mi].att_b, meshes[mi].att_dist};
        else
            refr_med = med_air();
    }

    V refr_col = {0, 0, 0};
    if (k > 0) {
        float cos_t = sqrtf(k);
        V refr_dir = add(mul(d, eta), mul(n_refr, eta * cos_i - cos_t));
        /* Seam-hit avoidance for mesh-glass entry: an origin offset along
           the refracted direction alone leaves the ray glued to the entry
           seam, where a neighbor triangle's back face produces a spurious
           surface hit ~1e-4 in — for a volume this falsely "exits" the
           medium before the real chord is charged, and for plain glass it
           yields a spurious second refraction.  Push along the (flipped)
           normal instead — straight into the surface.  Applies to ALL mesh
           glass entry (universal; not gated on absorption), matching the
           universal corrected eta/n_refr. */
        V refr_origin = add(p, mul(refr_dir, EPS));
        if (hit_type == 2 && side)
            refr_origin = add(refr_origin, mul(n_adj, EPS));
        refr_col = trace_ray(refr_origin, refr_dir, depth + 1,
                             spheres, num_spheres, meshes, num_meshes,
                             lights, num_lights, emissive, num_emissive,
                             sample_idx, env, textures, num_textures,
                             refr_med);
        refr_col = (V){refr_col.x * sc.x, refr_col.y * sc.y, refr_col.z * sc.z};
        refr_col = (V){refr_col.x * Tseg.x, refr_col.y * Tseg.y, refr_col.z * Tseg.z};
    }

    float r0 = (1.0f - ior) / (1.0f + ior);
    r0 = r0 * r0;
    float fresnel = r0 + (1.0f - r0) * powf(1.0f - cos_i, 5.0f);

    /* Iridescence: the film response replaces the glass surface weight per
       channel; the transmitted term is the clamped complement so the blend
       stays energy-consistent.  Without a film these reduce to the scalar
       weights above exactly. */
    V wr = (V){fresnel * reflectivity, fresnel * reflectivity, fresnel * reflectivity};
    V wt = (V){1.0f - fresnel, 1.0f - fresnel, 1.0f - fresnel};
    if (film_w > 0.0f) {
        wr = add(mul(wr, 1.0f - film_w), mul(film, film_w));
        wt = (V){fmaxf(0.0f, 1.0f - wr.x), fmaxf(0.0f, 1.0f - wr.y), fmaxf(0.0f, 1.0f - wr.z)};
    }
    V glass = add((V){refl_col.x * wr.x, refl_col.y * wr.y, refl_col.z * wr.z},
                  (V){refr_col.x * wt.x, refr_col.y * wt.y, refr_col.z * wt.z});
    return add(base_color, glass);
}

typedef struct {
    V cam, fwd, right, up;
    float asp;
    float fov_scale;
    float aperture;
    float focus_dist;
    SphereData* spheres;
    int num_spheres;
    MeshObjData* meshes;
    int num_meshes;
    LightData* lights;
    int num_lights;
    EmissiveSurf* emissive;
    int num_emissive;
    float exposure;
    int width, height;
    EnvMap* env;
    Image* img;
    ImageTexture* textures;
    int num_textures;
} RenderContext;

static void render_rows(RenderContext* ctx, int y_start, int y_end) {
    for (int y = y_start; y < y_end; y++) {
        for (int x = 0; x < ctx->width; x++) {
            V color_sum = {0, 0, 0};
            int sample_count = 0;

            for (int sy = 0; sy < AA_SAMPLES; sy++) {
                for (int sx = 0; sx < AA_SAMPLES; sx++) {
                    int sample_idx = sy * AA_SAMPLES + sx;
                    float sample_x = (float)(sx + 0.5f) / AA_SAMPLES;
                    float sample_y = (float)(sy + 0.5f) / AA_SAMPLES;
                    float uv_x = (2.0f*(x + sample_x)/ctx->width - 1.0f) * ctx->asp * ctx->fov_scale;
                    float uv_y = (1.0f - 2.0f*(y + sample_y)/ctx->height) * ctx->fov_scale;
                    V ray_dir = norm(add(add(ctx->fwd, mul(ctx->right, uv_x)), mul(ctx->up, uv_y)));

                    V origin = ctx->cam;
                    if (ctx->aperture > 0) {
                        V focal = add(ctx->cam, mul(ray_dir, ctx->focus_dist));
                        float angle = 2.0f * (float)M_PI * (sx + 0.5f) / AA_SAMPLES;
                        float r = ctx->aperture * 0.5f * sqrtf((sy + 0.5f) / AA_SAMPLES);
                        V off = add(mul(ctx->right, r * cosf(angle)), mul(ctx->up, r * sinf(angle)));
                        origin = add(ctx->cam, off);
                        ray_dir = norm(sub(focal, origin));
                    }

                    V color = trace_ray(origin, ray_dir, 0, ctx->spheres, ctx->num_spheres,
                                        ctx->meshes, ctx->num_meshes,
                                        ctx->lights, ctx->num_lights,
                                        ctx->emissive, ctx->num_emissive,
                                        sample_idx, ctx->env,
                                        ctx->textures, ctx->num_textures,
                                        med_air());
                    color_sum = add(color_sum, color);
                    sample_count++;
                }
            }

            V color_avg = mul(color_sum, 1.0f/sample_count);
            color_avg = tone_map(color_avg, ctx->exposure);

            size_t idx = (y * ctx->width + x) * 3;
            ctx->img->data[idx]   = (uint8_t)(fmaxf(0.0f, fminf(color_avg.x, 1.0f)) * 255.0f);
            ctx->img->data[idx+1] = (uint8_t)(fmaxf(0.0f, fminf(color_avg.y, 1.0f)) * 255.0f);
            ctx->img->data[idx+2] = (uint8_t)(fmaxf(0.0f, fminf(color_avg.z, 1.0f)) * 255.0f);
        }
    }
}

static RenderContext setup_context(const Scene* scene) {
    RenderContext ctx;
    ctx.cam = (V){scene->camera_pos.x, scene->camera_pos.y, scene->camera_pos.z};
    V tgt = (V){scene->camera_target.x, scene->camera_target.y, scene->camera_target.z};
    ctx.aperture = scene->aperture;
    ctx.focus_dist = scene->focus_dist > 0 ? scene->focus_dist : 1;

    SphereData* spheres = (SphereData*)calloc(scene->num_spheres > 0 ? scene->num_spheres : 1, sizeof(SphereData));
    ctx.num_spheres = scene->num_spheres;
    for (int i = 0; i < scene->num_spheres; i++) {
        spheres[i].c = (V){scene->spheres[i].pos.x, scene->spheres[i].pos.y, scene->spheres[i].pos.z};
        spheres[i].r = scene->spheres[i].radius;
        spheres[i].ref = scene->spheres[i].reflectivity;
        spheres[i].ior = scene->spheres[i].ior;
        spheres[i].roughness = scene->spheres[i].roughness;
        spheres[i].col = (V){scene->spheres[i].color.x, scene->spheres[i].color.y, scene->spheres[i].color.z};
        const char* mat = scene->spheres[i].material[0] ? scene->spheres[i].material : "glass";
        spheres[i].mat_type = mat_name_to_type(mat);
        spheres[i].tex.type = scene->spheres[i].tex_type;
        spheres[i].tex.scale = scene->spheres[i].tex_scale;
        spheres[i].tex.color2 = (V){scene->spheres[i].tex_color2.x, scene->spheres[i].tex_color2.y, scene->spheres[i].tex_color2.z};
    }
    ctx.spheres = spheres;

    ctx.num_meshes = scene->num_meshes;
    MeshObjData* meshes = (MeshObjData*)calloc(ctx.num_meshes > 0 ? ctx.num_meshes : 1, sizeof(MeshObjData));
    if (ctx.num_meshes > 0) {
        for (int i = 0; i < ctx.num_meshes; i++) {
            meshes[i].tris = scene->meshes[i].tris;
            meshes[i].num_tris = scene->meshes[i].num_tris;
            meshes[i].col = (V){scene->meshes[i].color.x, scene->meshes[i].color.y, scene->meshes[i].color.z};
            meshes[i].ref = scene->meshes[i].reflectivity;
            meshes[i].ior = scene->meshes[i].ior;
            meshes[i].roughness = scene->meshes[i].roughness;
            meshes[i].metallic = scene->meshes[i].metallic;
            meshes[i].tex_index = scene->meshes[i].tex_index;
            meshes[i].orm_tex_index = scene->meshes[i].orm_tex_index;
            meshes[i].iri_tex_index = scene->meshes[i].iri_tex_index;
            meshes[i].iri_factor = scene->meshes[i].iri_factor;
            meshes[i].iri_ior = scene->meshes[i].iri_ior;
            meshes[i].iri_thin_min = scene->meshes[i].iri_thin_min;
            meshes[i].iri_thin_max = scene->meshes[i].iri_thin_max;
            meshes[i].vol_th = scene->meshes[i].vol_th;
            meshes[i].att_r = scene->meshes[i].att_r;
            meshes[i].att_g = scene->meshes[i].att_g;
            meshes[i].att_b = scene->meshes[i].att_b;
            meshes[i].att_dist = scene->meshes[i].att_dist;
            meshes[i].vol_tex_index = scene->meshes[i].vol_tex_index;
            const char* mat = scene->meshes[i].material[0] ? scene->meshes[i].material : "glass";
            meshes[i].mat_type = mat_name_to_type(mat);
            meshes[i].tex.type = scene->meshes[i].tex_type;
            meshes[i].tex.scale = scene->meshes[i].tex_scale;
            meshes[i].tex.color2 = (V){scene->meshes[i].tex_color2.x, scene->meshes[i].tex_color2.y, scene->meshes[i].tex_color2.z};
            if (scene->meshes[i].num_tris > 0) {
                int max_nodes = 2 * scene->meshes[i].num_tris;
                meshes[i].bvh_nodes = (BvhNode*)malloc(max_nodes * sizeof(BvhNode));
                meshes[i].num_bvh_nodes = bvh_build(meshes[i].bvh_nodes,
                    max_nodes, meshes[i].tris, meshes[i].num_tris);
                fprintf(stderr, "  mesh[%d] loaded_tris=%d bvh_nodes=%d\n",
                        i, scene->meshes[i].num_tris, meshes[i].num_bvh_nodes);
                if (meshes[i].num_bvh_nodes > 0) {
                    BvhNode* root = &meshes[i].bvh_nodes[0];
                    int leaf_tris = 0;
                    for (int ni = 0; ni < meshes[i].num_bvh_nodes; ni++) {
                        if (meshes[i].bvh_nodes[ni].left < 0)
                            leaf_tris += meshes[i].bvh_nodes[ni].tri_end - meshes[i].bvh_nodes[ni].tri_start;
                    }
                    fprintf(stderr, "    root_bbox=[%.6e %.6e %.6e] x [%.6e %.6e %.6e] leaf_tris=%d\n",
                            root->bbox_min[0], root->bbox_min[1], root->bbox_min[2],
                            root->bbox_max[0], root->bbox_max[1], root->bbox_max[2],
                            leaf_tris);
                }
            } else {
                meshes[i].bvh_nodes = NULL;
                meshes[i].num_bvh_nodes = 0;
            }
        }
    }
    ctx.meshes = meshes;
    {
        int total_loaded = 0, total_bvh = 0;
        for (int i = 0; i < ctx.num_meshes; i++) {
            total_loaded += scene->meshes[i].num_tris;
            total_bvh   += ctx.meshes[i].num_tris;
        }
        fprintf(stderr, "[debug] total loaded_tris=%d  total in_bvh_tris=%d\n",
                total_loaded, total_bvh);
    }

    LightData* lights = (LightData*)calloc(scene->num_lights > 0 ? scene->num_lights : 1, sizeof(LightData));
    ctx.num_lights = scene->num_lights;
    for (int i = 0; i < scene->num_lights; i++) {
        lights[i].pos = (V){scene->lights[i].pos.x, scene->lights[i].pos.y, scene->lights[i].pos.z};
        lights[i].size = scene->lights[i].size;
    }
    ctx.lights = lights;

    int em_count = 0;
    for (int i = 0; i < scene->num_spheres; i++)
        if (mat_name_to_type(scene->spheres[i].material[0] ? scene->spheres[i].material : "glass") == MAT_EMISSIVE)
            em_count++;
    for (int i = 0; i < scene->num_meshes; i++)
        if (mat_name_to_type(scene->meshes[i].material[0] ? scene->meshes[i].material : "glass") == MAT_EMISSIVE)
            em_count++;
    ctx.num_emissive = em_count;
    EmissiveSurf* emissive = (EmissiveSurf*)calloc(em_count > 0 ? em_count : 1, sizeof(EmissiveSurf));
    ctx.num_emissive = em_count;
    if (em_count > 0) {
        int ei = 0;
        for (int i = 0; i < scene->num_spheres; i++) {
            if (mat_name_to_type(scene->spheres[i].material[0] ? scene->spheres[i].material : "glass") != MAT_EMISSIVE) continue;
            emissive[ei].emitted = (V){scene->spheres[i].color.x, scene->spheres[i].color.y, scene->spheres[i].color.z};
            emissive[ei].type = 0;
            emissive[ei].src_idx = i;
            emissive[ei].c = (V){scene->spheres[i].pos.x, scene->spheres[i].pos.y, scene->spheres[i].pos.z};
            emissive[ei].r = scene->spheres[i].radius;
            emissive[ei].area = 4.0f * (float)M_PI * scene->spheres[i].radius * scene->spheres[i].radius;
            emissive[ei].tris = NULL;
            emissive[ei].num_tris = 0;
            emissive[ei].tri_cdf = NULL;
            emissive[ei].total_area = 0;
            emissive[ei].bvh_nodes = NULL;
            emissive[ei].num_bvh_nodes = 0;
            ei++;
        }
        for (int i = 0; i < scene->num_meshes; i++) {
            if (mat_name_to_type(scene->meshes[i].material[0] ? scene->meshes[i].material : "glass") != MAT_EMISSIVE) continue;
            emissive[ei].emitted = (V){scene->meshes[i].color.x, scene->meshes[i].color.y, scene->meshes[i].color.z};
            emissive[ei].type = 1;
            emissive[ei].src_idx = i;
            emissive[ei].c = (V){0,0,0};
            emissive[ei].r = 0;
            emissive[ei].tris = scene->meshes[i].tris;
            emissive[ei].num_tris = scene->meshes[i].num_tris;
            float total = 0;
            float* cdf = (float*)malloc((scene->meshes[i].num_tris + 1) * sizeof(float));
            cdf[0] = 0;
            for (int j = 0; j < scene->meshes[i].num_tris; j++) {
                float ta = tri_area(&scene->meshes[i].tris[j]);
                total += ta;
                cdf[j + 1] = total;
            }
            emissive[ei].tri_cdf = cdf;
            emissive[ei].total_area = total;
            emissive[ei].area = total;
            emissive[ei].bvh_nodes = NULL;
            emissive[ei].num_bvh_nodes = 0;
            ei++;
        }
    }
    ctx.emissive = emissive;

    ctx.fwd = norm(sub(tgt, ctx.cam));
    V world_up = (V){0, 1, 0};
    if (fabsf(dot(world_up, ctx.fwd)) > 0.999f)
        world_up = (V){0, 0, 1};
    ctx.right = norm(cross(world_up, ctx.fwd));
    ctx.up = cross(ctx.fwd, ctx.right);
    ctx.asp = (float)scene->width / scene->height;
    ctx.fov_scale = tanf(scene->fov_y * 0.5f * (float)M_PI / 180.0f);
    fprintf(stderr, "[renderer] fov_y=%.1f  fov_scale=%.6f  top_uv_y=%.6f  bottom_uv_y=%.6f\n",
            scene->fov_y, ctx.fov_scale,
            (1.0f - 2.0f * 0.0f / scene->height) * ctx.fov_scale,
            (1.0f - 2.0f * (scene->height - 1) / scene->height) * ctx.fov_scale);
    ctx.exposure = scene->exposure;
    ctx.width = scene->width;
    ctx.height = scene->height;
    ctx.env = envmap_load(scene->env_file, scene->env_intensity);
    ctx.textures = scene->textures;
    ctx.num_textures = scene->num_textures;
    ctx.img = create_image(scene->width, scene->height);
    return ctx;
}

static void free_render_buffers(RenderContext* ctx) {
    if (ctx->meshes) {
        for (int i = 0; i < ctx->num_meshes; i++)
            free(ctx->meshes[i].bvh_nodes);
        free(ctx->meshes);
    }
    if (ctx->emissive) {
        for (int i = 0; i < ctx->num_emissive; i++)
            free(ctx->emissive[i].tri_cdf);
        free(ctx->emissive);
    }
    free(ctx->spheres);
    free(ctx->lights);
}

static void apply_denoise(Image* img, const Scene* scene) {
    if (!scene->denoise) return;
    GBuffer* gbuf = trace_gbuffer(scene);
    denoise(img, gbuf, scene->width, scene->height, scene->denoise_strength);
    free_gbuffer(gbuf);
}

Image* render_frame(const Scene* scene) {
    RenderContext ctx = setup_context(scene);
    fprintf(stderr, "\n[debug] skip_mesh values (emissive self-avoid):\n");
    for (int ei = 0; ei < ctx.num_emissive; ei++) {
        if (ctx.emissive[ei].type == 1) {
            fprintf(stderr, "  emissive[%d] type=mesh src_idx=%d -> skip_mesh=%d\n",
                    ei, ctx.emissive[ei].src_idx, ctx.emissive[ei].src_idx);
        }
    }
    g_debug_frame_done = 0;
    memset(g_hit_tri_tests, 0, sizeof(g_hit_tri_tests));
    memset(g_hit_tri_hits, 0, sizeof(g_hit_tri_hits));
    render_rows(&ctx, 0, ctx.height);
    fprintf(stderr, "\n[debug] hit_tri counters (one full frame):\n");
    for (int i = 0; i < ctx.num_meshes; i++) {
        fprintf(stderr, "  mesh[%d] tests=%d hits=%d\n", i, g_hit_tri_tests[i], g_hit_tri_hits[i]);
    }
    g_debug_frame_done = 1;
    apply_denoise(ctx.img, scene);
    envmap_free(ctx.env);
    free_render_buffers(&ctx);
    return ctx.img;
}

Image* render_frame_parallel(const Scene* scene, int num_threads) {
    RenderContext ctx = setup_context(scene);
    if (num_threads < 1) num_threads = 1;
    if (num_threads > ctx.height) num_threads = ctx.height;

    fprintf(stderr, "\n[debug] skip_mesh values (emissive self-avoid):\n");
    for (int ei = 0; ei < ctx.num_emissive; ei++) {
        if (ctx.emissive[ei].type == 1) {
            fprintf(stderr, "  emissive[%d] type=mesh src_idx=%d -> skip_mesh=%d\n",
                    ei, ctx.emissive[ei].src_idx, ctx.emissive[ei].src_idx);
        }
    }
    g_debug_frame_done = 0;
    memset(g_hit_tri_tests, 0, sizeof(g_hit_tri_tests));
    memset(g_hit_tri_hits, 0, sizeof(g_hit_tri_hits));

    std::vector<std::thread> threads;
    int rows_per = ctx.height / num_threads;
    for (int t = 0; t < num_threads; t++) {
        int y0 = t * rows_per;
        int y1 = (t == num_threads - 1) ? ctx.height : y0 + rows_per;
        threads.emplace_back(render_rows, &ctx, y0, y1);
    }
    for (auto& th : threads) th.join();
    fprintf(stderr, "\n[debug] hit_tri counters (one full frame):\n");
    for (int i = 0; i < ctx.num_meshes; i++) {
        fprintf(stderr, "  mesh[%d] tests=%d hits=%d\n", i, g_hit_tri_tests[i], g_hit_tri_hits[i]);
    }
    g_debug_frame_done = 1;
    apply_denoise(ctx.img, scene);
    envmap_free(ctx.env);
    free_render_buffers(&ctx);
    return ctx.img;
}
