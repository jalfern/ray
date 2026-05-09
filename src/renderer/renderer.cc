#include "renderer.h"
#include "../vector/vector.h"
#include "../shading/shading.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <thread>
#include <vector>

#define EPS 1e-4f
#define AA_SAMPLES 4
#define MAX_DEPTH 4

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

// Möller–Trumbore ray-triangle intersection
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

static int hit_mesh(V o, V d, float *t, V *hit_normal,
                    TriGpu* tris, int num_tris, int mesh_idx) {
    float best_t = 1e9f;
    int hit = 0;
    float best_u = 0, best_v = 0;
    TriGpu* best_tri = NULL;
    for (int i = 0; i < num_tris; i++) {
        float ti, u, v;
        if (hit_tri(o, d, tris[i].v0, tris[i].v1, tris[i].v2, &ti, &u, &v) && ti < best_t) {
            best_t = ti; hit = 1; best_tri = &tris[i]; best_u = u; best_v = v;
        }
    }
    if (hit) {
        *t = best_t;
        // Interpolate normal
        float w = 1.0f - best_u - best_v;
        float nx = w * best_tri->n0[0] + best_u * best_tri->n1[0] + best_v * best_tri->n2[0];
        float ny = w * best_tri->n0[1] + best_u * best_tri->n1[1] + best_v * best_tri->n2[1];
        float nz = w * best_tri->n0[2] + best_u * best_tri->n1[2] + best_v * best_tri->n2[2];
        float len = sqrtf(nx*nx + ny*ny + nz*nz);
        if (len > EPS) { nx /= len; ny /= len; nz /= len; }
        *hit_normal = (V){nx, ny, nz};
        if (dot(*hit_normal, d) > 0)
            *hit_normal = mul(*hit_normal, -1);
    }
    return hit;
}

static int hit_floor(V o, V d, float *t) {
    if (fabsf(d.y) < EPS) return 0;
    *t = -o.y / d.y;
    return *t > EPS;
}

static int in_shadow(V p, V light_pos, SphereData* spheres, int num_spheres,
                     MeshObjData* meshes, int num_meshes, int skip_sphere) {
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
        TriGpu* tris = meshes[m].tris;
        int nt = meshes[m].num_tris;
        for (int i = 0; i < nt; i++) {
            float t_hit, u, v;
            if (hit_tri(ray_o, ray_dir, tris[i].v0, tris[i].v1, tris[i].v2, &t_hit, &u, &v)) {
                if (t_hit < light_dist && t_hit > EPS) return 1;
            }
        }
    }
    return 0;
}

static V trace_ray(V o, V d, int depth, SphereData* spheres, int num_spheres,
                   MeshObjData* meshes, int num_meshes, V light_pos, int inside_idx) {
    if (depth > MAX_DEPTH) return (V){0,0,0};

    float ts, tf;
    V sn;
    int si = -1;
    int hs = hit_any_sphere(o, d, &ts, &sn, &si, spheres, num_spheres);

    // Mesh intersection
    float tm = 1e9f;
    V mn = {0,0,0};
    int mi = -1;
    for (int i = 0; i < num_meshes; i++) {
        V hit_n;
        float tmi;
        if (hit_mesh(o, d, &tmi, &hit_n, meshes[i].tris, meshes[i].num_tris, i) && tmi < tm) {
            tm = tmi; mi = i; mn = hit_n;
        }
    }
    int hm = (mi >= 0);

    int hf = hit_floor(o, d, &tf);

    // Find closest hit
    int hit_type = 0; // 0=none, 1=sphere, 2=mesh, 3=floor
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
        V n = (V){0, 1, 0};
        V to_light = sub(light_pos, p);
        V light_dir = norm(to_light);
        int shadowed = in_shadow(p, light_pos, spheres, num_spheres,
                                 meshes, num_meshes, -1);
        V base = floor_color(p);
        float diff = fmaxf(0.0f, dot(n, light_dir));
        float light_factor = shadowed ? 0.2f : 1.0f;
        return add(mul(base, 0.15f), mul(base, diff * light_factor));
    }

    // Sphere or mesh hit - use hit_n (always points toward incident ray)
    V n = hit_n;
    V sc = (V){sphere_col[0], sphere_col[1], sphere_col[2]};
    int is_plastic = (sphere_mat == 1);

    V to_light = sub(light_pos, p);
    V light_dir = norm(to_light);
    int shadowed = in_shadow(p, light_pos, spheres, num_spheres,
                             meshes, num_meshes,
                             hit_type == 1 ? si : -1);

    float diff = fmaxf(0.0f, dot(n, light_dir));
    V view = norm(sub(o, p));
    V half = norm(add(light_dir, view));
    float spec = powf(fmaxf(0.0f, dot(n, half)), is_plastic ? 32.0f : 64.0f);

    V ambient = mul(sc, 0.15f);
    float spec_strength = is_plastic ? 0.4f : 0.8f;
    float light_factor = shadowed ? 0.0f : 1.0f;
    V base_color = add(ambient, mul(sc, diff * light_factor));
    base_color = add(base_color, mul(sc, spec * spec_strength * light_factor));

    if (is_plastic) return base_color;

    // Glass: reflection + refraction with Fresnel
    float reflectivity = sphere_ref;
    float ior = sphere_ior;
    float cos_i = dot(n, d);
    int entering = cos_i < 0;
    V n_adj = entering ? n : mul(n, -1); // always toward incident ray
    cos_i = entering ? -cos_i : cos_i;   // always positive

    // Reflection
    V refl_dir = sub(d, mul(n_adj, 2.0f * dot(d, n_adj)));
    V refl_origin = add(p, mul(refl_dir, EPS));
    V refl_col = trace_ray(refl_origin, refl_dir, depth + 1,
                           spheres, num_spheres, meshes, num_meshes, light_pos, -1);

    // Refraction (Snell's law)
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
                             spheres, num_spheres, meshes, num_meshes, light_pos,
                             hit_type == 1 ? si : -1);
        // Beer's law absorption (color tint transmission)
        refr_col = (V){refr_col.x * sc.x, refr_col.y * sc.y, refr_col.z * sc.z};
    }

    // Schlick Fresnel approximation
    float r0 = (1.0f - ior) / (1.0f + ior);
    r0 = r0 * r0;
    float fresnel = r0 + (1.0f - r0) * powf(1.0f - cos_i, 5.0f);

    return add(mul(refl_col, fresnel * reflectivity), mul(refr_col, 1.0f - fresnel));
}

typedef struct {
    V cam, fwd, right, up;
    float asp;
    SphereData* spheres;
    int num_spheres;
    MeshObjData* meshes;
    int num_meshes;
    V light_pos;
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
                    float sample_x = (float)(sx + 0.5f) / AA_SAMPLES;
                    float sample_y = (float)(sy + 0.5f) / AA_SAMPLES;
                    float uv_x = (2.0f*(x + sample_x)/ctx->width - 1.0f) * ctx->asp;
                    float uv_y = 1.0f - 2.0f*(y + sample_y)/ctx->height;
                    V ray = norm(add(add(ctx->fwd, mul(ctx->right, uv_x)), mul(ctx->up, uv_y)));

                    V color = trace_ray(ctx->cam, ray, 0, ctx->spheres, ctx->num_spheres,
                                        ctx->meshes, ctx->num_meshes, ctx->light_pos, -1);
                    color_sum = add(color_sum, color);
                    sample_count++;
                }
            }

            V color_avg = mul(color_sum, 1.0f/sample_count);

            size_t idx = (y * ctx->width + x) * 3;
            ctx->img->data[idx]   = (uint8_t)(fminf(fmaxf(color_avg.x, 0.0f), 1.0f) * 255.0f);
            ctx->img->data[idx+1] = (uint8_t)(fminf(fmaxf(color_avg.y, 0.0f), 1.0f) * 255.0f);
            ctx->img->data[idx+2] = (uint8_t)(fminf(fmaxf(color_avg.z, 0.0f), 1.0f) * 255.0f);
        }
    }
}

static RenderContext setup_context(const Scene* scene) {
    RenderContext ctx;
    ctx.cam = (V){scene->camera_pos.x, scene->camera_pos.y, scene->camera_pos.z};
    V tgt = (V){scene->camera_target.x, scene->camera_target.y, scene->camera_target.z};
    ctx.light_pos = (V){scene->light_pos.x, scene->light_pos.y, scene->light_pos.z};

    ctx.spheres = (SphereData*)malloc(scene->num_spheres * sizeof(SphereData));
    ctx.num_spheres = scene->num_spheres;
    for (int i = 0; i < scene->num_spheres; i++) {
        ctx.spheres[i].c = (V){scene->spheres[i].pos.x, scene->spheres[i].pos.y, scene->spheres[i].pos.z};
        ctx.spheres[i].r = scene->spheres[i].radius;
        ctx.spheres[i].ref = scene->spheres[i].reflectivity;
        ctx.spheres[i].ior = scene->spheres[i].ior;
        ctx.spheres[i].col = (V){scene->spheres[i].color.x, scene->spheres[i].color.y, scene->spheres[i].color.z};
        const char* mat = scene->spheres[i].material[0] ? scene->spheres[i].material : "glass";
        ctx.spheres[i].mat_type = (strcmp(mat, "plastic") == 0) ? 1 : 0;
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
            ctx.meshes[i].mat_type = (strcmp(mat, "plastic") == 0) ? 1 : 0;
        }
    }

    ctx.fwd = norm(sub(tgt, ctx.cam));
    ctx.right = norm(cross((V){0,1,0}, ctx.fwd));
    ctx.up = cross(ctx.fwd, ctx.right);
    ctx.asp = (float)scene->width / scene->height;
    ctx.width = scene->width;
    ctx.height = scene->height;
    ctx.img = create_image(scene->width, scene->height);
    return ctx;
}

Image* render_frame(const Scene* scene) {
    RenderContext ctx = setup_context(scene);
    render_rows(&ctx, 0, ctx.height);
    free(ctx.spheres);
    free(ctx.meshes);
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
    free(ctx.meshes);
    return ctx.img;
}
