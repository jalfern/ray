#include <stdio.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// Include scene parser (no main function in parse_scene.c)
#include "parse_scene.c"

#define EPS 1e-4f
#define AA_SAMPLES 4

typedef struct { float x, y, z; } V;

// Vector operations
V add(V a, V b) { return (V){a.x+b.x, a.y+b.y, a.z+b.z}; }
V sub(V a, V b) { return (V){a.x-b.x, a.y-b.y, a.z-b.z}; }
V mul(V a, float s) { return (V){a.x*s, a.y*s, a.z*s}; }
V norm(V a) { 
    float l = sqrtf(a.x*a.x + a.y*a.y + a.z*a.z); 
    return (l > EPS) ? mul(a, 1.0f/l) : (V){0,0,0}; 
}
float dot(V a, V b) { return a.x*b.x + a.y*b.y + a.z*b.z; }
V cross(V a, V b) { return (V){a.y*b.z - a.z*b.y, a.z*b.x - a.x*b.z, a.x*b.y - a.y*b.x}; }

typedef struct { V c; float r; float ref; } SphereData;

// Sphere intersection
int hit_sphere(V o, V d, V c, float r, float *t) {
    V oc = sub(o, c);
    float a = dot(d, d);
    float b = 2.0f * dot(oc, d);
    float cc = dot(oc, oc) - r*r;
    float delta = b*b - 4*a*cc;
    if (delta < 0) return 0;
    *t = (-b - sqrtf(delta)) / (2.0f * a);
    return *t > EPS;
}

// Hit any sphere from scene
int hit_any_sphere(V o, V d, float *t, V *hit_normal, SphereData* spheres, int num_spheres) {
    float best_t = 1e9f;
    int hit = 0;
    int hit_idx = -1;
    
    for (int i = 0; i < num_spheres; i++) {
        float t_i;
        if (hit_sphere(o, d, spheres[i].c, spheres[i].r, &t_i) && t_i < best_t) {
            best_t = t_i;
            hit = 1;
            hit_idx = i;
        }
    }
    
    if (hit) {
        *t = best_t;
        V p = add(o, mul(d, best_t));
        *hit_normal = norm(sub(p, spheres[hit_idx].c));
    }
    
    return hit;
}

// Floor intersection
int hit_floor(V o, V d, float *t) {
    if (fabsf(d.y) < EPS) return 0;
    *t = -o.y / d.y;
    return *t > EPS;
}

// Three shades of blue for checkerboard
V floor_color(V p) {
    int ix = (int)floorf(p.x);
    int iz = (int)floorf(p.z);
    int sum = ix + iz;
    int shade = sum % 3;
    if (shade == 0) return (V){0.08f, 0.12f, 0.25f};
    else if (shade == 1) return (V){0.25f, 0.4f, 0.7f};
    else return (V){0.5f, 0.65f, 0.95f};
}

// Trace a single ray
V trace_ray(V o, V d, int depth, SphereData* spheres, int num_spheres, V light_dir) {
    if (depth > 4) return (V){0,0,0};

    float t_s, t_f;
    V hit_normal;
    int hs = hit_any_sphere(o, d, &t_s, &hit_normal, spheres, num_spheres);
    int hf = hit_floor(o, d, &t_f);

    V color;

    if (hs && (!hf || t_s < t_f)) {
        V p = add(o, mul(d, t_s));
        V n = hit_normal;
        
        // Specular highlight (Blinn-Phong)
        V view = norm(sub(o, p));
        V half = norm(add(light_dir, view));
        float spec = powf(fmaxf(0.0f, dot(n, half)), 64.0f);
        
        // Reflection
        V refl = sub(d, mul(n, 2.0f * dot(d, n)));
        V rp = add(p, mul(refl, EPS));
        V refl_color = trace_ray(rp, refl, depth + 1, spheres, num_spheres, light_dir);
        
        // Find this sphere's reflectivity
        float reflectivity = 0.7f;
        for (int i = 0; i < num_spheres; i++) {
            if (hit_sphere(add(o,mul(d,t_s-EPS)), d, spheres[i].c, spheres[i].r, &t_s)) {
                reflectivity = spheres[i].ref;
                break;
            }
        }
        
        V ambient = (V){0.1f, 0.1f, 0.15f};
        V spec_color = (V){1.0f, 1.0f, 1.0f};
        color = add(ambient, mul(spec_color, spec * 0.8f));
        color = add(color, mul(refl_color, reflectivity));
        
    } else if (hf) {
        V p = add(o, mul(d, t_f));
        color = floor_color(p);
    } else {
        color = (V){0.05f, 0.05f, 0.1f};
    }

    return color;
}

int main(int argc, char** argv) {
    const char* scene_file = "scene.json";
    if (argc > 1) scene_file = argv[1];
    
    Scene scene = {0};
    scene.width = 400;
    scene.height = 400;
    
    if (parse_scene(scene_file, &scene) != 0) {
        fprintf(stderr, "Failed to parse scene file: %s\n", scene_file);
        return 1;
    }
    
    // Convert Vec3 to V
    V cam = (V){scene.camera_pos.x, scene.camera_pos.y, scene.camera_pos.z};
    V tgt = (V){scene.camera_target.x, scene.camera_target.y, scene.camera_target.z};
    V light_dir = norm((V){scene.light_pos.x, scene.light_pos.y, scene.light_pos.z});
    
    // Prepare spheres
    SphereData* spheres = malloc(scene.num_spheres * sizeof(SphereData));
    for (int i = 0; i < scene.num_spheres; i++) {
        spheres[i].c = (V){scene.spheres[i].pos.x, scene.spheres[i].pos.y, scene.spheres[i].pos.z};
        spheres[i].r = scene.spheres[i].radius;
        spheres[i].ref = scene.spheres[i].reflectivity;
    }
    
    V fwd = norm(sub(tgt, cam));
    V right = norm(cross((V){0,1,0}, fwd));
    V up = cross(fwd, right);
    float asp = (float)scene.width / scene.height;

    // Output PPM header
    printf("P6\n%d %d\n255\n", scene.width, scene.height);

    for (int y = 0; y < scene.height; y++) {
        for (int x = 0; x < scene.width; x++) {
            V color_sum = {0, 0, 0};
            int sample_count = 0;

            for (int sy = 0; sy < AA_SAMPLES; sy++) {
                for (int sx = 0; sx < AA_SAMPLES; sx++) {
                    float sample_x = (float)(sx + 0.5f) / AA_SAMPLES;
                    float sample_y = (float)(sy + 0.5f) / AA_SAMPLES;
                    float uv_x = (2.0f*(x + sample_x)/scene.width - 1.0f) * asp;
                    float uv_y = 1.0f - 2.0f*(y + sample_y)/scene.height;
                    V ray = norm(add(add(fwd, mul(right, uv_x)), mul(up, uv_y)));

                    V color = trace_ray(cam, ray, 0, spheres, scene.num_spheres, light_dir);
                    color_sum = add(color_sum, color);
                    sample_count++;
                }
            }

            V color_avg = mul(color_sum, 1.0f/sample_count);

            uint8_t cr = (uint8_t)(fminf(fmaxf(color_avg.x, 0.0f), 1.0f) * 255.0f);
            uint8_t cg = (uint8_t)(fminf(fmaxf(color_avg.y, 0.0f), 1.0f) * 255.0f);
            uint8_t cb = (uint8_t)(fminf(fmaxf(color_avg.z, 0.0f), 1.0f) * 255.0f);
            fputc(cr, stdout); fputc(cg, stdout); fputc(cb, stdout);
        }
    }
    
    free(spheres);
    free(scene.spheres);
    return 0;
}
