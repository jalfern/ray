#include "renderer.h"
#include "../vector/vector.h"
#include "../shading/shading.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define EPS 1e-4f
#define AA_SAMPLES 4

// SphereData is defined in ../include/types.h

static int hit_sphere(V o, V d, V c, float r, float *t) {
    V oc = sub(o, c);
    float a = dot(d, d);
    float b = 2.0f * dot(oc, d);
    float cc = dot(oc, oc) - r*r;
    float delta = b*b - 4*a*cc;
    if (delta < 0) return 0;
    *t = (-b - sqrtf(delta)) / (2.0f * a);
    return *t > EPS;
}

static int hit_any_sphere(V o, V d, float *t, V *hit_normal, int *hit_idx, SphereData* spheres, int num_spheres) {
    float best_t = 1e9f;
    int hit = 0;
    *hit_idx = -1;
    
    for (int i = 0; i < num_spheres; i++) {
        float t_i;
        if (hit_sphere(o, d, spheres[i].c, spheres[i].r, &t_i) && t_i < best_t) {
            best_t = t_i;
            hit = 1;
            *hit_idx = i;
        }
    }
    
    if (hit) {
        *t = best_t;
        V p = add(o, mul(d, best_t));
        *hit_normal = norm(sub(p, spheres[*hit_idx].c));
    }
    
    return hit;
}

static int hit_floor(V o, V d, float *t) {
    if (fabsf(d.y) < EPS) return 0;
    *t = -o.y / d.y;
    return *t > EPS;
}

static int in_shadow(V p, V light_pos, SphereData* spheres, int num_spheres, int need_origin_idx) {
    V to_light = sub(light_pos, p);
    float light_dist = sqrtf(dot(to_light, to_light));
    V ray_dir = norm(to_light);
    V ray_o = add(p, mul(ray_dir, EPS));
    
    for (int i = 0; i < num_spheres; i++) {
        if (i == need_origin_idx) continue;
        float t_hit;
        if (hit_sphere(ray_o, ray_dir, spheres[i].c, spheres[i].r, &t_hit)) {
            if (t_hit < light_dist && t_hit > EPS) return 1;
        }
    }
    return 0;
}

static V trace_ray(V o, V d, int depth, SphereData* spheres, int num_spheres, V light_pos) {
    if (depth > 4) return (V){0,0,0};

    float t_s, t_f;
    V hit_normal;
    int hit_idx = -1;
    int hs = hit_any_sphere(o, d, &t_s, &hit_normal, &hit_idx, spheres, num_spheres);
    int hf = hit_floor(o, d, &t_f);

    V color;

    if (hs && (!hf || t_s < t_f)) {
        V p = add(o, mul(d, t_s));
        V n = hit_normal;
        V sphere_col = spheres[hit_idx].col;
        int is_plastic = (strcmp(spheres[hit_idx].mat, "plastic") == 0);
        
        V to_light = sub(light_pos, p);
        V light_dir = norm(to_light);
        int shadowed = in_shadow(p, light_pos, spheres, num_spheres, hit_idx);
        
        float diff = fmaxf(0.0f, dot(n, light_dir));
        V view = norm(sub(o, p));
        V half = norm(add(light_dir, view));
        float spec = powf(fmaxf(0.0f, dot(n, half)), is_plastic ? 32.0f : 64.0f);
        
        float reflectivity = spheres[hit_idx].ref;
        V ambient = mul(sphere_col, 0.15f);
        V spec_color = sphere_col;
        float spec_strength = is_plastic ? 0.4f : 0.8f;
        float light_factor = shadowed ? 0.0f : 1.0f;
        V base_color = add(ambient, mul(sphere_col, diff * light_factor));
        V contrib_spec = mul(spec_color, spec * spec_strength * light_factor);
        base_color = add(base_color, contrib_spec);
        
        if (!is_plastic) {
            V refl = sub(d, mul(n, 2.0f * dot(d, n)));
            V rp = add(p, mul(refl, EPS));
            V refl_color = trace_ray(rp, refl, depth + 1, spheres, num_spheres, light_pos);
            color = add(base_color, mul(refl_color, reflectivity));
        } else {
            color = base_color;
        }
        
    } else if (hf) {
        V p = add(o, mul(d, t_f));
        V n = (V){0, 1, 0};
        V to_light = sub(light_pos, p);
        V light_dir = norm(to_light);
        int shadowed = in_shadow(p, light_pos, spheres, num_spheres, -1);
        V base = floor_color(p);
        float diff = fmaxf(0.0f, dot(n, light_dir));
        float light_factor = shadowed ? 0.2f : 1.0f;
        color = add(mul(base, 0.15f), mul(base, diff * light_factor));
    } else {
        color = (V){0.1f, 0.1f, 0.2f};
    }

    return color;
}

Image* render_frame(const Scene* scene) {
    V cam = (V){scene->camera_pos.x, scene->camera_pos.y, scene->camera_pos.z};
    V tgt = (V){scene->camera_target.x, scene->camera_target.y, scene->camera_target.z};
    V light_pos = (V){scene->light_pos.x, scene->light_pos.y, scene->light_pos.z};
    
    SphereData* spheres = malloc(scene->num_spheres * sizeof(SphereData));
    for (int i = 0; i < scene->num_spheres; i++) {
        spheres[i].c = (V){scene->spheres[i].pos.x, scene->spheres[i].pos.y, scene->spheres[i].pos.z};
        spheres[i].r = scene->spheres[i].radius;
        spheres[i].ref = scene->spheres[i].reflectivity;
        spheres[i].col = (V){scene->spheres[i].color.x, scene->spheres[i].color.y, scene->spheres[i].color.z};
        strcpy(spheres[i].mat, scene->spheres[i].material[0] ? scene->spheres[i].material : "glass");
    }
    
    V fwd = norm(sub(tgt, cam));
    V right = norm(cross((V){0,1,0}, fwd));
    V up = cross(fwd, right);
    float asp = (float)scene->width / scene->height;

    Image* img = create_image(scene->width, scene->height);
    
    for (int y = 0; y < scene->height; y++) {
        for (int x = 0; x < scene->width; x++) {
            V color_sum = {0, 0, 0};
            int sample_count = 0;

            for (int sy = 0; sy < AA_SAMPLES; sy++) {
                for (int sx = 0; sx < AA_SAMPLES; sx++) {
                    float sample_x = (float)(sx + 0.5f) / AA_SAMPLES;
                    float sample_y = (float)(sy + 0.5f) / AA_SAMPLES;
                    float uv_x = (2.0f*(x + sample_x)/scene->width - 1.0f) * asp;
                    float uv_y = 1.0f - 2.0f*(y + sample_y)/scene->height;
                    V ray = norm(add(add(fwd, mul(right, uv_x)), mul(up, uv_y)));

                    V color = trace_ray(cam, ray, 0, spheres, scene->num_spheres, light_pos);
                    color_sum = add(color_sum, color);
                    sample_count++;
                }
            }

            V color_avg = mul(color_sum, 1.0f/sample_count);

            size_t idx = (y * scene->width + x) * 3;
            img->data[idx]   = (uint8_t)(fminf(fmaxf(color_avg.x, 0.0f), 1.0f) * 255.0f);
            img->data[idx+1] = (uint8_t)(fminf(fmaxf(color_avg.y, 0.0f), 1.0f) * 255.0f);
            img->data[idx+2] = (uint8_t)(fminf(fmaxf(color_avg.z, 0.0f), 1.0f) * 255.0f);
        }
    }
    
    free(spheres);
    return img;
}
