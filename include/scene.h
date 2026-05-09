#ifndef SCENE_H
#define SCENE_H

#include "mesh.h"

typedef struct {
    float x, y, z;
} Vec3;

typedef struct {
    Vec3 pos;
    float radius;
    float reflectivity;
    float ior;
    Vec3 color;
    char material[16];
    int tex_type;
    float tex_scale;
    Vec3 tex_color2;
} Sphere;

typedef struct {
    TriGpu* tris;
    int num_tris;
    Vec3 pos;
    float scale;
    float reflectivity;
    float ior;
    Vec3 color;
    char material[16];
    int tex_type;
    float tex_scale;
    Vec3 tex_color2;
} MeshObj;

typedef struct {
    Vec3 pos;
    float size;
} Light;

typedef struct {
    float duration;
    int fps;
    Vec3 orbit_center;
    float orbit_radius;
    float orbit_height;
} AnimationConfig;

typedef struct {
    Vec3 camera_pos;
    Vec3 camera_target;
    float aperture;
    float focus_dist;
    Light* lights;
    int num_lights;
    Sphere* spheres;
    int num_spheres;
    MeshObj* meshes;
    int num_meshes;
    int width;
    int height;
    float exposure;
    int has_floor;
    char output[256];
    int has_animation;
    AnimationConfig animation;
} Scene;

#endif
