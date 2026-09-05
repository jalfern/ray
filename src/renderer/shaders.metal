#include <metal_stdlib>
using namespace metal;

/* Upper bound on scene textures bound as an argument-buffer texture array.
   The CPU samples textures[per-material index] freely; the GPU mirrors that
   by binding every scene texture and indexing this array by the same index. */
#define MAXTEX 64

constant float EPS = 1e-4f;
constant int AA_SAMPLES = 16;
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

/* Argument-buffer bundle of every scene texture.  Metal allows dynamic
   indexing only through an array that lives in an argument buffer (a struct
   passed by device reference), so the CPU's textures[per-material index]
   sampling is mirrored by bundling all textures and indexing .t[idx]. */
struct TexBundle {
    array<texture2d<float>, MAXTEX> t;
};

struct SphereGpu {
    packed_float3 c;
    float r;
    float ref;
    float ior;
    float roughness;
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
    float fov_scale;
    int num_textures;
    int has_floor;
    int has_bg_color;
    float bg_r;
    float bg_g;
    float bg_b;
    float sh[12];   /* Phase 2 IBL diffuse SH: [3][4] {c00,c1x,c1y,c1z} per channel */
    int env_mips;   /* mip-chain length of env_tex (0 = no env) */
    int env_w, env_h;  /* base env dimensions (for buffer-based prefiltered read) */
    int dbg_x;      /* TEMP-DBG: pixel to log env escapes for (-1 = off) */
    int dbg_y;
};

static_assert(sizeof(SceneGpu) == 140, "SceneGpu size must match gpu_renderer.mm");

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
    float len1_sq = e1.x*e1.x + e1.y*e1.y + e1.z*e1.z;
    float len2_sq = e2.x*e2.x + e2.y*e2.y + e2.z*e2.z;
    if (fabs(det) < 1e-7 * (len1_sq + len2_sq + 1e-12) * 0.5) return false;
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
    return ((int(floor(p.x))+int(floor(p.z))) & 1) ? float3(0.08f,0.12f,0.25f) : float3(0.25f,0.4f,0.7f);
}

static float3 sample_envmap(texture2d<float> env_tex, float3 d) {
    float u = atan2(d.z, d.x) * (0.5f / M_PI_F) + 0.5f;
    float v = acos(clamp(d.y, -1.0f, 1.0f)) * (1.0f / M_PI_F);
    return env_tex.sample(sampler(filter::linear, address::repeat), float2(u, v)).rgb;
}

/* Phase 2 IBL: bilinear over one equirect mip level, x and y wrap
   (periodic).  Reads the CPU-built mip chain from a device buffer
   (mip is the packed chain, base_w/base_h the level-0 size).  Mirrors
   the CPU's envmap.cc sample_level_data operation-for-operation
   (u*w-0.5 texel-center offset, floor, wrap, 4-texel lerp).  Used
   because this MSL's sample(sampler,coord,lod) explicit-LOD overload
   samples the wrong level and sample_level / read(int2,level) are
   unavailable. */
 static float3 env_bilinear_read(device const float* mip, int base_w, int base_h,
                                  int level, float u, float v) {
     int w = base_w >> level; if (w < 1) w = 1;
     int h = base_h >> level; if (h < 1) h = 1;
     int offset = 0;
     for (int i = 0; i < level; i++) {
         int wi = base_w >> i; if (wi < 1) wi = 1;
         int hi = base_h >> i; if (hi < 1) hi = 1;
         offset += wi * hi * 3;
     }
     device const float* data = mip + offset;
     float fx = u * (float)w - 0.5f;
     float fy = v * (float)h - 0.5f;
     int ix = (int)floor(fx);
     int iy = (int)floor(fy);
     float tx = fx - (float)ix;
     float ty = fy - (float)iy;
     ix = ((ix % w) + w) % w;
     iy = ((iy % h) + h) % h;
     int ix1 = (ix + 1) % w;
     int iy1 = (iy + 1) % h;
     device const float* p00 = data + (iy * w + ix) * 3;
     device const float* p10 = data + (iy * w + ix1) * 3;
     device const float* p01 = data + (iy1 * w + ix) * 3;
     device const float* p11 = data + (iy1 * w + ix1) * 3;
     float wx0 = 1.0f - tx, wx1 = tx;
     float wy0 = 1.0f - ty, wy1 = ty;
     float3 r;
     r.x = (p00[0] * wx0 + p10[0] * wx1) * wy0 + (p01[0] * wx0 + p11[0] * wx1) * wy1;
     r.y = (p00[1] * wx0 + p10[1] * wx1) * wy0 + (p01[1] * wx0 + p11[1] * wx1) * wy1;
     r.z = (p00[2] * wx0 + p10[2] * wx1) * wy0 + (p01[2] * wx0 + p11[2] * wx1) * wy1;
     return r;
 }

/* Phase 2 IBL: the env blurred by a surface's roughness, for a
   specular/refracted ray escaping to the environment.  roughness in
   [0,1] maps onto the mip chain the CPU built in envmap.cc
   (roughness * maxLevelOfDetail); linear-mip-linear between the
   bracketing levels, bilinear within a level — the same algorithm as
   the CPU's envmap_sample_prefiltered.  mip is the packed CPU mip chain
   (see env_bilinear_read); env_w/env_h are the level-0 dimensions. */
 static float3 sample_env_prefiltered(device const float* mip, int env_w, int env_h,
                                       float3 d, float rough, int env_mips) {
     float u = atan2(d.z, d.x) * (0.5f / M_PI_F) + 0.5f;
     float v = acos(clamp(d.y, -1.0f, 1.0f)) * (1.0f / M_PI_F);
     int maxlod = env_mips - 1;   /* must match the CPU's env->num_mips - 1 */
     float lod = clamp(rough, 0.0f, 1.0f) * (float)maxlod;
     int l0 = (int)floor(lod);
     if (l0 > maxlod) l0 = maxlod;
     float f = lod - (float)l0;
     int l1 = min(l0 + 1, maxlod);
     float3 c0 = env_bilinear_read(mip, env_w, env_h, l0, u, v);
     float3 c1 = env_bilinear_read(mip, env_w, env_h, l1, u, v);
     return c0 + (c1 - c0) * f;
 }

/* Phase 2 IBL: band-0..1 SH diffuse irradiance, the closed-form
   hemispherical integral  E(N) = sqrt(pi)*c00 + sqrt(pi/3)*(c1 . N).
   Operation-for-operation with the CPU's envmap_irradiance; sh is
   SceneGpu.sh (12 floats, [channel]{c00,c1x,c1y,c1z}). */
static float3 env_irradiance_sh(constant const float* sh, float3 N) {
    const float SQRT_PI = 1.7724538509055160f;
    const float SQRT_PI_3 = 1.0233267079464890f;
    float3 c00 = float3(sh[0], sh[4], sh[8]);
    float3 c1d = float3(sh[1] * N.x + sh[2] * N.y + sh[3] * N.z,
                        sh[5] * N.x + sh[6] * N.y + sh[7] * N.z,
                        sh[9] * N.x + sh[10] * N.y + sh[11] * N.z);
    return SQRT_PI * c00 + SQRT_PI_3 * c1d;
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
    col = min(col, 1.0f);
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
                      int sample_idx, int origin, int skip_mesh) {
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
                    if (skip_mesh >= 0 && tris[i].mesh_idx == skip_mesh) continue;
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
    /* Flip normal toward the shaded point so winding doesn't silently kill emission. */
    float3 to_p = normalize(from - p);
    if (dot(n, to_p) < 0) n = -n;
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
    float roughness;
    float metallic;
    float transmission;
    int mat_type;
    int tex_type;
    float tex_scale;
    packed_float3 tex_color2;
    int tex_index;
    int orm_tex_index;
    int iri_tex_index;
    float iri_factor;
    float iri_ior;
    float iri_thin_min;
    float iri_thin_max;
    float vol_th;
    float att_r;
    float att_g;
    float att_b;
    float att_dist;
    int vol_tex_index;
    int ao_tex_index;
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

static float srgb_to_linear(float c) {
    return c <= 0.04045f ? c / 12.92f : pow((c + 0.055f) / 1.055f, 2.4f);
}

static float3 sample_base_color(texture2d<float> tex, float2 uv) {
    constexpr sampler s(filter::linear, address::repeat);
    float u = uv.x - floor(uv.x);
    float v = uv.y - floor(uv.y);
    float4 sample = tex.sample(s, float2(u, v));
    return float3(srgb_to_linear(sample.r), srgb_to_linear(sample.g), srgb_to_linear(sample.b));
}

/* ORM data is linear (not color) — sample raw, no transfer function. */
static float3 sample_linear(texture2d<float> tex, float2 uv) {
    constexpr sampler s(filter::linear, address::repeat);
    float u = uv.x - floor(uv.x);
    float v = uv.y - floor(uv.y);
    return tex.sample(s, float2(u, v)).rgb;
}

/* ── KHR_materials_iridescence: thin-film interference ─────────
 *  MSL port, line-for-line with include/thin_film.h (source of record
 *  for the constants: three.js iridescence_fragment.glsl.js / common.
 *  glsl.js / lights_physical_pars_fragment.glsl.js).  d_nm is in nm.
 *  cv = clamped |N·V| (view direction, per the reference).
 */
static float tf_r_f0(float n_a, float n_b) {
    float t = (n_a - n_b) / (n_a + n_b);
    return t * t;
}

static float tf_f_schlick(float f0, float f90, float cos_v) {
    float fresnel = exp2((-5.55473f * cos_v - 6.98316f) * cos_v);
    return f0 * (1.0f - fresnel) + f90 * fresnel;
}

static float3 tf_eval_sensitivity(float opd, float3 phi) {
    float phase = 2.0f * M_PI_F * opd * 1.0e-9f;
    float p2 = phase * phase;
    float3 xr = float3(5.4856e-13f, 4.4201e-13f, 5.2481e-13f)
              * float3(sqrt(2.0f * M_PI_F * 4.3278e+09f), sqrt(2.0f * M_PI_F * 9.3046e+09f), sqrt(2.0f * M_PI_F * 6.6121e+09f))
              * float3(cos(1.6810e+06f * phase + phi.x), cos(1.7953e+06f * phase + phi.y), cos(2.2084e+06f * phase + phi.z))
              * float3(exp(-p2 * 4.3278e+09f), exp(-p2 * 9.3046e+09f), exp(-p2 * 6.6121e+09f));
    xr.x += 9.7470e-14f * sqrt(2.0f * M_PI_F * 4.5282e+09f) * cos(2.2399e+06f * phase + phi.x) * exp(-p2 * 4.5282e+09f);
    xr /= 1.0685e-7f;
    /* XYZ -> linear sRGB (Rec.709) */
    return float3(3.2404542f * xr.x - 1.5371385f * xr.y - 0.4985314f * xr.z,
                  -0.9692660f * xr.x + 1.8760108f * xr.y + 0.0415560f * xr.z,
                  0.0556434f * xr.x - 0.2040259f * xr.y + 1.0572252f * xr.z);
}

static float3 tf_eval_iridescence(float outside_ior, float eta2, float cv, float d_nm, float3 base_f0) {
    /* Force iridescenceIOR -> outsideIOR when thinFilmThickness -> 0.0 */
    float s = d_nm / 0.03f;
    s = clamp(s, 0.0f, 1.0f);
    s = s * s * (3.0f - 2.0f * s);
    float n_f = outside_ior + (eta2 - outside_ior) * s;

    /* cosTheta2 via Snell's law, with TIR handling */
    float si = outside_ior / n_f;
    float cos2q = 1.0f - si * si * (1.0f - cv * cv);
    if (cos2q < 0.0f) {
        return float3(1.0f);
    }
    float cos2 = sqrt(cos2q);

    /* First interface (air / film) */
    float R12 = tf_f_schlick(tf_r_f0(n_f, outside_ior), 1.0f, cv);
    float T121 = 1.0f - R12;
    float phi12 = 0.0f;
    if (n_f < outside_ior) phi12 = M_PI_F;
    float phi21 = M_PI_F - phi12;

    /* Second interface (film / base material) */
    float3 bf0 = clamp(base_f0, 0.0f, 0.9999f);
    float3 sr = sqrt(bf0);
    float3 bior = (float3(1.0f) + sr) / (float3(1.0f) - sr);
    float3 R1 = float3(tf_r_f0(bior.x, n_f), tf_r_f0(bior.y, n_f), tf_r_f0(bior.z, n_f));
    float3 R23 = float3(tf_f_schlick(R1.x, 1.0f, cos2), tf_f_schlick(R1.y, 1.0f, cos2), tf_f_schlick(R1.z, 1.0f, cos2));
    float3 phi23 = float3((bior.x < n_f) ? M_PI_F : 0.0f,
                          (bior.y < n_f) ? M_PI_F : 0.0f,
                          (bior.z < n_f) ? M_PI_F : 0.0f);

    /* Phase shift and compound terms */
    float opd = 2.0f * n_f * d_nm * cos2;
    float3 R123 = clamp(R12 * R23, 1e-5f, 0.9999f);
    float3 r123 = sqrt(R123);
    float3 Rs = T121 * T121 * R23 / (float3(1.0f) - R123);

    /* m = 0 (DC term amplitude), then m = 1, 2 (pairs of diracs) */
    float3 I = R12 + Rs;
    float3 Cm = Rs - T121;
    for (int m = 1; m <= 2; m++) {
        Cm *= r123;
        float3 Sm = 2.0f * tf_eval_sensitivity((float)m * opd, (float)m * (float3(phi21) + phi23));
        I += Cm * Sm;
    }

    /* Out-of-gamut colors can be produced; clamp negative values to 0. */
    return max(I, float3(0.0f));
}

static float3 tf_schlick_to_f0(float3 f, float f90, float cos_v) {
    float x = clamp(1.0f - cos_v, 0.0f, 1.0f);
    float x2 = x * x;
    float x5 = clamp(x * x2 * x2, 0.0f, 0.9999f);
    return (f - float3(f90) * x5) / (1.0f - x5);
}

/* ── KHR_materials_volume: Beer-Lambert attenuation ─────────────
 *  MSL port, operation-for-operation with include/volume.h
 *  (reference model: three.js volumeAttenuation in
 *  transmission_pars_fragment.glsl.js).  dist is the actual
 *  ray-traced path length in the medium (KHR spec instruction to
 *  ray tracers; three.js uses the material thickness parameter).
 *  att_d == +inf short-circuits to float3(1) so default materials
 *  leave renders byte-identical.
 */
static float3 vol_transmittance(float dist, float3 att_c, float att_d) {
    if (isinf(att_d)) return float3(1.0f);
    float3 coeff = -log(att_c) / att_d;
    return exp(-coeff * dist);
}

/* 1 if any channel's attenuation coefficient is non-zero. */
static int vol_sigma_nonzero(float3 att_c, float att_d) {
    if (isinf(att_d)) return 0;
    if (att_c.x == 1.0f && att_c.y == 1.0f && att_c.z == 1.0f) return 0;
    return 1;
}

static float3 trace_ray(float3 o, float3 d, device const SphereGpu* spheres, int sc,
                        device const TriGpu* tris, int tc, device const BvhNode* bvh, int nb,
                        device const MeshMat* mats, int nm,
                        device const LightGpu* lights, int nl,
                        device const EmissiveGpu* emissive, int ne,
                        device const float* emissive_cdf, int ncdf,
                        int sample_idx, int num_textures,
                        texture2d<float> env_tex, int has_env,
                        constant const float* ibl_sh, int env_mips,
                        device const float* env_mip, int env_w, int env_h,
                        const device TexBundle& scene_tex,
                        int has_floor, int has_bg_color,
                        float bg_r, float bg_g, float bg_b) {
    packed_float3 stk_o[MAX_DEPTH + 2];
    packed_float3 stk_d[MAX_DEPTH + 2];
    packed_float3 stk_th[MAX_DEPTH + 2];
    float4 stk_md[MAX_DEPTH + 2];  /* KHR_materials_volume medium: (ior, cr, cg, cb) */
    float stk_ma[MAX_DEPTH + 2];   /* attenuationDistance, may be +inf */
    int stk_dp[MAX_DEPTH + 2];
    float stk_sr[MAX_DEPTH + 2];   /* Phase 2 IBL: roughness of the surface that
                                      spawned this ray (< 0 = primary/sharp env) */
    int stk = 0;
    float3 accum = float3(0.0f);

    stk_o[stk] = (packed_float3)o;
    stk_d[stk] = (packed_float3)d;
    stk_th[stk] = (packed_float3)float3(1.0f);
    stk_md[stk] = float4(1.0f, 1.0f, 1.0f, 1.0f);  /* air */
    stk_ma[stk] = INFINITY;
    stk_dp[stk] = 0;
    stk_sr[stk] = -1.0f;
    stk++;

    while (stk > 0) {
        stk--;
        float3 ro = stk_o[stk];
        float3 rd = stk_d[stk];
        float3 thru = stk_th[stk];
        float4 mid_c = stk_md[stk];   /* (ior, cr, cg, cb) — air is float4(1,1,1,1) */
        float mid_d = stk_ma[stk];
        int dp0 = stk_dp[stk];
        float sr = stk_sr[stk];
        /* "In a medium" is discriminated by ior > 1 (CPU twin: Medium.ior).
           Air is stored as ior 1.0.  This assumes a transmitting volume
           always has ior > 1 (KHR guarantees it in practice); a volume with
           ior <= 1 would be silently treated as air.  Keep this float4
           packing in lockstep with the CPU Medium struct. */
        bool in_med = mid_c.x > 1.0f;

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

            bool hf0 = has_floor != 0 && hit_floor(ro, rd, tf);

            int hit_type = 0;
            bool side_entry = true;   /* spheres always report their near face */
            float t_hit;
            float3 hit_n;
            float3 sc_col = float3(1.0f);
            float sref = 0, sior = 1.5f, srough = 1.0f;
            float smetal = 0.0f, sao = 1.0f, strans = 0.0f;
            int smat = 0;

            if (hs && (!hf0 || ts < tf) && (!hm || ts < tm)) {
                hit_type = 1; t_hit = ts; hit_n = sn;
                sc_col = spheres[si].col;
                sref = spheres[si].ref;
                sior = spheres[si].ior;
                srough = spheres[si].roughness;
                smat = spheres[si].mat_type;
                smetal = (smat == MAT_METALLIC) ? 1.0f : 0.0f;
            } else if (hm && (!hf0 || tm < tf)) {
                hit_type = 2; t_hit = tm;
                int mesh_idx = tris[mi].mesh_idx;
                if (mesh_idx >= 0 && mesh_idx < nm) {
                    sc_col = mats[mesh_idx].col;
                    sref = mats[mesh_idx].ref;
                    sior = mats[mesh_idx].ior;
                    srough = mats[mesh_idx].roughness;
                    smetal = mats[mesh_idx].metallic;
                    strans = mats[mesh_idx].transmission;
                    smat = mats[mesh_idx].mat_type;
                }

                  hit_n = tri_normal(tris[mi].v0, tris[mi].v1, tris[mi].v2,
                                      tris[mi].n0, tris[mi].n1, tris[mi].n2, mu, mv);
                  /* Shell side from the pre-flip normal: volume entry/exit
                     is decided from this — the flip below erases the sign. */
                  side_entry = dot(hit_n, rd) < 0;
                  if (dot(hit_n, rd) > 0) hit_n = -hit_n;
                  mesh_uv = (1.0f - mu - mv) * tris[mi].t0 + mu * tris[mi].t1 + mv * tris[mi].t2;
            } else if (hf0) {
                hit_type = 3; t_hit = tf;
            }

            if (hit_type == 0) {
                /* Phase 2 IBL: a specular/refracted ray (sr >= 0) escaping
                   to the env samples it blurred by the spawning surface's
                   roughness (the traced mirror is the sampled lobe in the
                   sharp limit — no separate env-lobe term); primary rays
                   (sr < 0) keep the sharp env. */
                 float3 env_col = has_bg_color ? float3(bg_r, bg_g, bg_b)
                                : (has_env ? (sr >= 0.0f ? sample_env_prefiltered(env_mip, env_w, env_h, rd, sr, env_mips)
                                                         : sample_envmap(env_tex, rd))
                                           : env_procedural(rd));
                /* Infinite path in an absorbing medium: photon fully absorbed. */
                if (in_med && vol_sigma_nonzero(float3(mid_c.y, mid_c.z, mid_c.w), mid_d))
                    break;
                accum += env_col * thru;
                break;
            }

            float3 p = ro + rd * t_hit;

            /* KHR_materials_volume: the segment from the previous surface
               to this hit was already traveled inside the current medium.
               Every contribution leaving this surface back toward the
               camera — the surface light (ambient + lit) and the downstream
               reflection and refraction — must carry that segment's
               Beer-Lambert loss.  Charging it here (in addition to the
               downstream bakes) is what tints light arriving at an
               in-medium surface (e.g. the far wall of a solid glass body):
               that light must exit the medium to reach the camera.
               Exactly (1,1,1) without an absorbing medium. */
            float3 Tseg = float3(1.0f);
            if (in_med)
                Tseg = vol_transmittance(t_hit, float3(mid_c.y, mid_c.z, mid_c.w), mid_d);

            if (hit_type == 3) {
                float3 nf = float3(0, 1, 0);
                float3 fl = floor_color(p);
                /* Phase 2 IBL: with a loaded env the flat 0.15 floor ambient
                   is replaced by the band-0..1 SH irradiance for N = +Y. */
                float3 lit = has_env ? fl * env_irradiance_sh(ibl_sh, nf) : fl * 0.15f;
                for (int li = 0; li < nl; li++) {
                    float3 ld = normalize(lights[li].pos - p);
                    int sidx = (sample_idx << 2) | li;
                    bool sh = in_shadow(p, lights[li], spheres, sc, tris, tc, bvh, nb, sidx, -1, -1);
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
                        pdf = 2.0f / emissive[ei].area;
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
                    float G = cos_surf * cos_light / fmax(ldist * ldist, 1e-3f);
                    int skip_sphere = emissive[ei].type == 0 ? emissive[ei].src_idx : -1;
                    int skip_mesh = emissive[ei].type == 1 ? emissive[ei].src_idx : -1;
                    bool vis = !emissive_visible_gpu(p, lp, ldist, spheres, sc, skip_sphere,
                                                      tris, tc, bvh, nb, skip_mesh);
                    if (vis) {
                        lit += fl * emissive[ei].emitted * (G / pdf);
                    }
                }
                accum += lit * (thru * Tseg);
                break;
            }

            float3 n_hit = hit_n;
            int mat = smat;

            if (hit_type == 1) {
                sc_col = eval_texture(p, sc_col, spheres[si].tex_type, spheres[si].tex_scale, spheres[si].tex_color2);
            } else if (hit_type == 2) {
                int midx = tris[mi].mesh_idx;
                if (midx >= 0 && midx < nm) {
                    if (mats[midx].tex_index >= 0 && mats[midx].tex_index < num_textures &&
                        mats[midx].tex_index < MAXTEX) {
                        sc_col = sample_base_color(scene_tex.t[mats[midx].tex_index], mesh_uv);
                    } else {
                        sc_col = (mesh_uv.x != 0 || mesh_uv.y != 0)
                            ? eval_texture_uv(mesh_uv, sc_col, mats[midx].tex_type, mats[midx].tex_scale, mats[midx].tex_color2)
                            : eval_texture(p, sc_col, mats[midx].tex_type, mats[midx].tex_scale, mats[midx].tex_color2);
                    }
                    /* Override material params from ORM texture (linear data): G = roughness, B = metallic, R = AO. */
                    if (mats[midx].orm_tex_index >= 0 && mats[midx].orm_tex_index < num_textures &&
                        mats[midx].orm_tex_index < MAXTEX) {
                        float3 orm = sample_linear(scene_tex.t[mats[midx].orm_tex_index], mesh_uv);
                        srough = srough * orm.g;
                        smetal = smetal * orm.b;
                        sao = orm.r;
                    }
                }
            }

            if (mat == MAT_EMISSIVE) {
                accum += sc_col * (thru * Tseg);
                break;
            }

             /* Merged plastic+metallic PBR params (per pixel):
                kd = 1 - metallic, F0 = mix(0.04, basecolor, metallic).
                F0 is sc when metallic=1 and 0.04 when metallic=0. */
             float kd = 1.0f - smetal;
             float3 f0 = sc_col * smetal + float3(0.04f) * kd;

             /* KHR_materials_iridescence: thin-film interference tint on the
                specular/reflection lobes (view-direction angle blend per the
                reference model).  Entering hits only — the film coats the outer
                surface.  Diffuse/ambient terms untouched. */
             float3 film = float3(0.0f);
             float film_w = 0.0f;
             float3 f0u = f0;
             if (hit_type == 2 && tris[mi].mesh_idx >= 0 && tris[mi].mesh_idx < nm &&
                 mats[tris[mi].mesh_idx].iri_factor > 0.0f &&
                 (mat != MAT_GLASS || dot(n_hit, rd) < 0.0f)) {
                 int iri_mid = tris[mi].mesh_idx;
                 float tv = 1.0f; /* three.js: no thickness map -> maximum thickness */
                  if (mats[iri_mid].iri_tex_index >= 0 && mats[iri_mid].iri_tex_index < num_textures &&
                      mats[iri_mid].iri_tex_index < MAXTEX) {
                      tv = sample_linear(scene_tex.t[mats[iri_mid].iri_tex_index], mesh_uv).g;
                  }
                 float d_nm = mats[iri_mid].iri_thin_min +
                              (mats[iri_mid].iri_thin_max - mats[iri_mid].iri_thin_min) * tv;
                 float cv = min(abs(dot(n_hit, normalize(ro - p))), 1.0f);
                 float3 bf0 = f0;
                 if (mat == MAT_GLASS) {
                     /* Dielectric substrate F0 from the glass IOR (three.js feeds
                        the material's specularColor; r0 ~ 0.054 for IOR 1.6). */
                     float gr = (1.0f - sior) / (1.0f + sior);
                     gr *= gr;
                     bf0 = sc_col * gr;
                 }
                 film_w = min(mats[iri_mid].iri_factor, 1.0f);
                 float3 filmv = tf_eval_iridescence(1.0f, mats[iri_mid].iri_ior, cv, d_nm, bf0);
                 float3 filmf0 = tf_schlick_to_f0(filmv, 1.0f, cv);
                 f0u = f0 * (1.0f - film_w) + filmf0 * film_w;
                 film = filmv;
             }

              /* KHR_materials_transmission: three.js replaces the diffuse
                 with the transmitted light; scale every diffuse contribution
                 (direct, ambient, emissive) by (1 - transmission).  The
                 specular lobe and the mirror are untouched.  Spheres carry
                 no transmission factor (0) and are unchanged. */
              float glass_trans = (mat == MAT_GLASS && hit_type == 2)
                  ? min(1.0f, max(0.0f, strans)) : 0.0f;

             float3 lit = float3(0.0f);
             /* Shadow-ray self-avoid must skip the SHADING MESH by its mesh
                index (in_shadow filters triangles by tris[].mesh_idx), matching
                the CPU twin which passes the MeshObj index mi.  Passing the
                global triangle index mi here was a type mismatch: no triangle's
                mesh_idx ever equalled a triangle index, so the GPU skipped
                nothing and self-shadowed the shading mesh (visible as the
                CPU/GPU divergence over the pitted olives under the dish). */
             int shadow_skip_mesh = (hit_type == 2 && tris[mi].mesh_idx >= 0 &&
                                     tris[mi].mesh_idx < nm) ? tris[mi].mesh_idx : -1;
             for (int li = 0; li < nl; li++) {
                 float3 ld = normalize(lights[li].pos - p);
                 int sidx = (sample_idx << 2) | li;
                 bool sh = in_shadow(p, lights[li], spheres, sc, tris, tc, bvh, nb,
                                     sidx, hit_type == 1 ? si : -1,
                                     shadow_skip_mesh);

                float diff = max(0.0f, dot(n_hit, ld));
                float3 vw = normalize(ro - p);
                float3 hv = normalize(ld + vw);
                float spec_exp = 2.0f + 510.0f * (1.0f - srough) * (1.0f - srough);
                float sp = pow(max(0.0f, dot(n_hit, hv)), spec_exp);
                float lf = sh ? 0.0f : 1.0f;
                float ss = (mat == MAT_GLASS) ? 0.8f : 0.4f;

                /* AO (ORM.R) attenuates the ambient and diffuse terms only;
                   the specular lobe and the F0-weighted mirror are untouched. */
                if (mat == MAT_SUBSURFACE) {
                    float bdiff = max(0.0f, dot(-n_hit, ld));
                    lit += sc_col * diff * lf * 0.7f * sao
                         + sc_col * bdiff * lf * 0.3f * sao
                         + sc_col * sp * ss * lf;
                } else if (mat == MAT_GLASS) {
                    /* Specular weight takes the film response per channel when present. */
                    float3 glw = sc_col * ss;
                    if (film_w > 0.0f) glw = glw * (1.0f - film_w) + film * film_w;
                    lit += sc_col * diff * lf * sao * (1.0f - glass_trans) + glw * sp * lf;
                } else {
                    lit += (sc_col * kd) * diff * lf * sao + f0u * sp * ss * lf;
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
                    pdf = 2.0f / emissive[ei].area;
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
                float G = cos_surf * cos_light / fmax(ldist * ldist, 1e-3f);
                bool vis = !emissive_visible_gpu(p, lp, ldist, spheres, sc, skip_sph,
                                                  tris, tc, bvh, nb, skip_mesh);
                if (vis) {
                    float3 emd = sc_col * (kd * sao * (1.0f - glass_trans));
                    lit += emd * emissive[ei].emitted * (G / pdf);
                }
            }
            /* Phase 2 IBL: with a loaded env the flat 0.15 ambient is
               replaced by the band-0..1 SH diffuse irradiance —
               kd * irradiance(N) * AO * (1 - transmission), same
               diffuse convention as the CPU (specular/mirror untouched).
               Operation order matches the CPU twin: (sc*ir) * f. */
            float3 amb;
            if (has_env) {
                float3 ir = env_irradiance_sh(ibl_sh, n_hit);
                float f = kd * sao * (1.0f - glass_trans);
                amb = (sc_col * ir) * f;
            } else {
                amb = sc_col * (0.15f * sao * (1.0f - glass_trans));
            }
            float3 base = amb + lit;
            /* Surface light leaving an in-medium hit must travel the
               already traversed segment back through the medium. */
            accum += base * (thru * Tseg);

            if (depth == MAX_DEPTH) break;

            float cos_i = dot(n_hit, rd);
            bool entering = cos_i < 0;
            float3 na = entering ? n_hit : -n_hit;
            cos_i = entering ? -cos_i : cos_i;

            float3 refl_d = reflect(rd, na);
            float3 refl_o = p + refl_d * EPS;

            if (mat == MAT_PLASTIC || mat == MAT_METALLIC) {
                /* Unified PBR mirror: keep tracing the reflection ray,
                   tinting throughput per-channel by F0 (the old
                   basecolor-tinted metal mirror at metallic=1). */
                ro = refl_o;
                rd = refl_d;
                thru = (thru * Tseg) * f0u;
                sr = srough;   /* this surface scatters: env-escape blurs by its roughness */
                continue;
            }

            /* Hit geometry (see the CPU twin): on a mesh face the
               `entering` flag is ALWAYS true (the returned normal is
               flipped to face the ray), so the legacy n1/n2 read every mesh
               crossing with eta = 1/ior — correct at the front, wrong at
               the back, where physics wants eta = ior.  The wrong back eta
               bends the exit toward the normal like an entry and
               reconverges inside the shell (the in-glass walk).  The
               pre-flip stored-normal sign (side_entry) is the true
               front/back test.  On the mesh na is the flipped normal
               itself — the correct refraction normal for both crossings —
               so n_refr keeps its value in all cases.  Spheres are
               untouched (their n1/n2 is the genuine state test). */
            float3 n_refr = na;
            float eta;
            if (hit_type == 2) {
                if (side_entry) {
                    eta = 1.0f / sior;
                } else {
                    n_refr = n_hit;
                    eta = sior;
                }
            } else {
                float n1 = entering ? 1.0f : sior;
                float n2 = entering ? sior : 1.0f;
                eta = n1 / n2;
            }
            float k = 1.0f - eta * eta * (1.0f - cos_i * cos_i);

            float r0 = (1.0f - sior) / (1.0f + sior);
            r0 = r0 * r0;
            float fresnel = r0 + (1.0f - r0) * pow(1.0f - cos_i, 5.0f);

            /* Iridescence: the film response replaces the glass surface weight
               per channel; the transmitted term is the clamped complement so
               the blend stays energy-consistent.  Without a film these reduce
               to the scalar weights exactly. */
            float3 wr = float3(fresnel) * sref;
            float3 wt = float3(1.0f - fresnel);
            if (film_w > 0.0f) {
                wr = wr * (1.0f - film_w) + film * film_w;
                wt = max(float3(0.0f), float3(1.0f) - wr);
            }

            if (k > 0) {
                float cos_t = sqrt(k);
                float3 refr_d = rd * eta + n_refr * (eta * cos_i - cos_t);
                float3 refr_o = p + refr_d * EPS;

                /* Seam-hit avoidance for mesh-glass entry: an origin
                   offset along the refracted direction alone leaves the
                   ray glued to the entry seam, where a neighbor back face
                   produces a spurious surface hit ~1e-4 in — for a volume
                   this falsely "exits" the medium before the real chord is
                   charged, and for plain glass it yields a spurious second
                   refraction.  Push along the (flipped) normal instead —
                   straight into the surface.  Universal for ALL mesh glass
                   entry (not gated on absorption), matching the universal
                   corrected eta/n_refr. */
                int mside_mid = tris[mi].mesh_idx;
                if (hit_type == 2 && side_entry && mside_mid >= 0 && mside_mid < nm)
                    refr_o = refr_o + na * EPS;

                float3 cur_thru = thru * Tseg;

                /* Downstream medium for the refracted ray: a volume
                   boundary is a closed convex shell; crossing its front
                   face lands inside the volume (medium = this material),
                   crossing the back face exits it (air).  Otherwise the
                   incoming medium carries through (homogeneous).
                   INTENTIONALLY gated on vol_th > 0 — only volumes carry a
                   medium; the corrected eta/n_refr and the origin push
                   above apply to ALL mesh glass. */
                float4 mid_rc = stk_md[stk];
                float mid_dd = stk_ma[stk];
                int mm = tris[mi].mesh_idx;
                if (hit_type == 2 && mm >= 0 && mm < nm &&
                    mats[mm].vol_th > 0.0f) {
                    if (side_entry) {
                        mid_rc = float4(sior, mats[mm].att_r, mats[mm].att_g, mats[mm].att_b);
                        mid_dd = mats[mm].att_dist;
                    } else {
                        mid_rc = float4(1.0f, 1.0f, 1.0f, 1.0f);
                        mid_dd = INFINITY;
                    }
                }

                if (stk < MAX_DEPTH + 2) {
                    stk_o[stk] = (packed_float3)refr_o;
                    stk_d[stk] = (packed_float3)refr_d;
                    stk_th[stk] = (packed_float3)(cur_thru * wt * sc_col);
                    stk_md[stk] = mid_rc;
                    stk_ma[stk] = mid_dd;
                    stk_dp[stk] = depth + 1;
                    stk_sr[stk] = srough;
                    stk++;
                }

                ro = refl_o;
                rd = refl_d;
                thru = cur_thru * wr;
                sr = srough;
            } else {
                ro = refl_o;
                rd = refl_d;
                thru = (thru * Tseg) * wr;
                sr = srough;
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
    const device TexBundle& scene_tex [[buffer(10)]],
    device const float* env_mip [[buffer(12)]],
    uint2 tid [[thread_position_in_grid]],
    uint2 grid [[threads_per_grid]]
) {
    int x = tid.x, y = tid.y;
    if (x >= scene.width || y >= scene.height) return;

    float3 fwd = normalize(cam.target - cam.pos);
    float3 world_up = float3(0.0f, 1.0f, 0.0f);
    if (fabs(dot(world_up, fwd)) > 0.999f)
        world_up = float3(0.0f, 0.0f, 1.0f);
    float3 right = normalize(cross(world_up, fwd));
    float3 up = cross(fwd, right);
    float asp = (float)scene.width / (float)scene.height;

    float3 sum = float3(0.0f);
    for (int sy = 0; sy < AA_SAMPLES; sy++) {
        for (int sx = 0; sx < AA_SAMPLES; sx++) {
            int sidx = sy * AA_SAMPLES + sx;
            float ux = (2.0f * (x + (sx + 0.5f) / AA_SAMPLES) / scene.width - 1.0f) * asp * scene.fov_scale;
            float uy = (1.0f - 2.0f * (y + (sy + 0.5f) / AA_SAMPLES) / scene.height) * scene.fov_scale;
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
                                sidx, scene.num_textures,
                                env_tex, scene.has_env, scene.sh, scene.env_mips,
                                env_mip, scene.env_w, scene.env_h,
                                scene_tex,
                                scene.has_floor, scene.has_bg_color,
                                scene.bg_r, scene.bg_g, scene.bg_b);
        }
    }
    float3 final = sum / (float)(AA_SAMPLES * AA_SAMPLES);
    out[y * scene.width + x] = tone_map(final, scene.exposure);
}
