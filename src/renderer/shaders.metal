#include <metal_stdlib>
using namespace metal;

constant float EPS = 1e-4f;
constant int AA_SAMPLES = 4;
constant int MAX_DEPTH = 4;

constant int MAT_GLASS = 0;
constant int MAT_PLASTIC = 1;
constant int MAT_EMISSIVE = 2;
constant int MAT_METALLIC = 3;
constant int MAT_SUBSURFACE = 4;

constant int TEX_NONE = 0;
constant int TEX_CHECKER = 1;
constant int TEX_POLKA = 2;
constant int TEX_MARBLE = 3;
constant int TEX_RINGS = 4;

struct SphereGpu {
    packed_float3 c;
    float r;
    float ref;
    float ior;
    packed_float3 col;
    int mat_type;
    int tex_type;
    float tex_scale;
    packed_float3 tex_color2;
};

struct CameraGpu {
    packed_float3 pos;
    packed_float3 target;
    float aperture;
    float focus_dist;
};

struct LightGpu {
    packed_float3 pos;
    float size;
};

struct SceneGpu {
    int num_spheres;
    int num_mesh_tris;
    int num_bvh_nodes;
    int num_meshes;
    int num_lights;
    int num_emissive;
    int num_emissive_cdf;
    float exposure;
    int width;
    int height;
    int has_env;
};

struct EmissiveGpu {
    packed_float3 emitted;
    int type;
    packed_float3 c;
    float r;
    float area;
    int tri_start;
    int tri_end;
    int cdf_offset;
    int src_idx;
};

struct TriGpu {
    packed_float3 v0, v1, v2;
    packed_float3 n0, n1, n2;
    packed_float2 t0, t1, t2;
    int mesh_idx;
};

struct BvhNode {
    packed_float3 bbox_min;
    int tri_start;
    packed_float3 bbox_max;
    int tri_end;
    int left;
    int right;
    int _pad;
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

static bool bbox_hit(float3 o, float3 d, float3 bmin, float3 bmax) {
    float tmin = 0.0f, tmax = 1e9f;
    for (int a = 0; a < 3; a++) {
        float inv = 1.0f / d[a];
        float t0 = (bmin[a] - o[a]) * inv;
        float t1 = (bmax[a] - o[a]) * inv;
        if (inv < 0.0f) { float tmp = t0; t0 = t1; t1 = tmp; }
        tmin = max(tmin, t0);
        tmax = min(tmax, t1);
        if (tmax < tmin) return false;
    }
    return true;
}

static float3 floor_color(float3 p) {
    return ((int(p.x)+int(p.z)) & 1) ? float3(0.08f,0.12f,0.25f) : float3(0.25f,0.4f,0.7f);
}

static float3 sample_envmap(texture2d<float> env_tex, float3 d) {
    float u = atan2(d.z, d.x) * (0.5f / M_PI_F) + 0.5f;
    float v = acos(clamp(d.y, -1.0f, 1.0f)) * (1.0f / M_PI_F);
    return env_tex.sample(sampler(filter::linear, address::repeat), float2(u, v)).rgb;
}

static float3 env_procedural(float3 d) {
    float t = d.y * 0.5f + 0.5f;
    float horizon = 0.5f + 0.5f * d.y;
    float sky_r = 0.3f + 0.5f * horizon;
    float sky_g = 0.4f + 0.6f * horizon;
    float sky_b = 0.6f + 0.4f * horizon;
    float sun = pow(max(d.y, 0.0f), 64.0f) * 4.0f;
    float cloud = pow(max(0.2f + 0.8f * sin(d.x * 12.0f + d.z * 8.0f) * sin(d.z * 10.0f - d.x * 6.0f), 0.0f), 2.0f) * 0.3f;
    float3 col = float3(sky_r + sun + cloud, sky_g + sun * 0.8f + cloud, sky_b + sun * 0.4f + cloud);
    col *= 0.3f + 0.7f * max(d.y, 0.0f);
    return col;
}

static float3 tone_map(float3 c, float exposure) {
    float3 s = c * exposure;
    return s / (1.0f + s);
}

static float3 area_light_sample(float3 lp, float size, int sample_idx) {
    if (size <= 0.0f) return lp;
    int sx = sample_idx & 3;
    int sy = (sample_idx >> 2) & 3;
    float angle = 2.0f * M_PI_F * (sx + 0.5f) / 4.0f;
    float r = size * sqrt((sy + 0.5f) / 4.0f);
    return lp + float3(r * cos(angle), 0.0f, r * sin(angle));
}

static bool in_shadow(float3 p, LightGpu light,
                      device const SphereGpu* spheres, int sc,
                      device const TriGpu* tris, int tc,
                      device const BvhNode* bvh, int nb,
                      int sample_idx, int origin) {
    float3 lp = area_light_sample(light.pos, light.size, sample_idx);
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
    if (nb > 0) {
        int stk[64];
        int sp = 0;
        stk[sp++] = 0;
        while (sp > 0) {
            int ni = stk[--sp];
            BvhNode node = bvh[ni];
            if (!bbox_hit(ro, rd, node.bbox_min, node.bbox_max)) continue;
            if (node.left >= 0) {
                if (sp + 2 > 64) continue;
                stk[sp++] = node.left;
                stk[sp++] = node.right;
            } else {
                for (int i = node.tri_start; i < node.tri_end; i++) {
                    float t, u, v;
                    if (hit_tri(ro, rd, tris[i].v0, tris[i].v1, tris[i].v2, t, u, v) &&
                        t < ld && t > EPS)
                        return true;
                }
            }
        }
    }
    return false;
}

static float3 sample_emissive_sphere_gpu(float3 c, float r, thread float3& normal, int sample_idx, int ei) {
    int sx = (sample_idx + ei * 7) & 3;
    int sy = ((sample_idx >> 2) + ei * 3) & 3;
    float theta = 2.0f * M_PI_F * (sx + 0.5f) / 4.0f;
    float phi = acos(1.0f - 2.0f * (sy + 0.5f) / 4.0f);
    float3 dir = float3(sin(phi) * cos(theta), cos(phi), sin(phi) * sin(theta));
    normal = dir;
    return c + dir * r;
}

static float3 sample_emissive_mesh_gpu(device const TriGpu* tris, device const float* cdf,
                                         int tri_start, int tri_end, int cdf_offset,
                                         float total_area,
                                         thread float3& normal, thread float& dist,
                                         int sample_idx, int ei, float3 from) {
    int num_tris = tri_end - tri_start;
    if (num_tris <= 0) return from;
    int rnd = (sample_idx * 257 + ei * 101 + 53) & 0xFFFF;
    float r = (float)rnd / 65536.0f * total_area;
    int lo = 0, hi = num_tris;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (cdf[cdf_offset + mid + 1] <= r) lo = mid + 1;
        else hi = mid;
    }
    int ti = tri_start + min(lo, num_tris - 1);

    int sx = (sample_idx + ei * 11) & 3;
    int sy = ((sample_idx >> 2) + ei * 5) & 3;
    float su = (sx + 0.5f) / 4.0f;
    float sv = (sy + 0.5f) / 4.0f;
    if (su + sv > 1.0f) { su = 1.0f - su; sv = 1.0f - sv; }

    float3 v0 = tris[ti].v0, v1 = tris[ti].v1, v2 = tris[ti].v2;
    float3 p = v0 + su * (v1 - v0) + sv * (v2 - v0);
    float w = 1.0f - su - sv;
    float3 n = normalize(w * tris[ti].n0 + su * tris[ti].n1 + sv * tris[ti].n2);
    normal = n;
    dist = length(p - from);
    return p;
}

static bool emissive_visible_gpu(float3 p, float3 light_pos, float light_dist,
                                  device const SphereGpu* spheres, int sc, int skip_sphere,
                                  device const TriGpu* tris, int tc,
                                  device const BvhNode* bvh, int nb, int skip_mesh) {
    float3 rd = normalize(light_pos - p);
    float3 ro = p + rd * EPS;
    for (int i = 0; i < sc; i++) {
        if (i == skip_sphere) continue;
        float t;
        if (hit_sphere(ro, rd, spheres[i].c, spheres[i].r, t) && t < light_dist - EPS && t > EPS)
            return true;
    }
    if (nb > 0) {
        int stk[64];
        int sp = 0;
        stk[sp++] = 0;
        while (sp > 0) {
            int ni = stk[--sp];
            BvhNode node = bvh[ni];
            if (!bbox_hit(ro, rd, node.bbox_min, node.bbox_max)) continue;
            if (node.left >= 0) {
                if (sp + 2 > 64) continue;
                stk[sp++] = node.left;
                stk[sp++] = node.right;
            } else {
                for (int i = node.tri_start; i < node.tri_end; i++) {
                    if (skip_mesh >= 0 && tris[i].mesh_idx == skip_mesh) continue;
                    float t, u, v;
                    if (hit_tri(ro, rd, tris[i].v0, tris[i].v1, tris[i].v2, t, u, v) &&
                        t < light_dist - EPS && t > EPS)
                        return true;
                }
            }
        }
    }
    return false;
}

struct MeshMat {
    packed_float3 col;
    float ref;
    float ior;
    int mat_type;
    int tex_type;
    float tex_scale;
    packed_float3 tex_color2;
};

static float hash3(float x, float y, float z) {
    float n = sin(x * 127.1f + y * 311.7f + z * 74.7f) * 43758.5453f;
    return n - floor(n);
}

static float smooth(float t) {
    return t * t * (3.0f - 2.0f * t);
}

static float vnoise(float x, float y, float z) {
    int ix = floor(x), iy = floor(y), iz = floor(z);
    float fx = smooth(x - ix), fy = smooth(y - iy), fz = smooth(z - iz);
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

static float3 eval_texture_uv(float2 uv, float3 primary, int tex_type, float tex_scale, float3 tex_color2) {
    if (tex_type == 0) return primary;
    float s = tex_scale;
    float3 c1 = primary;
    float3 c2 = tex_color2;

    if (tex_type == 1) {
        int u = floor(uv.x * s);
        int v = floor(uv.y * s);
        return ((u + v) & 1) ? c1 : c2;
    }
    if (tex_type == 2) {
        float cx = floor(uv.x * s) + 0.5f;
        float cy = floor(uv.y * s) + 0.5f;
        float dx = uv.x * s - cx;
        float dy = uv.y * s - cy;
        return (dx*dx + dy*dy < 0.12f) ? c1 : c2;
    }
    if (tex_type == 3) {
        float n = vnoise(uv.x * s * 0.5f, uv.y * s * 0.5f, 0.0f);
        float marble = sin((uv.x + uv.y) * s * 1.5f + n * 3.0f) * 0.5f + 0.5f;
        return float3(c1.x * marble + c2.x * (1.0f - marble),
                      c1.y * marble + c2.y * (1.0f - marble),
                      c1.z * marble + c2.z * (1.0f - marble));
    }
    if (tex_type == 4) {
        float dx = uv.x - 0.5f, dy = uv.y - 0.5f;
        float dist = sqrt(dx*dx + dy*dy) * s * 2.0f;
        float ring = sin(dist * M_PI_F * 2.0f) * 0.5f + 0.5f;
        return float3(c1.x * ring + c2.x * (1.0f - ring),
                      c1.y * ring + c2.y * (1.0f - ring),
                      c1.z * ring + c2.z * (1.0f - ring));
    }
    return c1;
}

static float3 eval_texture(float3 p, float3 primary, int tex_type, float tex_scale, float3 tex_color2) {
    if (tex_type == 0) return primary;
    float s = tex_scale;
    float3 c1 = primary;
    float3 c2 = tex_color2;

    if (tex_type == 1) {
        int ix = floor(p.x * s);
        int iy = floor(p.y * s);
        int iz = floor(p.z * s);
        return ((ix + iy + iz) & 1) ? c1 : c2;
    }
    if (tex_type == 2) {
        float cx = floor(p.x * s) + 0.5f;
        float cy = floor(p.y * s) + 0.5f;
        float cz = floor(p.z * s) + 0.5f;
        float dx = p.x * s - cx;
        float dy = p.y * s - cy;
        float dz = p.z * s - cz;
        float dist = sqrt(dx*dx + dy*dy + dz*dz);
        return (dist < 0.35f) ? c1 : c2;
    }
    if (tex_type == 3) {
        float n = vnoise(p.x * s * 0.5f, p.y * s * 0.5f, p.z * s * 0.5f);
        float marble = sin((p.x + p.z) * s * 1.5f + n * 3.0f) * 0.5f + 0.5f;
        return float3(c1.x * marble + c2.x * (1.0f - marble),
                      c1.y * marble + c2.y * (1.0f - marble),
                      c1.z * marble + c2.z * (1.0f - marble));
    }
    if (tex_type == 4) {
        float dist = sqrt(p.x*p.x + p.z*p.z) * s;
        float ring = sin(dist * M_PI_F * 2.0f) * 0.5f + 0.5f;
        return float3(c1.x * ring + c2.x * (1.0f - ring),
                      c1.y * ring + c2.y * (1.0f - ring),
                      c1.z * ring + c2.z * (1.0f - ring));
    }
    return c1;
}

static float3 trace_ray(float3 o, float3 d, device const SphereGpu* spheres, int sc,
                        device const TriGpu* tris, int tc, device const BvhNode* bvh, int nb,
                        device const MeshMat* mats, int nm,
                        device const LightGpu* lights, int nl,
                        device const EmissiveGpu* emissive, int ne,
                        device const float* emissive_cdf, int ncdf,
                        int sample_idx,
                        texture2d<float> env_tex, int has_env) {
    packed_float3 stk_o[MAX_DEPTH + 2];
    packed_float3 stk_d[MAX_DEPTH + 2];
    packed_float3 stk_th[MAX_DEPTH + 2];
    int stk_dp[MAX_DEPTH + 2];
    int stk = 0;
    float3 accum = float3(0.0f);

    stk_o[stk] = (packed_float3)o;
    stk_d[stk] = (packed_float3)d;
    stk_th[stk] = (packed_float3)float3(1.0f);
    stk_dp[stk] = 0;
    stk++;

    while (stk > 0) {
        stk--;
        float3 ro = stk_o[stk];
        float3 rd = stk_d[stk];
        float3 thru = stk_th[stk];
        int dp0 = stk_dp[stk];

        for (int depth = dp0; depth <= MAX_DEPTH; depth++) {
            float ts, tf;
            float3 sn;
            int si = -1;
            bool hs = hit_any_sphere(ro, rd, ts, sn, si, spheres, sc);

            float tm = 1e9f;
            float3 mn = float3(0);
            int mi = -1;
            float mu = 0, mv = 0;
            float2 mesh_uv = float2(0);
            if (nb > 0) {
                int stk2[64];
                int sp = 0;
                stk2[sp++] = 0;
                while (sp > 0) {
                    int ni = stk2[--sp];
                    BvhNode node = bvh[ni];
                    if (!bbox_hit(ro, rd, node.bbox_min, node.bbox_max)) continue;
                    if (node.left >= 0) {
                        if (sp + 2 > 64) continue;
                        stk2[sp++] = node.left;
                        stk2[sp++] = node.right;
                    } else {
                        for (int i = node.tri_start; i < node.tri_end; i++) {
                            float ti, u, v;
                            if (hit_tri(ro, rd, tris[i].v0, tris[i].v1, tris[i].v2, ti, u, v) && ti < tm) {
                                tm = ti; mi = i; mu = u; mv = v;
                            }
                        }
                    }
                }
            }
            bool hm = mi >= 0;

            bool hf0 = hit_floor(ro, rd, tf);

            int hit_type = 0;
            float t_hit;
            float3 hit_n;
            float3 sc_col = float3(1.0f);
            float sref = 0, sior = 1.5f;
            int smat = 0;

            if (hs && (!hf0 || ts < tf) && (!hm || ts < tm)) {
                hit_type = 1; t_hit = ts; hit_n = sn;
                sc_col = spheres[si].col;
                sref = spheres[si].ref;
                sior = spheres[si].ior;
                smat = spheres[si].mat_type;
            } else if (hm && (!hf0 || tm < tf)) {
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
                mesh_uv = (1.0f - mu - mv) * tris[mi].t0 + mu * tris[mi].t1 + mv * tris[mi].t2;
            } else if (hf0) {
                hit_type = 3; t_hit = tf;
            }

            if (hit_type == 0) {
                float3 env_col = has_env ? sample_envmap(env_tex, rd) : env_procedural(rd);
                accum += env_col * thru;
                break;
            }

            float3 p = ro + rd * t_hit;

            if (hit_type == 3) {
                float3 nf = float3(0, 1, 0);
                float3 fl = floor_color(p);
                float3 lit = fl * 0.15f;
                for (int li = 0; li < nl; li++) {
                    float3 ld = normalize(lights[li].pos - p);
                    int sidx = (sample_idx << 2) | li;
                    bool sh = in_shadow(p, lights[li], spheres, sc, tris, tc, bvh, nb, sidx, -1);
                    float diff = max(0.0f, dot(nf, ld));
                    float lf = sh ? 0.2f : 1.0f;
                    lit += fl * diff * lf;
                }
                for (int ei = 0; ei < ne; ei++) {
                    float3 lp, ln;
                    float ldist, pdf;
                    if (emissive[ei].type == 0) {
                        lp = sample_emissive_sphere_gpu(emissive[ei].c, emissive[ei].r, ln, sample_idx, ei);
                        ldist = length(lp - p);
                        pdf = 1.0f / emissive[ei].area;
                    } else {
                        lp = sample_emissive_mesh_gpu(tris, emissive_cdf,
                                                       emissive[ei].tri_start, emissive[ei].tri_end,
                                                       emissive[ei].cdf_offset, emissive[ei].area,
                                                       ln, ldist, sample_idx, ei, p);
                        pdf = 1.0f / emissive[ei].area;
                    }
                    if (ldist < 1e-4f) continue;
                    float3 wi = normalize(lp - p);
                    float cos_surf = max(0.0f, dot(nf, wi));
                    if (cos_surf <= 0) continue;
                    float cos_light = max(0.0f, dot(ln, -wi));
                    if (cos_light <= 0) continue;
                    float G = cos_surf * cos_light / (ldist * ldist);
                    int skip_sphere = emissive[ei].type == 0 ? emissive[ei].src_idx : -1;
                    int skip_mesh = emissive[ei].type == 1 ? emissive[ei].src_idx : -1;
                    bool vis = !emissive_visible_gpu(p, lp, ldist, spheres, sc, skip_sphere,
                                                      tris, tc, bvh, nb, skip_mesh);
                    if (vis) {
                        lit += fl * emissive[ei].emitted * (G / pdf);
                    }
                }
                accum += lit * thru;
                break;
            }

            float3 n_hit = hit_n;
            int mat = smat;

            if (hit_type == 1) {
                sc_col = eval_texture(p, sc_col, spheres[si].tex_type, spheres[si].tex_scale, spheres[si].tex_color2);
            } else if (hit_type == 2) {
                int midx = tris[mi].mesh_idx;
                if (midx >= 0 && midx < nm)
                    sc_col = (mesh_uv.x != 0 || mesh_uv.y != 0)
                        ? eval_texture_uv(mesh_uv, sc_col, mats[midx].tex_type, mats[midx].tex_scale, mats[midx].tex_color2)
                        : eval_texture(p, sc_col, mats[midx].tex_type, mats[midx].tex_scale, mats[midx].tex_color2);
            }

            if (mat == MAT_EMISSIVE) {
                accum += sc_col * thru;
                break;
            }

            float3 lit = float3(0.0f);
            for (int li = 0; li < nl; li++) {
                float3 ld = normalize(lights[li].pos - p);
                int sidx = (sample_idx << 2) | li;
                bool sh = in_shadow(p, lights[li], spheres, sc, tris, tc, bvh, nb,
                                    sidx, hit_type == 1 ? si : -1);

                float diff = max(0.0f, dot(n_hit, ld));
                float3 vw = normalize(ro - p);
                float3 hv = normalize(ld + vw);
                float sp = pow(max(0.0f, dot(n_hit, hv)),
                               (mat == MAT_PLASTIC || mat == MAT_SUBSURFACE) ? 32.0f : 64.0f);
                float lf = sh ? 0.0f : 1.0f;
                float ss = (mat == MAT_PLASTIC || mat == MAT_SUBSURFACE) ? 0.4f : 0.8f;

                if (mat == MAT_SUBSURFACE) {
                    float bdiff = max(0.0f, dot(-n_hit, ld));
                    lit += sc_col * diff * lf * 0.7f
                         + sc_col * bdiff * lf * 0.3f
                         + sc_col * sp * ss * lf;
                } else {
                    lit += sc_col * diff * lf + sc_col * sp * ss * lf;
                }
            }
            for (int ei = 0; ei < ne; ei++) {
                int skip_sph = -1, skip_mesh = -1;
                float3 lp, ln;
                float ldist, pdf;
                if (emissive[ei].type == 0) {
                    if (hit_type == 1 && si == emissive[ei].src_idx) continue;
                    skip_sph = emissive[ei].src_idx;
                    lp = sample_emissive_sphere_gpu(emissive[ei].c, emissive[ei].r, ln, sample_idx, ei);
                    ldist = length(lp - p);
                    pdf = 1.0f / emissive[ei].area;
                } else {
                    int mesh_idx = emissive[ei].src_idx;
                    if (hit_type == 2 && tris[mi].mesh_idx == mesh_idx) continue;
                    skip_mesh = mesh_idx;
                    lp = sample_emissive_mesh_gpu(tris, emissive_cdf,
                                                   emissive[ei].tri_start, emissive[ei].tri_end,
                                                   emissive[ei].cdf_offset, emissive[ei].area,
                                                   ln, ldist, sample_idx, ei, p);
                    pdf = 1.0f / emissive[ei].area;
                }
                if (ldist < 1e-4f) continue;
                float3 wi = normalize(lp - p);
                float cos_surf = max(0.0f, dot(n_hit, wi));
                if (cos_surf <= 0) continue;
                float cos_light = max(0.0f, dot(ln, -wi));
                if (cos_light <= 0) continue;
                float G = cos_surf * cos_light / (ldist * ldist);
                bool vis = !emissive_visible_gpu(p, lp, ldist, spheres, sc, skip_sph,
                                                  tris, tc, bvh, nb, skip_mesh);
                if (vis) {
                    lit += sc_col * emissive[ei].emitted * (G / pdf);
                }
            }
            float3 amb = sc_col * 0.15f;
            float3 base = amb + lit;
            accum += base * thru;

            if (mat == MAT_PLASTIC || mat == MAT_SUBSURFACE || depth == MAX_DEPTH) break;

            float cos_i = dot(n_hit, rd);
            bool entering = cos_i < 0;
            float3 na = entering ? n_hit : -n_hit;
            cos_i = entering ? -cos_i : cos_i;

            float3 refl_d = reflect(rd, na);
            float3 refl_o = p + refl_d * EPS;

            if (mat == MAT_METALLIC) {
                ro = refl_o;
                rd = refl_d;
                thru *= sc_col;
                continue;
            }

            float n1 = entering ? 1.0f : sior;
            float n2 = entering ? sior : 1.0f;
            float eta = n1 / n2;
            float k = 1.0f - eta * eta * (1.0f - cos_i * cos_i);

            float r0 = (1.0f - sior) / (1.0f + sior);
            r0 = r0 * r0;
            float fresnel = r0 + (1.0f - r0) * pow(1.0f - cos_i, 5.0f);

            if (k > 0) {
                float cos_t = sqrt(k);
                float3 refr_d = rd * eta + na * (eta * cos_i - cos_t);
                float3 refr_o = p + refr_d * EPS;

                float3 cur_thru = thru;
                if (stk < MAX_DEPTH + 2) {
                    stk_o[stk] = (packed_float3)refr_o;
                    stk_d[stk] = (packed_float3)refr_d;
                    stk_th[stk] = (packed_float3)(cur_thru * (1.0f - fresnel) * sc_col);
                    stk_dp[stk] = depth + 1;
                    stk++;
                }

                ro = refl_o;
                rd = refl_d;
                thru = cur_thru * fresnel * sref;
            } else {
                ro = refl_o;
                rd = refl_d;
                thru *= fresnel * sref;
            }
        }
    }
    return accum;
}

kernel void rk(
    device packed_float3* out [[buffer(0)]],
    constant CameraGpu& cam [[buffer(1)]],
    constant SceneGpu& scene [[buffer(2)]],
    device const SphereGpu* spheres [[buffer(3)]],
    device const TriGpu* tris [[buffer(4)]],
    device const BvhNode* bvh [[buffer(5)]],
    device const MeshMat* mats [[buffer(6)]],
    device const LightGpu* lights [[buffer(7)]],
    device const EmissiveGpu* emissive [[buffer(8)]],
    device const float* emissive_cdf [[buffer(9)]],
    texture2d<float> env_tex [[texture(0)]],
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
            int sidx = sy * AA_SAMPLES + sx;
            float ux = (2.0f * (x + (sx + 0.5f) / AA_SAMPLES) / scene.width - 1.0f) * asp;
            float uy = 1.0f - 2.0f * (y + (sy + 0.5f) / AA_SAMPLES) / scene.height;
            float3 rd = normalize(fwd + right * ux + up * uy);

            float3 origin = cam.pos;
            if (cam.aperture > 0.0f) {
                float3 focal = cam.pos + rd * cam.focus_dist;
                float angle = 2.0f * M_PI_F * (sx + 0.5f) / AA_SAMPLES;
                float r = cam.aperture * 0.5f * sqrt((sy + 0.5f) / AA_SAMPLES);
                float3 off = right * (r * cos(angle)) + up * (r * sin(angle));
                origin = cam.pos + off;
                rd = normalize(focal - origin);
            }

            sum += trace_ray(origin, rd, spheres, scene.num_spheres,
                             tris, scene.num_mesh_tris, bvh, scene.num_bvh_nodes,
                             mats, scene.num_meshes,
                             lights, scene.num_lights,
                             emissive, scene.num_emissive,
                             emissive_cdf, scene.num_emissive_cdf,
                             sidx,
                             env_tex, scene.has_env);
        }
    }
    float3 final = sum / (float)(AA_SAMPLES * AA_SAMPLES);
    out[y * scene.width + x] = tone_map(final, scene.exposure);
}
