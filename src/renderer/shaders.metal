#include <metal_stdlib>
using namespace metal;

constant float EPS = 1e-4f;
constant int AA_SAMPLES = 4;
constant int MAX_DEPTH = 4;

struct SphereGpu {
    packed_float3 c;
    float r;
    float ref;
    float ior;
    packed_float3 col;
    int mat_type;
};

struct CameraGpu {
    packed_float3 pos;
    packed_float3 target;
};

struct SceneGpu {
    packed_float3 light_pos;
    int num_spheres;
    int num_mesh_tris;
    int width;
    int height;
};

struct TriGpu {
    packed_float3 v0, v1, v2;
    packed_float3 n0, n1, n2;
    int mesh_idx;
    float pad0, pad1, pad2;
};

static bool hit_sphere(float3 o, float3 d, float3 c, float r, thread float& t) {
    float3 oc = o - c;
    float a = dot(d, d);
    float b = 2.0f * dot(oc, d);
    float cc = dot(oc, oc) - r*r;
    float delta = b*b - 4.0f*a*cc;
    if (delta < 0.0f) return false;
    float sd = sqrt(delta);
    float t1 = (-b - sd) / (2.0f * a);
    float t2 = (-b + sd) / (2.0f * a);
    t = (t1 > EPS) ? t1 : t2;
    return t > EPS;
}

static bool hit_any_sphere(float3 o, float3 d, thread float& t, thread float3& n,
                           thread int& idx, device const SphereGpu* spheres, int count) {
    float best = 1e9f;
    bool hit = false;
    idx = -1;
    for (int i = 0; i < count; i++) {
        float ti;
        if (hit_sphere(o, d, spheres[i].c, spheres[i].r, ti) && ti < best) {
            best = ti; hit = true; idx = i;
        }
    }
    if (hit) { t = best; n = normalize(o + d * best - spheres[idx].c); }
    return hit;
}

static bool hit_tri(float3 o, float3 d, float3 v0, float3 v1, float3 v2,
                    thread float& t, thread float& u, thread float& v) {
    float3 e1 = v1 - v0, e2 = v2 - v0;
    float3 pv = cross(d, e2);
    float det = dot(e1, pv);
    if (fabs(det) < EPS) return false;
    float inv = 1.0f / det;
    float3 tv = o - v0;
    u = dot(tv, pv) * inv;
    if (u < 0.0f || u > 1.0f) return false;
    float3 qv = cross(tv, e1);
    v = dot(d, qv) * inv;
    if (v < 0.0f || u + v > 1.0f) return false;
    t = dot(e2, qv) * inv;
    return t > EPS;
}

static float3 tri_normal(float3 v0, float3 v1, float3 v2,
                         float3 n0, float3 n1, float3 n2, float u, float v) {
    float w = 1.0f - u - v;
    float3 n = w * n0 + u * n1 + v * n2;
    float len = length(n);
    return len > EPS ? n / len : float3(0.0f, 1.0f, 0.0f);
}

static bool hit_floor(float3 o, float3 d, thread float& t) {
    if (fabs(d.y) < EPS) return false;
    t = -o.y / d.y;
    return t > EPS;
}

static float3 floor_color(float3 p) {
    return ((int(p.x)+int(p.z)) & 1) ? float3(0.08f,0.12f,0.25f) : float3(0.25f,0.4f,0.7f);
}

static bool in_shadow(float3 p, float3 lp, device const SphereGpu* spheres, int sc,
                      device const TriGpu* tris, int tc, int origin) {
    float3 tl = lp - p;
    float ld = length(tl);
    float3 rd = normalize(tl);
    float3 ro = p + rd * EPS;
    for (int i = 0; i < sc; i++) {
        if (i == origin) continue;
        float t;
        if (hit_sphere(ro, rd, spheres[i].c, spheres[i].r, t) && t < ld && t > EPS)
            return true;
    }
    for (int i = 0; i < tc; i++) {
        float t, u, v;
        if (hit_tri(ro, rd, tris[i].v0, tris[i].v1, tris[i].v2, t, u, v) && t < ld && t > EPS)
            return true;
    }
    return false;
}

// Per-mesh material data stored in a separate buffer
struct MeshMat {
    packed_float3 col;
    float ref;
    float ior;
    int mat_type;
};

static float3 trace_ray(float3 o, float3 d, device const SphereGpu* spheres, int sc,
                        device const TriGpu* tris, int tc, device const MeshMat* mats, int nm,
                        float3 lp, int inside_idx) {
    float3 accum = float3(0.0f);
    float3 thru = float3(1.0f);
    float3 ro = o, rd = d;

    for (int depth = 0; depth <= MAX_DEPTH; depth++) {
        float ts, tf;
        float3 sn;
        int si = -1;
        bool hs = hit_any_sphere(ro, rd, ts, sn, si, spheres, sc);

        // Mesh intersection
        float tm = 1e9f;
        float3 mn = float3(0);
        int mi = -1;
        float mu = 0, mv = 0;
        for (int i = 0; i < tc; i++) {
            float ti, u, v;
            if (hit_tri(ro, rd, tris[i].v0, tris[i].v1, tris[i].v2, ti, u, v) && ti < tm) {
                tm = ti; mi = i; mu = u; mv = v;
            }
        }
        bool hm = mi >= 0;

        bool hf = hit_floor(ro, rd, tf);

        int hit_type = 0;
        float t_hit;
        float3 hit_n;
        float3 sc_col = float3(1.0f);
        float sref = 0, sior = 1.5f;
        int smat = 0;

        if (hs && (!hf || ts < tf) && (!hm || ts < tm)) {
            hit_type = 1; t_hit = ts; hit_n = sn;
            sc_col = spheres[si].col;
            sref = spheres[si].ref;
            sior = spheres[si].ior;
            smat = spheres[si].mat_type;
        } else if (hm && (!hf || tm < tf)) {
            hit_type = 2; t_hit = tm;
            int mesh_idx = tris[mi].mesh_idx;
            if (mesh_idx >= 0 && mesh_idx < nm) {
                sc_col = mats[mesh_idx].col;
                sref = mats[mesh_idx].ref;
                sior = mats[mesh_idx].ior;
                smat = mats[mesh_idx].mat_type;
            }
            hit_n = tri_normal(tris[mi].v0, tris[mi].v1, tris[mi].v2,
                               tris[mi].n0, tris[mi].n1, tris[mi].n2, mu, mv);
            if (dot(hit_n, rd) > 0) hit_n = -hit_n;
        } else if (hf) {
            hit_type = 3; t_hit = tf;
        }

        if (hit_type == 0) { accum += float3(0.1f, 0.1f, 0.2f) * thru; break; }

        float3 p = ro + rd * t_hit;

        if (hit_type == 3) {
            float3 nf = float3(0, 1, 0);
            float3 tl = lp - p;
            float3 ld = normalize(tl);
            bool sh = in_shadow(p, lp, spheres, sc, tris, tc, -1);
            float3 fl = floor_color(p);
            float diff = max(0.0f, dot(nf, ld));
            float lf = sh ? 0.2f : 1.0f;
            accum += (fl * 0.15f + fl * diff * lf) * thru;
            break;
        }

        float3 n_hit = hit_n;
        int is_plastic = (smat == 1);

        float3 tl = lp - p;
        float3 ld = normalize(tl);
        bool sh = in_shadow(p, lp, spheres, sc, tris, tc,
                            hit_type == 1 ? si : -1);

        float diff = max(0.0f, dot(n_hit, ld));
        float3 vw = normalize(ro - p);
        float3 hf = normalize(ld + vw);
        float sp = pow(max(0.0f, dot(n_hit, hf)), is_plastic ? 32.0f : 64.0f);

        float3 amb = sc_col * 0.15f;
        float ss = is_plastic ? 0.4f : 0.8f;
        float lf = sh ? 0.0f : 1.0f;
        float3 base = amb + sc_col * diff * lf + sc_col * sp * ss * lf;

        accum += base * thru;

        if (is_plastic || depth == MAX_DEPTH) break;

        // Glass: reflection + refraction
        float cos_i = dot(n_hit, rd);
        bool entering = cos_i < 0;
        float3 na = entering ? n_hit : -n_hit;
        cos_i = entering ? -cos_i : cos_i;

        // Reflect
        float3 refl_d = reflect(rd, na);
        float3 refl_o = p + refl_d * EPS;

        // We don't follow the reflected ray in the iterative path for simplicity.
        // Instead, we estimate the Fresnel term and combine with transmission.

        // For a real implementation, we'd need to handle two ray paths.
        // Let's use a simplified approach: continue with one path weighted by Fresnel.
        float n1 = entering ? 1.0f : sior;
        float n2 = entering ? sior : 1.0f;
        float eta = n1 / n2;
        float k = 1.0f - eta * eta * (1.0f - cos_i * cos_i);

        float r0 = (1.0f - sior) / (1.0f + sior);
        r0 = r0 * r0;
        float fresnel = r0 + (1.0f - r0) * pow(1.0f - cos_i, 5.0f);

        if (depth == 0) {
            // First bounce: store reflection contribution
            // Accumulate reflected light (will trace on next iteration via recursion simulation)
            // For now, refract and continue
        }

        if (k > 0) {
            float cos_t = sqrt(k);
            float3 refr_d = rd * eta + na * (eta * cos_i - cos_t);
            ro = p + refr_d * EPS;
            rd = refr_d;
            thru *= (1.0f - fresnel);
            thru *= sc_col; // Beer's law tint
        } else {
            // Total internal reflection
            ro = p + refl_d * EPS;
            rd = refl_d;
            thru *= fresnel * sref;
        }
    }
    return accum;
}

kernel void raytrace_kernel(
    device float3* out [[buffer(0)]],
    constant CameraGpu& cam [[buffer(1)]],
    constant SceneGpu& scene [[buffer(2)]],
    device const SphereGpu* spheres [[buffer(3)]],
    device const TriGpu* tris [[buffer(4)]],
    device const MeshMat* mats [[buffer(5)]],
    uint2 tid [[thread_position_in_grid]],
    uint2 grid [[threads_per_grid]]
) {
    int x = tid.x, y = tid.y;
    if (x >= scene.width || y >= scene.height) return;

    float3 fwd = normalize(cam.target - cam.pos);
    float3 right = normalize(cross(float3(0.0f, 1.0f, 0.0f), fwd));
    float3 up = cross(fwd, right);
    float asp = (float)scene.width / (float)scene.height;

    float3 sum = float3(0.0f);
    for (int sy = 0; sy < AA_SAMPLES; sy++) {
        for (int sx = 0; sx < AA_SAMPLES; sx++) {
            float ux = (2.0f * (x + (sx + 0.5f) / AA_SAMPLES) / scene.width - 1.0f) * asp;
            float uy = 1.0f - 2.0f * (y + (sy + 0.5f) / AA_SAMPLES) / scene.height;
            float3 rd = normalize(fwd + right * ux + up * uy);
            sum += trace_ray(cam.pos, rd, spheres, scene.num_spheres,
                             tris, scene.num_mesh_tris, mats, scene.num_mesh_tris > 0 ? 1 : 0,
                             scene.light_pos, -1);
        }
    }
    out[y * scene.width + x] = sum / (float)(AA_SAMPLES * AA_SAMPLES);
}
