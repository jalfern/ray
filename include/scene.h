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
} MeshObj;

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
    Vec3 light_pos;
    Sphere* spheres;
    int num_spheres;
    MeshObj* meshes;
    int num_meshes;
    int width;
    int height;
    int has_floor;
    char output[256];
    int has_animation;
    AnimationConfig animation;
} Scene;

#endif
