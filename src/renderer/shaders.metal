#include <metal_stdlib>
using namespace metal;

constant float EPS = 1e-4f;
constant int AA_SAMPLES = 4;
constant int MAX_DEPTH = 4;

struct SphereGpu {
    packed_float3 c;
    float r;
    float ref;
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
    int width;
    int height;
};

static bool hit_sphere(float3 o, float3 d, float3 c, float r, thread float& t) {
    float3 oc = o - c;
    float a = dot(d, d);
    float b = 2.0f * dot(oc, d);
    float cc = dot(oc, oc) - r*r;
    float delta = b*b - 4.0f*a*cc;
    if (delta < 0.0f) return false;
    t = (-b - sqrt(delta)) / (2.0f * a);
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
            best = ti;
            hit = true;
            idx = i;
        }
    }
    if (hit) {
        t = best;
        n = normalize(o + d * best - spheres[idx].c);
    }
    return hit;
}

static bool hit_floor(float3 o, float3 d, thread float& t) {
    if (fabs(d.y) < EPS) return false;
    t = -o.y / d.y;
    return t > EPS;
}

static float3 floor_color(float3 p) {
    int ix = (int)floor(p.x);
    int iz = (int)floor(p.z);
    return ((ix + iz) & 1) ? float3(0.08f, 0.12f, 0.25f) : float3(0.25f, 0.4f, 0.7f);
}

static bool in_shadow(float3 p, float3 lp, device const SphereGpu* spheres, int count, int origin) {
    float3 to_light = lp - p;
    float light_dist = length(to_light);
    float3 dir = normalize(to_light);
    float3 o = p + dir * EPS;
    for (int i = 0; i < count; i++) {
        if (i == origin) continue;
        float t;
        if (hit_sphere(o, dir, spheres[i].c, spheres[i].r, t) && t < light_dist && t > EPS)
            return true;
    }
    return false;
}

static float3 trace_ray(float3 o, float3 d, device const SphereGpu* spheres, int count, float3 lp) {
    float3 accum = float3(0.0f);
    float3 thru = float3(1.0f);
    float3 ro = o, rd = d;

    for (int depth = 0; depth <= MAX_DEPTH; depth++) {
        float ts, tf;
        float3 n;
        int idx = -1;

        bool hs = hit_any_sphere(ro, rd, ts, n, idx, spheres, count);
        bool hf = hit_floor(ro, rd, tf);

        if (hs && (!hf || ts < tf)) {
            float3 p = ro + rd * ts;
            int is_plastic = (spheres[idx].mat_type == 1);
            float3 sc = spheres[idx].col;

            float3 to_light = lp - p;
            float3 ld = normalize(to_light);
            bool shadowed = in_shadow(p, lp, spheres, count, idx);

            float diff = max(0.0f, dot(n, ld));
            float3 view = normalize(ro - p);
            float3 half_vec = normalize(ld + view);
            float spec = pow(max(0.0f, dot(n, half_vec)), is_plastic ? 32.0f : 64.0f);

            float reflectivity = spheres[idx].ref;
            float3 ambient = sc * 0.15f;
            float spec_str = is_plastic ? 0.4f : 0.8f;
            float lf = shadowed ? 0.0f : 1.0f;
            float3 base = ambient + sc * diff * lf;
            base += sc * spec * spec_str * lf;

            accum += base * thru;

            if (is_plastic || depth == MAX_DEPTH) break;

            thru *= reflectivity;
            rd = reflect(rd, n);
            ro = p + rd * EPS;

        } else if (hf) {
            float3 p = ro + rd * tf;
            float3 nf = float3(0.0f, 1.0f, 0.0f);
            float3 to_light = lp - p;
            float3 ld = normalize(to_light);
            bool shadowed = in_shadow(p, lp, spheres, count, -1);
            float3 base = floor_color(p);
            float diff = max(0.0f, dot(nf, ld));
            float lf = shadowed ? 0.2f : 1.0f;
            accum += (base * 0.15f + base * diff * lf) * thru;
            break;
        } else {
            accum += float3(0.1f, 0.1f, 0.2f) * thru;
            break;
        }
    }
    return accum;
}

kernel void raytrace_kernel(
    device float3* output [[buffer(0)]],
    constant CameraGpu& cam [[buffer(1)]],
    constant SceneGpu& scene [[buffer(2)]],
    device const SphereGpu* spheres [[buffer(3)]],
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
            sum += trace_ray(cam.pos, rd, spheres, scene.num_spheres, scene.light_pos);
        }
    }

    output[y * scene.width + x] = sum / (float)(AA_SAMPLES * AA_SAMPLES);
}
