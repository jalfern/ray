#include "renderer.h"
#include "../vector/vector.h"
#include "../shading/shading.h"
#include "bvh.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <thread>
#include <vector>
#include <float.h>

#define EPS 1e-4f
#define AA_SAMPLES 4
#define MAX_DEPTH 4

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
                   float *u, float *v) {
    V e1 = (V){v1[0]-v0[0], v1[1]-v0[1], v1[2]-v0[2]};
    V e2 = (V){v2[0]-v0[0], v2[1]-v0[1], v2[2]-v0[2]};
    V pv = cross(d, e2);
    float det = dot(e1, pv);
    if (fabsf(det) < EPS) return 0;
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
                         TriGpu* tris, BvhNode* nodes, int /*num_nodes*/) {
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
                if (hit_tri(o, d, tris[i].v0, tris[i].v1, tris[i].v2, &ti, &u, &v) && ti < best_t) {
                    best_t = ti; hit = 1; best_tri = &tris[i]; best_u = u; best_v = v;
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
        *hit_normal = (V){nx, ny, nz};
        if (dot(*hit_normal, d) > 0)
            *hit_normal = mul(*hit_normal, -1);
        out_uv[0] = w * best_tri->t0[0] + best_u * best_tri->t1[0] + best_v * best_tri->t2[0];
        out_uv[1] = w * best_tri->t0[1] + best_u * best_tri->t1[1] + best_v * best_tri->t2[1];
    }
    return hit;
}

static int hit_mesh_bvh_any(V o, V d, float max_t,
                              TriGpu* tris, BvhNode* nodes, int /*num_nodes*/) {
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
                if (hit_tri(o, d, tris[i].v0, tris[i].v1, tris[i].v2, &t, &u, &v) &&
                    t < max_t && t > EPS)
                    return 1;
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
                             meshes[m].tris, meshes[m].bvh_nodes, meshes[m].num_bvh_nodes))
            return 0;
    }
    return 1;
}

static float in_shadow(V p, LightData light, SphereData* spheres, int num_spheres,
                       MeshObjData* meshes, int num_meshes, int sample_idx, int skip_sphere) {
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
        if (meshes[m].num_bvh_nodes > 0 &&
            hit_mesh_bvh_any(ray_o, ray_dir, light_dist,
                             meshes[m].tris, meshes[m].bvh_nodes, meshes[m].num_bvh_nodes))
            return 1;
    }
    return 0;
}

static V trace_ray(V o, V d, int depth, SphereData* spheres, int num_spheres,
                   MeshObjData* meshes, int num_meshes,
                   LightData* lights, int num_lights,
                   EmissiveSurf* emissive, int num_emissive, int sample_idx) {
    if (depth > MAX_DEPTH) return (V){0,0,0};

    float ts, tf;
    V sn;
    int si = -1;
    int hs = hit_any_sphere(o, d, &ts, &sn, &si, spheres, num_spheres);

    float tm = 1e9f;
    V mn = {0,0,0};
    float m_uv[2] = {0,0};
    int mi = -1;
    for (int i = 0; i < num_meshes; i++) {
        V hit_n;
        float tmi;
        float uv[2];
        if (meshes[i].num_bvh_nodes > 0 &&
            hit_mesh_bvh(o, d, &tmi, &hit_n, uv, meshes[i].tris, meshes[i].bvh_nodes, meshes[i].num_bvh_nodes) && tmi < tm) {
            tm = tmi; mi = i; mn = hit_n; m_uv[0] = uv[0]; m_uv[1] = uv[1];
        }
    }
    int hm = (mi >= 0);

    int hf = hit_floor(o, d, &tf);

    int hit_type = 0;
    float t_hit;
    V hit_n;
    float sphere_col[3] = {1,1,1};
    float sphere_ref = 0, sphere_ior = 1.5f;
    int sphere_mat = 0;

    if (hs && (!hf || ts < tf) && (!hm || ts < tm)) {
        hit_type = 1; t_hit = ts; hit_n = sn;
        sphere_col[0] = spheres[si].col.x;
        sphere_col[1] = spheres[si].col.y;
        sphere_col[2] = spheres[si].col.z;
        sphere_ref = spheres[si].ref;
        sphere_ior = spheres[si].ior;
        sphere_mat = spheres[si].mat_type;
    } else if (hm && (!hf || tm < tf)) {
        hit_type = 2; t_hit = tm; hit_n = mn;
        sphere_col[0] = meshes[mi].col.x;
        sphere_col[1] = meshes[mi].col.y;
        sphere_col[2] = meshes[mi].col.z;
        sphere_ref = meshes[mi].ref;
        sphere_ior = meshes[mi].ior;
        sphere_mat = meshes[mi].mat_type;
    } else if (hf) {
        hit_type = 3; t_hit = tf;
    }

    if (hit_type == 0) return (V){0.1f, 0.1f, 0.2f};

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
                                 meshes, num_meshes, sidx, -1);
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
                pdf = 1.0f / emissive[ei].area;
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
            float G = cos_surf * cos_light / (ldist * ldist);
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
        sc = (m_uv[0] != 0 || m_uv[1] != 0) ? eval_texture_uv(uv, sc, &meshes[mi].tex)
                                             : eval_texture(p, sc, &meshes[mi].tex);
    }

    if (mat == MAT_EMISSIVE) return sc;

    V lit = {0, 0, 0};
    for (int li = 0; li < num_lights; li++) {
        V light_pos = lights[li].pos;
        V light_dir = norm(sub(light_pos, p));
        int sidx = (sample_idx << 2) | li;
        float sf = in_shadow(p, lights[li], spheres, num_spheres,
                             meshes, num_meshes, sidx,
                             hit_type == 1 ? si : -1);

        float diff = fmaxf(0.0f, dot(n, light_dir));
        V view = norm(sub(o, p));
        V half = norm(add(light_dir, view));

        float spec_exp = (mat == MAT_PLASTIC || mat == MAT_SUBSURFACE) ? 32.0f : 64.0f;
        float spec_str = (mat == MAT_PLASTIC || mat == MAT_SUBSURFACE) ? 0.4f : 0.8f;
        float spec = powf(fmaxf(0.0f, dot(n, half)), spec_exp);
        float lf = sf ? 0.0f : 1.0f;

        if (mat == MAT_SUBSURFACE) {
            float bdiff = fmaxf(0.0f, dot(mul(n, -1), light_dir));
            lit = add(lit, add(mul(sc, diff * lf * 0.7f),
                              add(mul(sc, bdiff * lf * 0.3f),
                                  mul(sc, spec * spec_str * lf))));
        } else {
            lit = add(lit, add(mul(sc, diff * lf), mul(sc, spec * spec_str * lf)));
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
            pdf = 1.0f / emissive[ei].area;
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
        float G = cos_surf * cos_light / (ldist * ldist);
        int vis = emissive_visible(p, lp, ldist, spheres, num_spheres,
                                    meshes, num_meshes, skip_sph, skip_mesh);
        if (vis) {
            V le = emissive[ei].emitted;
            V em_contrib = mul(le, G / pdf);
            lit = add(lit, (V){sc.x * em_contrib.x, sc.y * em_contrib.y, sc.z * em_contrib.z});
        }
    }

    V ambient = mul(sc, 0.15f);
    V base_color = add(ambient, lit);

    if (mat == MAT_PLASTIC) return base_color;
    if (mat == MAT_SUBSURFACE) return base_color;

    float cos_i = dot(n, d);
    int entering = cos_i < 0;
    V n_adj = entering ? n : mul(n, -1);
    cos_i = entering ? -cos_i : cos_i;

    V refl_dir = sub(d, mul(n_adj, 2.0f * dot(d, n_adj)));
    V refl_origin = add(p, mul(refl_dir, EPS));
    V refl_col = trace_ray(refl_origin, refl_dir, depth + 1,
                           spheres, num_spheres, meshes, num_meshes,
                           lights, num_lights, emissive, num_emissive, sample_idx);

    if (mat == MAT_METALLIC) return (V){refl_col.x * sc.x, refl_col.y * sc.y, refl_col.z * sc.z};

    float reflectivity = sphere_ref;
    float ior = sphere_ior;

    float n1 = entering ? 1.0f : ior;
    float n2 = entering ? ior : 1.0f;
    float eta = n1 / n2;
    float k = 1.0f - eta * eta * (1.0f - cos_i * cos_i);

    V refr_col = {0, 0, 0};
    if (k > 0) {
        float cos_t = sqrtf(k);
        V refr_dir = add(mul(d, eta), mul(n_adj, eta * cos_i - cos_t));
        V refr_origin = add(p, mul(refr_dir, EPS));
        refr_col = trace_ray(refr_origin, refr_dir, depth + 1,
                             spheres, num_spheres, meshes, num_meshes,
                             lights, num_lights, emissive, num_emissive, sample_idx);
        refr_col = (V){refr_col.x * sc.x, refr_col.y * sc.y, refr_col.z * sc.z};
    }

    float r0 = (1.0f - ior) / (1.0f + ior);
    r0 = r0 * r0;
    float fresnel = r0 + (1.0f - r0) * powf(1.0f - cos_i, 5.0f);

    return add(mul(refl_col, fresnel * reflectivity), mul(refr_col, 1.0f - fresnel));
}

typedef struct {
    V cam, fwd, right, up;
    float asp;
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
    Image* img;
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
                    float uv_x = (2.0f*(x + sample_x)/ctx->width - 1.0f) * ctx->asp;
                    float uv_y = 1.0f - 2.0f*(y + sample_y)/ctx->height;
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
                                        ctx->emissive, ctx->num_emissive, sample_idx);
                    color_sum = add(color_sum, color);
                    sample_count++;
                }
            }

            V color_avg = mul(color_sum, 1.0f/sample_count);
            color_avg = tone_map(color_avg, ctx->exposure);

            size_t idx = (y * ctx->width + x) * 3;
            ctx->img->data[idx]   = (uint8_t)(fminf(color_avg.x, 1.0f) * 255.0f);
            ctx->img->data[idx+1] = (uint8_t)(fminf(color_avg.y, 1.0f) * 255.0f);
            ctx->img->data[idx+2] = (uint8_t)(fminf(color_avg.z, 1.0f) * 255.0f);
        }
    }
}

static RenderContext setup_context(const Scene* scene) {
    RenderContext ctx;
    ctx.cam = (V){scene->camera_pos.x, scene->camera_pos.y, scene->camera_pos.z};
    V tgt = (V){scene->camera_target.x, scene->camera_target.y, scene->camera_target.z};
    ctx.aperture = scene->aperture;
    ctx.focus_dist = scene->focus_dist > 0 ? scene->focus_dist : 1;

    ctx.spheres = (SphereData*)malloc(scene->num_spheres * sizeof(SphereData));
    ctx.num_spheres = scene->num_spheres;
    for (int i = 0; i < scene->num_spheres; i++) {
        ctx.spheres[i].c = (V){scene->spheres[i].pos.x, scene->spheres[i].pos.y, scene->spheres[i].pos.z};
        ctx.spheres[i].r = scene->spheres[i].radius;
        ctx.spheres[i].ref = scene->spheres[i].reflectivity;
        ctx.spheres[i].ior = scene->spheres[i].ior;
        ctx.spheres[i].col = (V){scene->spheres[i].color.x, scene->spheres[i].color.y, scene->spheres[i].color.z};
        const char* mat = scene->spheres[i].material[0] ? scene->spheres[i].material : "glass";
        ctx.spheres[i].mat_type = mat_name_to_type(mat);
        ctx.spheres[i].tex.type = scene->spheres[i].tex_type;
        ctx.spheres[i].tex.scale = scene->spheres[i].tex_scale;
        ctx.spheres[i].tex.color2 = (V){scene->spheres[i].tex_color2.x, scene->spheres[i].tex_color2.y, scene->spheres[i].tex_color2.z};
    }

    ctx.meshes = NULL;
    ctx.num_meshes = scene->num_meshes;
    if (ctx.num_meshes > 0) {
        ctx.meshes = (MeshObjData*)malloc(ctx.num_meshes * sizeof(MeshObjData));
        for (int i = 0; i < ctx.num_meshes; i++) {
            ctx.meshes[i].tris = scene->meshes[i].tris;
            ctx.meshes[i].num_tris = scene->meshes[i].num_tris;
            ctx.meshes[i].col = (V){scene->meshes[i].color.x, scene->meshes[i].color.y, scene->meshes[i].color.z};
            ctx.meshes[i].ref = scene->meshes[i].reflectivity;
            ctx.meshes[i].ior = scene->meshes[i].ior;
            const char* mat = scene->meshes[i].material[0] ? scene->meshes[i].material : "glass";
            ctx.meshes[i].mat_type = mat_name_to_type(mat);
            ctx.meshes[i].tex.type = scene->meshes[i].tex_type;
            ctx.meshes[i].tex.scale = scene->meshes[i].tex_scale;
            ctx.meshes[i].tex.color2 = (V){scene->meshes[i].tex_color2.x, scene->meshes[i].tex_color2.y, scene->meshes[i].tex_color2.z};
            if (scene->meshes[i].num_tris > 0) {
                int max_nodes = 2 * scene->meshes[i].num_tris;
                if (max_nodes > BVH_MAX_NODES) max_nodes = BVH_MAX_NODES;
                ctx.meshes[i].bvh_nodes = (BvhNode*)malloc(max_nodes * sizeof(BvhNode));
                ctx.meshes[i].num_bvh_nodes = bvh_build(ctx.meshes[i].bvh_nodes,
                    ctx.meshes[i].tris, ctx.meshes[i].num_tris);
            } else {
                ctx.meshes[i].bvh_nodes = NULL;
                ctx.meshes[i].num_bvh_nodes = 0;
            }
        }
    }

    ctx.lights = (LightData*)malloc(scene->num_lights * sizeof(LightData));
    ctx.num_lights = scene->num_lights;
    for (int i = 0; i < scene->num_lights; i++) {
        ctx.lights[i].pos = (V){scene->lights[i].pos.x, scene->lights[i].pos.y, scene->lights[i].pos.z};
        ctx.lights[i].size = scene->lights[i].size;
    }

    int em_count = 0;
    for (int i = 0; i < scene->num_spheres; i++)
        if (mat_name_to_type(scene->spheres[i].material[0] ? scene->spheres[i].material : "glass") == MAT_EMISSIVE)
            em_count++;
    for (int i = 0; i < scene->num_meshes; i++)
        if (mat_name_to_type(scene->meshes[i].material[0] ? scene->meshes[i].material : "glass") == MAT_EMISSIVE)
            em_count++;
    ctx.num_emissive = em_count;
    ctx.emissive = NULL;
    if (em_count > 0) {
        ctx.emissive = (EmissiveSurf*)malloc(em_count * sizeof(EmissiveSurf));
        int ei = 0;
        for (int i = 0; i < scene->num_spheres; i++) {
            if (mat_name_to_type(scene->spheres[i].material[0] ? scene->spheres[i].material : "glass") != MAT_EMISSIVE) continue;
            ctx.emissive[ei].emitted = (V){scene->spheres[i].color.x, scene->spheres[i].color.y, scene->spheres[i].color.z};
            ctx.emissive[ei].type = 0;
            ctx.emissive[ei].src_idx = i;
            ctx.emissive[ei].c = (V){scene->spheres[i].pos.x, scene->spheres[i].pos.y, scene->spheres[i].pos.z};
            ctx.emissive[ei].r = scene->spheres[i].radius;
            ctx.emissive[ei].area = 4.0f * (float)M_PI * scene->spheres[i].radius * scene->spheres[i].radius;
            ctx.emissive[ei].tris = NULL;
            ctx.emissive[ei].num_tris = 0;
            ctx.emissive[ei].tri_cdf = NULL;
            ctx.emissive[ei].total_area = 0;
            ctx.emissive[ei].bvh_nodes = NULL;
            ctx.emissive[ei].num_bvh_nodes = 0;
            ei++;
        }
        for (int i = 0; i < scene->num_meshes; i++) {
            if (mat_name_to_type(scene->meshes[i].material[0] ? scene->meshes[i].material : "glass") != MAT_EMISSIVE) continue;
            ctx.emissive[ei].emitted = (V){scene->meshes[i].color.x, scene->meshes[i].color.y, scene->meshes[i].color.z};
            ctx.emissive[ei].type = 1;
            ctx.emissive[ei].src_idx = i;
            ctx.emissive[ei].c = (V){0,0,0};
            ctx.emissive[ei].r = 0;
            ctx.emissive[ei].tris = scene->meshes[i].tris;
            ctx.emissive[ei].num_tris = scene->meshes[i].num_tris;
            float total = 0;
            float* cdf = (float*)malloc((scene->meshes[i].num_tris + 1) * sizeof(float));
            cdf[0] = 0;
            for (int j = 0; j < scene->meshes[i].num_tris; j++) {
                float ta = tri_area(&scene->meshes[i].tris[j]);
                total += ta;
                cdf[j + 1] = total;
            }
            ctx.emissive[ei].tri_cdf = cdf;
            ctx.emissive[ei].total_area = total;
            ctx.emissive[ei].area = total;
            ctx.emissive[ei].bvh_nodes = NULL;
            ctx.emissive[ei].num_bvh_nodes = 0;
            ei++;
        }
    }

    ctx.fwd = norm(sub(tgt, ctx.cam));
    ctx.right = norm(cross((V){0,1,0}, ctx.fwd));
    ctx.up = cross(ctx.fwd, ctx.right);
    ctx.asp = (float)scene->width / scene->height;
    ctx.exposure = scene->exposure;
    ctx.width = scene->width;
    ctx.height = scene->height;
    ctx.img = create_image(scene->width, scene->height);
    return ctx;
}

static void free_mesh_data(MeshObjData* meshes, int num_meshes) {
    if (meshes) {
        for (int i = 0; i < num_meshes; i++)
            free(meshes[i].bvh_nodes);
        free(meshes);
    }
}

Image* render_frame(const Scene* scene) {
    RenderContext ctx = setup_context(scene);
    render_rows(&ctx, 0, ctx.height);
    free(ctx.spheres);
    free(ctx.lights);
    for (int i = 0; i < ctx.num_emissive; i++)
        free(ctx.emissive[i].tri_cdf);
    free(ctx.emissive);
    free_mesh_data(ctx.meshes, ctx.num_meshes);
    return ctx.img;
}

Image* render_frame_parallel(const Scene* scene, int num_threads) {
    RenderContext ctx = setup_context(scene);
    if (num_threads < 1) num_threads = 1;
    if (num_threads > ctx.height) num_threads = ctx.height;

    std::vector<std::thread> threads;
    int rows_per = ctx.height / num_threads;
    for (int t = 0; t < num_threads; t++) {
        int y0 = t * rows_per;
        int y1 = (t == num_threads - 1) ? ctx.height : y0 + rows_per;
        threads.emplace_back(render_rows, &ctx, y0, y1);
    }
    for (auto& th : threads) th.join();

    free(ctx.spheres);
    free(ctx.lights);
    for (int i = 0; i < ctx.num_emissive; i++)
        free(ctx.emissive[i].tri_cdf);
    free(ctx.emissive);
    free_mesh_data(ctx.meshes, ctx.num_meshes);
    return ctx.img;
}
